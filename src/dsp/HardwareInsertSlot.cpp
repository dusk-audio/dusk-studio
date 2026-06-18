#include "HardwareInsertSlot.h"
#include "../session/Session.h"

#include <cmath>

namespace duskstudio
{
HardwareInsertSlot::HardwareInsertSlot() = default;
HardwareInsertSlot::~HardwareInsertSlot() = default;

void HardwareInsertSlot::prepare (double sampleRate, int blockSize)
{
    prepSampleRate = sampleRate;
    prepBlockSize  = blockSize;

    juce::dsp::ProcessSpec spec;
    spec.sampleRate       = sampleRate;
    spec.maximumBlockSize = (juce::uint32) blockSize;
    spec.numChannels      = 1;

    dryDelayL.prepare (spec);
    dryDelayR.prepare (spec);
    dryDelayL.setMaximumDelayInSamples (kMaxDelaySamples);
    dryDelayR.setMaximumDelayInSamples (kMaxDelaySamples);
    dryDelayL.reset();
    dryDelayR.reset();
    dryDelayL.setDelay (0.0f);
    dryDelayR.setDelay (0.0f);

    // Ping calibration buffers. Chirp is pre-rendered at session SR so
    // the audio thread never builds a sine table in-callback. Capture
    // holds the full chirp plus the maximum measurable lag — the
    // correlator slides the chirp across [0, kMaxDelaySamples].
    chirpBuffer.assign ((size_t) kChirpMaxSamples, 0.0f);
    renderChirp (sampleRate);
    captureBuffer.assign ((size_t) (chirpLength + kMaxDelaySamples), 0.0f);
    pingState = PingState::Idle;
    pingPlayPos = pingCapturePos = pingCorrelateK = 0;
    pingBestPeak = 0.0f;
    pingBestK    = -1;

    // 20 ms smoothing on the continuous knobs - long enough to mask
    // zipper noise on a user drag, short enough that the control still
    // feels responsive.
    constexpr double kSmoothMs = 20.0;
    outGainLin .reset (sampleRate, kSmoothMs * 0.001);
    inGainLin  .reset (sampleRate, kSmoothMs * 0.001);
    dryWetSmooth.reset (sampleRate, kSmoothMs * 0.001);
    outGainLin .setCurrentAndTargetValue (1.0f);
    inGainLin  .setCurrentAndTargetValue (1.0f);
    dryWetSmooth.setCurrentAndTargetValue (1.0f);

    cachedLatencySamples.store (0, std::memory_order_relaxed);
}

void HardwareInsertSlot::bind (const HardwareInsertParams& params) noexcept
{
    paramsRef = &params;
}

void HardwareInsertSlot::resetTailsAndDelayLine() noexcept
{
    dryDelayL.reset();
    dryDelayR.reset();
}

void HardwareInsertSlot::renderChirp (double sampleRate)
{
    // Linear sine chirp 20 Hz -> 20 kHz over 100 ms. -12 dBFS peak.
    // 5 ms raised-cosine fades at each end so the leading/trailing edges
    // don't read as Diracs to the hardware's input stage. Pre-computed
    // sum of the squared samples seeds the auto-correlation peak used
    // by the result-threshold check.
    const int targetLen = juce::jmin (kChirpMaxSamples,
                                        (int) std::lround (0.100 * sampleRate));
    chirpLength = juce::jmax (64, targetLen);

    const double f0 = 20.0;
    const double f1 = 20000.0;
    const double duration = (double) chirpLength / sampleRate;
    const double sweepRate = (f1 - f0) / duration;
    constexpr float peak = 0.2512f;   // -12 dBFS
    const int fadeSamples = juce::jmin (chirpLength / 4,
                                          (int) std::lround (0.005 * sampleRate));

    pingAutoPeak = 0.0f;
    for (int i = 0; i < chirpLength; ++i)
    {
        const double t = (double) i / sampleRate;
        const double phase = 2.0 * juce::MathConstants<double>::pi
                                * (f0 * t + 0.5 * sweepRate * t * t);
        float s = peak * (float) std::sin (phase);

        if (i < fadeSamples)
        {
            const float w = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi
                                                       * (float) i / (float) fadeSamples));
            s *= w;
        }
        else if (i >= chirpLength - fadeSamples)
        {
            // >= so the tail fade covers exactly fadeSamples samples,
            // matching the head fade's [0, fadeSamples) span. With >
            // the last sample of the tail (at i = chirpLength - 1) was
            // outside both branches and stayed at full amplitude.
            const int j = chirpLength - i;
            const float w = 0.5f * (1.0f - std::cos (juce::MathConstants<float>::pi
                                                       * (float) j / (float) fadeSamples));
            s *= w;
        }
        chirpBuffer[(size_t) i] = s;
        pingAutoPeak += s * s;
    }
    // Tail of the buffer past chirpLength stays zero; samples written
    // into the device output beyond chirpLength must be silent so the
    // user's monitoring path doesn't sustain the test tone.
    for (int i = chirpLength; i < (int) chirpBuffer.size(); ++i)
        chirpBuffer[(size_t) i] = 0.0f;
}

void HardwareInsertSlot::startPing()
{
    pingState              = PingState::Playing;
    pingPlayPos            = 0;
    pingCapturePos         = 0;
    pingCorrelateK         = 0;
    pingBestPeak           = 0.0f;
    pingBestK              = -1;
    pingCaptureStallSamples = 0;
    std::fill (captureBuffer.begin(), captureBuffer.end(), 0.0f);
}

bool HardwareInsertSlot::isPingRequested() const noexcept
{
    return paramsRef != nullptr
        && paramsRef->pingPending.load (std::memory_order_acquire);
}

void HardwareInsertSlot::finishPing (int measuredLag)
{
    if (paramsRef != nullptr)
    {
        paramsRef->pingResult.store (measuredLag, std::memory_order_relaxed);
        paramsRef->pingPending.store (false, std::memory_order_release);
    }
    pingState = PingState::Idle;
}

void HardwareInsertSlot::processStereoBlock (float* L, float* R, int numSamples,
                                                const float* const* deviceInputs,
                                                int numDeviceInputs,
                                                float* const*       deviceOutputs,
                                                int numDeviceOutputs) noexcept
{
    juce::ScopedNoDenormals nd;
    if (paramsRef == nullptr || L == nullptr || R == nullptr || numSamples <= 0)
        return;

    // Acquire-load the routing snapshot once. The audio thread now has
    // a stable, internally-consistent view of every routing field for
    // the duration of this block.
    const auto* routingPtr = paramsRef->routing.read();
    if (routingPtr == nullptr) return;
    const HardwareInsertRouting routing = *routingPtr;   // value-copy a small POD

    // Update the dry-path delay length when the user changes the latency
    // setpoint. The delay line was pre-sized to kMaxDelaySamples in
    // prepare(), so setDelay() is a parameter update only - no realloc.
    const int requestedLatency = juce::jlimit (0, kMaxDelaySamples,
                                                  routing.latencySamples);
    const int previousLatency = cachedLatencySamples.load (std::memory_order_relaxed);
    if (requestedLatency != previousLatency)
    {
        cachedLatencySamples.store (requestedLatency, std::memory_order_relaxed);
        dryDelayL.setDelay ((float) requestedLatency);
        dryDelayR.setDelay ((float) requestedLatency);
    }

    // Pull the latest knob values onto the smoother targets.
    const float outDb = paramsRef->outputGainDb.load (std::memory_order_relaxed);
    const float inDb  = paramsRef->inputGainDb .load (std::memory_order_relaxed);
    const float dw    = paramsRef->dryWet      .load (std::memory_order_relaxed);
    outGainLin  .setTargetValue (juce::Decibels::decibelsToGain (outDb));
    inGainLin   .setTargetValue (juce::Decibels::decibelsToGain (inDb));
    dryWetSmooth.setTargetValue (juce::jlimit (0.0f, 1.0f, dw));

    const bool midSide = (routing.format == 1);

    // --- Ping calibration handling -------------------------------------
    // When pingPending is set by the UI, the state machine takes over the
    // SEND side until a result lands. Normal audio still passes through
    // dry to the strip output (the gate's dry/wet handles the user-side
    // mix) - the only override is what we put on the device send pair.
    if (pingState == PingState::Idle
        && paramsRef->pingPending.load (std::memory_order_acquire))
    {
        startPing();
    }

    // Ping state machine. Runs once per block (state) plus per-sample
    // play/capture in the loop below. Correlation work is budgeted per
    // block (kCorrelationsPerBlock candidate lags) so the audio thread
    // never blows its time budget; the result lands a few blocks after
    // capture finishes.
    if (pingState == PingState::Correlating)
    {
        const int chirpLen = juce::jmin (chirpLength, (int) chirpBuffer.size());
        const int maxK = juce::jmax (0, (int) captureBuffer.size() - chirpLen);
        int kCount = 0;
        while (pingCorrelateK < maxK && kCount < kCorrelationsPerBlock)
        {
            float corr = 0.0f;
            for (int i = 0; i < chirpLen; ++i)
                corr += captureBuffer[(size_t) (pingCorrelateK + i)]
                       * chirpBuffer  [(size_t) i];
            const float absCorr = std::fabs (corr);
            if (absCorr > pingBestPeak)
            {
                pingBestPeak = absCorr;
                pingBestK    = pingCorrelateK;
            }
            ++pingCorrelateK;
            ++kCount;
        }
        if (pingCorrelateK >= maxK)
        {
            // Peak below 5 % of the chirp's auto-correlation means we
            // didn't see a clean return - bail with -1 so the UI can
            // surface "ping failed".
            const float threshold = 0.05f * pingAutoPeak;
            const int result = (pingBestPeak >= threshold && pingBestK >= 0)
                                  ? pingBestK : -1;
            finishPing (result);
        }
    }
    // -------------------------------------------------------------------

    // Bounds-check the device channel indices. An out-of-range routing
    // (e.g. user picked output 7-8 but then switched to a 2-out device)
    // falls through to a dry-only path so the user hears something
    // sensible instead of silence + a crashed audio thread.
    const bool outValid = routing.outputChL >= 0
                       && routing.outputChR >= 0
                       && routing.outputChL < numDeviceOutputs
                       && routing.outputChR < numDeviceOutputs
                       && deviceOutputs != nullptr
                       && deviceOutputs[routing.outputChL] != nullptr
                       && deviceOutputs[routing.outputChR] != nullptr;
    const bool inValid  = routing.inputChL >= 0
                       && routing.inputChR >= 0
                       && routing.inputChL < numDeviceInputs
                       && routing.inputChR < numDeviceInputs
                       && deviceInputs != nullptr
                       && deviceInputs[routing.inputChL] != nullptr
                       && deviceInputs[routing.inputChR] != nullptr;

    for (int i = 0; i < numSamples; ++i)
    {
        const float drySrcL = L[i];
        const float drySrcR = R[i];

        // Phase-align the dry copy with the wet return. The hardware
        // takes `latency` samples to round-trip the audio; we delay
        // the dry by the same amount so the dry/wet mix is coherent.
        dryDelayL.pushSample (0, drySrcL);
        dryDelayR.pushSample (0, drySrcR);
        const float dryL = dryDelayL.popSample (0);
        const float dryR = dryDelayR.popSample (0);

        // Encode the SEND. Mid/Side uses 0.5 scaling on encode so the
        // matching decode below (which sums M+S and M-S unscaled)
        // reconstructs the original L/R at unity gain. No level jump
        // when the user toggles the format radio.
        float sendL = drySrcL;
        float sendR = drySrcR;
        if (midSide)
        {
            sendL = 0.5f * (drySrcL + drySrcR);
            sendR = 0.5f * (drySrcL - drySrcR);
        }

        const float outGain = outGainLin.getNextValue();
        sendL *= outGain;
        sendR *= outGain;

        // Ping override: while the chirp is playing, REPLACE the SEND
        // signal with the chirp sample so the only thing on the device
        // output is the calibration excitation - any user audio mixed
        // alongside would corrupt the cross-correlation peak. After the
        // chirp ends the SEND drops to silence while capture finishes,
        // and user audio stays muted on the send path until the result
        // lands.
        if (pingState == PingState::Playing && pingPlayPos < chirpLength)
        {
            const float chirpSample = chirpBuffer[(size_t) pingPlayPos];
            sendL = chirpSample;
            sendR = chirpSample;
            ++pingPlayPos;
            if (pingPlayPos >= chirpLength)
                pingState = PingState::Capturing;
        }
        else if (pingState == PingState::Capturing
                 || pingState == PingState::Correlating)
        {
            // Send silence on the device output during capture +
            // correlation. We don't want stray user audio in the
            // outboard's input while we're trying to measure it.
            sendL = 0.0f;
            sendR = 0.0f;
        }

        // Accumulate the SEND into the device-output buffers. The
        // engine pre-zeroed every output at the top of the callback
        // (Phase 2), so multiple inserts sharing an output pair sum
        // naturally and the master overwrites its assigned channels
        // via memcpy at the end of the callback regardless.
        if (outValid)
        {
            deviceOutputs[routing.outputChL][i] += sendL;
            deviceOutputs[routing.outputChR][i] += sendR;
        }

        // Read the RETURN. With invalid routing, the wet path is
        // silent - the user gets the dry signal back through the
        // dry/wet mix and a visible empty input dropdown in the
        // editor modal.
        float retL = 0.0f, retR = 0.0f;
        if (inValid)
        {
            retL = deviceInputs[routing.inputChL][i];
            retR = deviceInputs[routing.inputChR][i];

            // Ping capture: store the L-channel return into the ring.
            // We correlate against L only - the M/S decode hasn't run
            // yet, so a stereo hardware unit's centre material is
            // strongest on L. Capture runs concurrently with playback
            // from the chirp's first sample, so the correlator's best
            // lag IS the round-trip latency. (Capture only after the
            // chirp would discard any return that arrives during the
            // 100 ms play window - i.e. every realistic round-trip.)
            if ((pingState == PingState::Playing
                 || pingState == PingState::Capturing)
                && pingCapturePos < (int) captureBuffer.size())
            {
                captureBuffer[(size_t) pingCapturePos] = retL;
                ++pingCapturePos;
                pingCaptureStallSamples = 0;   // capture is making forward progress
                if (pingCapturePos >= (int) captureBuffer.size())
                {
                    pingState      = PingState::Correlating;
                    pingCorrelateK = 0;
                    pingBestPeak   = 0.0f;
                    pingBestK      = -1;
                }
            }
        }
        else if (pingState == PingState::Playing
                 || pingState == PingState::Capturing)
        {
            // Routing went invalid mid-capture (user cleared the input
            // combo, device hot-swap, etc.). Without this watchdog
            // pingCapturePos would never advance and the UI would hang
            // on the "Pinging..." button forever. Bail with -2 (distinct
            // from -1 = "captured but correlation peak too low") so the
            // UI can say "no input signal" instead of the generic
            // "ping failed - check level".
            if (++pingCaptureStallSamples > kPingCaptureStallMax)
                finishPing (-2);
        }
        const float inGain = inGainLin.getNextValue();
        retL *= inGain;
        retR *= inGain;

        // Mid/Side decode: L = M+S, R = M-S. Combined with the 0.5
        // encode above, a pure-mono input (L=R) round-trips unchanged
        // and a hard-panned input (L=x, R=0) round-trips unchanged.
        float wetL = retL;
        float wetR = retR;
        if (midSide)
        {
            const float M = retL;
            const float S = retR;
            wetL = M + S;
            wetR = M - S;
        }

        const float wetMix = dryWetSmooth.getNextValue();
        const float dryMix = 1.0f - wetMix;
        L[i] = dryMix * dryL + wetMix * wetL;
        R[i] = dryMix * dryR + wetMix * wetR;
    }
}
} // namespace duskstudio
