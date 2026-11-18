#include "CompStageAlign.h"

#include <algorithm>
#include <cmath>

namespace duskstudio
{
namespace
{
constexpr double kRampSeconds = 0.020;

// A fade's weight at sample i of the block: `at` before the block, moved one
// step a sample in `dir`, held to 0..ramp. Every member computes it from the
// same integers, so members of one stage fade on the same samples.
inline float rampWeight (int at, int dir, int i, int ramp, float invRamp) noexcept
{
    return (float) std::clamp (at + dir * (i + 1), 0, ramp) * invRamp;
}
} // namespace

void CompStageClock::prepare (double sampleRate, int stageLatency) noexcept
{
    latency = std::max (0, stageLatency);
    const double sr = std::isfinite (sampleRate) && sampleRate > 0.0 ? sampleRate : 48000.0;
    ramp = std::max (1, (int) std::lround (sr * kRampSeconds));
    state = State::Off;
    pos = 0;
    filled = 0;
    wantOn = false;
    emit();
    reported.store (0, std::memory_order_relaxed);
}

void CompStageClock::snap (bool engaged) noexcept
{
    const bool on = engaged && latency > 0;
    state = on ? State::On : State::Off;
    pos = on ? ramp : 0;
    filled = on ? latency : 0;
    wantOn = on;
    ++snaps;
    emit();
    reported.store (latencySamples(), std::memory_order_relaxed);
}

void CompStageClock::advance (bool anyEngaged, bool transportStopped, int numSamples) noexcept
{
    if (latency > 0)
    {
        if (anyEngaged)            wantOn = true;
        else if (transportStopped) wantOn = false;

        switch (state)
        {
            case State::Off:
                if (wantOn) { state = State::Filling; filled = 0; }
                break;
            case State::Filling:
                // The delayed path fades in only once the delay holds a whole
                // latency of input.
                if (! wantOn)                 state = State::Off;
                else if (filled >= latency)   state = State::Rising;
                break;
            case State::Rising:
            case State::On:
                if (! wantOn) state = State::Falling;
                break;
            case State::Falling:
                if (wantOn) state = State::Rising;
                break;
        }
    }

    ++serial;
    emit();

    const int n = std::max (0, numSamples);
    switch (state)
    {
        case State::Filling:
            filled = std::min (latency, filled + n);
            break;
        case State::Rising:
            pos = std::min (ramp, pos + n);
            if (pos == ramp) state = State::On;
            break;
        case State::Falling:
            pos = std::max (0, pos - n);
            if (pos == 0) state = State::Off;
            break;
        case State::Off:
        case State::On:
            break;
    }
    reported.store (latencySamples(), std::memory_order_relaxed);
}

void CompStageClock::emit() noexcept
{
    current.latency     = latency;
    current.write       = state != State::Off;
    current.rampSamples = ramp;
    current.alignAt     = pos;
    current.alignDir    = state == State::Rising ? 1 : (state == State::Falling ? -1 : 0);
    current.serial      = serial;
    current.snap        = snaps;
    current.cut         = cuts;
}

void CompStageAlign::prepare (int blockSize, int maxLatency)
{
    maxBlock = std::max (1, blockSize);
    maxRingLatency = std::max (0, maxLatency);
    int size = 2;
    while (size < maxRingLatency + 1)
        size <<= 1;
    ringL.assign ((std::size_t) size, 0.0f);
    ringR.assign ((std::size_t) size, 0.0f);
    mask = size - 1;
    writeAt = 0;
    ringLive = false;
    for (auto* v : { &dryL, &dryR, &wetL, &wetR })
        v->assign ((std::size_t) maxBlock, 0.0f);

    // The owner prepares its compressors first, which leaves them at rest.
    running = nullptr;
    runSamples = 0;
    mixAt = 0;
    mixDir = 0;
    heldLatency = 0;
    resync = true;
    dropHeld = false;
}

void CompStageAlign::discard() noexcept
{
    stop();
    dropHeld = true;
}

int CompStageAlign::tailSamples() const noexcept
{
    // The plug-in's anti-alias filters reach past its latency; twice it covers
    // them, and more than covers the plain delay.
    return ringLive || running != nullptr ? 2 * heldLatency : 0;
}

void CompStageAlign::stop() noexcept
{
    if (running != nullptr)
    {
        running->reset();
        running = nullptr;
    }
    runSamples = 0;
}

void CompStageAlign::clearRing() noexcept
{
    std::fill (ringL.begin(), ringL.end(), 0.0f);
    std::fill (ringR.begin(), ringR.end(), 0.0f);
    writeAt = 0;
    ringLive = true;
}

// A mono block feeds both sides, so a flip to stereo finds a history on R.
void CompStageAlign::push (const float* L, const float* R, int n) noexcept
{
    int w = writeAt;
    for (int i = 0; i < n; ++i)
    {
        ringL[(std::size_t) w] = L[i];
        ringR[(std::size_t) w] = R != nullptr ? R[i] : L[i];
        w = (w + 1) & mask;
    }
    writeAt = w;
}

// outL / outR may be L / R: each input sample is stored before its delayed
// partner is read, and the latency is never 0 here.
void CompStageAlign::pushAndRead (const float* L, const float* R, float* outL, float* outR,
                                  int n, int latency) noexcept
{
    int w = writeAt;
    for (int i = 0; i < n; ++i)
    {
        const float l = L[i];
        const float r = R != nullptr ? R[i] : l;
        ringL[(std::size_t) w] = l;
        ringR[(std::size_t) w] = r;
        const auto read = (std::size_t) ((w - latency) & mask);
        outL[i] = ringL[read];
        if (outR != nullptr) outR[i] = ringR[read];
        w = (w + 1) & mask;
    }
    writeAt = w;
}

void CompStageAlign::process (float* L, float* R, int numSamples, const CompStageBlock& stage,
                              bool engaged, CompStageProcessor* compressor) noexcept
{
    if (numSamples <= 0 || L == nullptr) return;

    const int latency = stage.latency;
    if (latency <= 0 || latency > maxRingLatency)
    {
        // A stage without a latency (no donor DSP), or one longer than this
        // member was prepared for: nothing to align, nothing runs.
        stop();
        mixAt = mixDir = 0;
        ringLive = false;
        heldLatency = 0;
        return;
    }

    // A serial that moved on by more than one block is a block this member
    // did not run: it was asleep on silence or cut, so its delay holds nothing
    // to replay, and a compressor that drained into that silence keeps its
    // state. One that stayed put is a stage nobody advances (a fixed one),
    // which is no gap either. A cut drops the compressor's state as well.
    const bool snapped = resync || stage.snap != lastSnap;
    const bool cut = dropHeld || stage.cut != lastCut;
    const bool gap = snapped || cut || (std::uint32_t) (stage.serial - lastSerial) > 1u;
    resync = dropHeld = false;
    lastSnap = stage.snap;
    lastCut = stage.cut;
    lastSerial = stage.serial;
    heldLatency = latency;

    if (! stage.write)
    {
        // The stage's latency is off: the member costs nothing, and its delay
        // holds nothing it may replay when the latency returns.
        stop();
        mixAt = mixDir = 0;
        ringLive = false;
        return;
    }

    if (gap || ! ringLive) clearRing();
    if (snapped || cut) stop();

    const bool wantsCompressor = engaged && compressor != nullptr;
    if (running != nullptr && running != compressor)
    {
        // The strip's width flipped: the other layout starts from rest, and
        // the member plays its delayed dry until that one is ready.
        stop();
        mixAt = 0;
    }
    if (gap)
    {
        // Nothing the member held is heard any more (it was skipped, cut or
        // the stage just snapped), so its engage fade goes straight to where
        // it is heading. A compressor started here runs from rest, as the
        // cleared delay beside it does, and both play the same silence first.
        if (wantsCompressor)
        {
            if (running == nullptr) running = compressor;
            runSamples = latency;
            mixAt = stage.rampSamples;
        }
        else
        {
            stop();
            mixAt = 0;
        }
    }

    if (wantsCompressor)
    {
        // A compressor starts from rest beside the delayed dry and fades in
        // once its own delay is full, and, on the first one engaged, once the
        // stage's latency is in.
        if (running == nullptr)
        {
            running = compressor;
            runSamples = 0;
        }
        const bool ready = runSamples >= latency && (mixAt > 0 || stage.settledOn());
        mixDir = ready && mixAt < stage.rampSamples ? 1 : 0;
    }
    else
    {
        mixDir = mixAt > 0 ? -1 : 0;
        if (mixAt == 0) stop();
    }

    for (int offset = 0; offset < numSamples; offset += maxBlock)
    {
        const int n = std::min (maxBlock, numSamples - offset);
        runChunk (L + offset, R != nullptr ? R + offset : nullptr, n, offset, stage);
    }

    if (running != nullptr) runSamples = std::min (latency, runSamples + numSamples);
    mixAt = std::clamp (mixAt + mixDir * numSamples, 0, stage.rampSamples);
    // Faded out: the compressor stops here and is back at rest for next time.
    if (mixAt == 0 && ! wantsCompressor) stop();
}

void CompStageAlign::runChunk (float* L, float* R, int n, int offset, const CompStageBlock& stage) noexcept
{
    const int latency = stage.latency;
    const int ramp = stage.rampSamples;
    const bool stageFlat = stage.alignDir == 0;
    const bool delayedOnly = stageFlat && stage.alignAt >= ramp;
    const bool undelayedOnly = stageFlat && stage.alignAt <= 0;

    if (running == nullptr)
    {
        if (undelayedOnly) { push (L, R, n); return; }
        if (delayedOnly)   { pushAndRead (L, R, L, R, n, latency); return; }
    }
    else if (delayedOnly && mixDir == 0 && mixAt >= ramp)
    {
        // Fully compressed: the plug-in's output as it is, the dry kept warm.
        push (L, R, n);
        running->process (L, R, n);
        return;
    }

    pushAndRead (L, R, dryL.data(), R != nullptr ? dryR.data() : nullptr, n, latency);
    const bool wet = running != nullptr;
    if (wet)
    {
        std::copy (L, L + n, wetL.begin());
        if (R != nullptr) std::copy (R, R + n, wetR.begin());
        running->process (wetL.data(), R != nullptr ? wetR.data() : nullptr, n);
    }

    // The delayed path is the delayed dry crossfaded with the plug-in, both a
    // latency late, and the output fades from the input to that path. A fade
    // that sits at either end plays that end as it is.
    const float invRamp = 1.0f / (float) ramp;
    const bool mixing = wet && ! (mixDir == 0 && mixAt <= 0);
    const auto blend = [&] (float* x, const float* dry, const float* plug)
    {
        for (int i = 0; i < n; ++i)
        {
            float delayed = dry[i];
            if (mixing)
                delayed += rampWeight (mixAt, mixDir, offset + i, ramp, invRamp) * (plug[i] - dry[i]);
            if (delayedOnly)
                x[i] = delayed;
            else
                x[i] += rampWeight (stage.alignAt, stage.alignDir, offset + i, ramp, invRamp) * (delayed - x[i]);
        }
    };
    blend (L, dryL.data(), wet ? wetL.data() : nullptr);
    if (R != nullptr) blend (R, dryR.data(), wet ? wetR.data() : nullptr);
}
} // namespace duskstudio
