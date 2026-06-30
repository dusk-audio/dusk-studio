#include "BrickwallLimiter.h"
#include <algorithm>
#include <cmath>

namespace duskstudio
{
namespace
{
inline float onePoleCoef (float timeMs, double rate) noexcept
{
    const float t = timeMs * 0.001f * (float) rate;
    return (t > 0.0f) ? 1.0f - std::exp (-1.0f / t) : 1.0f;
}

// Maps the smoothed reduction depth (linear, 0..1) to the fast↔slow release
// blend. 2.0 → full slow at ~6 dB of sustained GR, mostly fast at ≤1 dB.
constexpr float kReleaseAdaptGain = 2.0f;
// Time constant of the reduction-depth tracker that drives the blend — short
// enough to follow program, long enough that a lone transient stays "fast".
constexpr float kRelDepthTrackMs  = 30.0f;
// Lookahead bounds. The delay/deque are sized for the max so the active value
// can change at runtime without reallocating.
constexpr float kMinLookaheadMs   = 0.1f;
constexpr float kMaxLookaheadMs   = 10.0f;
} // namespace

void BrickwallLimiter::prepare (double sampleRate, int maxBlockSize, double initialLookaheadMs)
{
    sr       = sampleRate > 0.0 ? sampleRate : 44100.0;
    osFactor = 4;
    osRate   = sr * (double) osFactor;

    const int bs     = juce::jmax (1, maxBlockSize);
    const int stages = 2;   // two 2x stages = 4x

    // Linear-phase FIR so the limiter doesn't smear transients with phase
    // distortion; integer latency keeps delay compensation exact.
    oversampler = std::make_unique<juce::dsp::Oversampling<float>> (
        2, (size_t) stages,
        juce::dsp::Oversampling<float>::filterHalfBandFIREquiripple,
        true /*max quality*/, true /*integer latency*/);
    oversampler->initProcessing ((size_t) bs);
    oversampler->reset();

    // Size the delay + deque for the MAX lookahead; the active read offset is
    // set below (and re-settable at runtime) within this allocation.
    maxLookaheadOs = juce::jmax (1, (int) (osRate * kMaxLookaheadMs / 1000.0));
    bufLen         = maxLookaheadOs + 1;
    delayL.assign ((size_t) bufLen, 0.0f);
    delayR.assign ((size_t) bufLen, 0.0f);
    writePos = 0;

    const int cap = maxLookaheadOs + 2;   // max window (maxLookaheadOs + 1) + 1
    for (int c = 0; c < 2; ++c)
    {
        dqVal[c].assign ((size_t) cap, 1.0f);
        dqIdx[c].assign ((size_t) cap, 0);
        dqHead[c] = dqCount[c] = 0;
        env[c] = 1.0f;
        holdCounter[c] = 0;
        relDepth[c] = 0.0f;
    }
    sampleCounter = 0;

    holdOs        = juce::jmax (0, (int) (osRate * 5.0 / 1000.0));   // default (Modern)
    lastReleaseMs = -1.0f;   // force release/hold recompute on first block
    lastMode      = -1;
    {
        const float relMs0 = releaseMs.load (std::memory_order_relaxed);
        releaseCoefFast = onePoleCoef (relMs0 * 0.5f, osRate);
        releaseCoefSlow = onePoleCoef (relMs0 * 2.0f, osRate);
    }
    relDepthCoef  = onePoleCoef (kRelDepthTrackMs, osRate);

    osLatencyBase = (int) std::lround (oversampler->getLatencyInSamples());
    const float la0 = std::isfinite ((float) initialLookaheadMs) ? (float) initialLookaheadMs : 2.0f;
    lookaheadMs.store (juce::jlimit (kMinLookaheadMs, kMaxLookaheadMs, la0), std::memory_order_relaxed);
    recomputeActiveLookahead();
    lastActiveLaOs = activeLookaheadOs.load (std::memory_order_relaxed);

    currentGrDb.store (0.0f, std::memory_order_relaxed);
}

void BrickwallLimiter::setLookaheadMs (float ms) noexcept
{
    // Guard against non-finite input: jlimit would pass NaN through (NaN
    // compares false against both bounds) and recomputeActiveLookahead would
    // then cast NaN to int — undefined. Ignore garbage, keep the last value.
    if (! std::isfinite (ms)) return;
    lookaheadMs.store (juce::jlimit (kMinLookaheadMs, kMaxLookaheadMs, ms), std::memory_order_relaxed);
    recomputeActiveLookahead();
}

void BrickwallLimiter::recomputeActiveLookahead() noexcept
{
    if (osRate <= 0.0 || maxLookaheadOs <= 0) return;
    const float ms = lookaheadMs.load (std::memory_order_relaxed);
    // Quantize the lookahead in BASE-rate samples FIRST, then derive the
    // oversampled count from it. This keeps laOs an exact multiple of osFactor,
    // so the reported latency (lookaheadBase, base samples) equals the delay
    // actually applied (laOs / osFactor) — no sub-sample drift between them.
    const int maxLookaheadBase = juce::jmax (1, maxLookaheadOs / osFactor);
    const int lookaheadBase = juce::jlimit (1, maxLookaheadBase,
                                            (int) std::lround (sr * (double) ms / 1000.0));
    activeLookaheadOs.store (lookaheadBase * osFactor, std::memory_order_relaxed);
    latencySamplesBase.store (osLatencyBase + lookaheadBase, std::memory_order_relaxed);
}

void BrickwallLimiter::reset() noexcept
{
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    writePos = 0;
    sampleCounter = 0;
    for (int c = 0; c < 2; ++c)
    {
        dqHead[c] = dqCount[c] = 0;
        env[c] = 1.0f;
        holdCounter[c] = 0;
        relDepth[c] = 0.0f;
    }
    lastActiveLaOs = activeLookaheadOs.load (std::memory_order_relaxed);
    if (oversampler != nullptr)
        oversampler->reset();
    currentGrDb.store (0.0f, std::memory_order_relaxed);
}

void BrickwallLimiter::processInPlace (float* L, float* R, int numSamples) noexcept
{
    if (oversampler == nullptr || bufLen == 0 || numSamples <= 0 || L == nullptr || R == nullptr)
        return;

    juce::ScopedNoDenormals noDenormals;

    const bool  active  = enabled.load (std::memory_order_relaxed);
    const float drive   = active ? juce::Decibels::decibelsToGain (
                                        inputDrive.load (std::memory_order_relaxed))
                                  : 1.0f;
    const float ceiling = juce::Decibels::decibelsToGain (
                            ceilingDb.load (std::memory_order_relaxed));

    {
        const int   modeNow = mode.load (std::memory_order_relaxed);
        const float relMs   = releaseMs.load (std::memory_order_relaxed);
        if (modeNow != lastMode || ! juce::approximatelyEqual (relMs, lastReleaseMs))
        {
            lastMode      = modeNow;
            lastReleaseMs = relMs;
            // Mode shapes hold + effective release around the release knob:
            // Transparent recovers fast with almost no hold; Punchy holds longer
            // and releases slower for density.
            float holdMs, effRelMs;
            switch (modeNow)
            {
                case 1:  holdMs = 1.0f;  effRelMs = relMs * 0.5f; break;  // Transparent
                case 2:  holdMs = 12.0f; effRelMs = relMs * 2.0f; break;  // Punchy
                default: holdMs = 5.0f;  effRelMs = relMs;        break;  // Modern
            }
            releaseCoefFast = onePoleCoef (effRelMs * 0.5f, osRate);
            releaseCoefSlow = onePoleCoef (effRelMs * 2.0f, osRate);
            holdOs      = juce::jmax (0, (int) (osRate * (double) holdMs / 1000.0));
        }
    }
    const bool linked = stereoLink.load (std::memory_order_relaxed);

    if (! juce::approximatelyEqual (drive, 1.0f))
        for (int i = 0; i < numSamples; ++i) { L[i] *= drive; R[i] *= drive; }

    float* basePtrs[2] = { L, R };
    juce::dsp::AudioBlock<float> baseBlock (basePtrs, 2, (size_t) numSamples);
    auto up = oversampler->processSamplesUp (baseBlock);

    const int osN = (int) up.getNumSamples();
    float* uL = up.getChannelPointer (0);
    float* uR = up.getChannelPointer (1);

    const int cap = (int) dqVal[0].size();
    // Active lookahead read once per block — keeps the read offset and the
    // deque window consistent even if setLookaheadMs() runs mid-stream.
    const int laOs      = activeLookaheadOs.load (std::memory_order_relaxed);
    const int windowLen = laOs + 1;
    float blockMinEnv = 1.0f;

    // Lookahead changed since the last block: rebuild the min-deque FORWARD from
    // empty instead of extending the existing window in place. A grown window
    // would otherwise need older samples this deque already dropped, leaving the
    // running minimum under-covered; clearing it re-warms cleanly over the next
    // windowLen samples (env untouched, so no gain step; the terminal base-rate
    // clamp still guarantees the ceiling during the re-warm).
    if (laOs != lastActiveLaOs)
    {
        for (int c = 0; c < 2; ++c) { dqHead[c] = 0; dqCount[c] = 0; }
        lastActiveLaOs = laOs;
    }

    for (int i = 0; i < osN; ++i)
    {
        const float inL = uL[i];
        const float inR = uR[i];
        delayL[(size_t) writePos] = inL;
        delayR[(size_t) writePos] = inR;

        // Required gain per channel. Linked: both see the max-of-L/R target so
        // the reduction is matched and the stereo image is preserved. Unlinked:
        // each channel limits on its own peak.
        float tgt[2];
        if (linked)
        {
            const float peak = juce::jmax (std::abs (inL), std::abs (inR));
            tgt[0] = tgt[1] = (peak > ceiling && peak > 1.0e-9f) ? ceiling / peak : 1.0f;
        }
        else
        {
            const float pL = std::abs (inL), pR = std::abs (inR);
            tgt[0] = (pL > ceiling && pL > 1.0e-9f) ? ceiling / pL : 1.0f;
            tgt[1] = (pR > ceiling && pR > 1.0e-9f) ? ceiling / pR : 1.0f;
        }

        for (int c = 0; c < 2; ++c)
        {
            const float target = tgt[c];
            // Monotonic min deque push (back holds the largest).
            while (dqCount[c] > 0
                   && dqVal[c][(size_t) ((dqHead[c] + dqCount[c] - 1) % cap)] >= target)
                --dqCount[c];
            {
                const int slot = (dqHead[c] + dqCount[c]) % cap;
                dqVal[c][(size_t) slot] = target;
                dqIdx[c][(size_t) slot] = sampleCounter;
                ++dqCount[c];
            }
            // Drop entries that have fallen out of the lookahead window.
            while (dqCount[c] > 0 && dqIdx[c][(size_t) dqHead[c]] <= sampleCounter - windowLen)
            {
                dqHead[c] = (dqHead[c] + 1) % cap;
                --dqCount[c];
            }
            const float wmin = dqVal[c][(size_t) dqHead[c]];

            // When bypassed, hold the smoother neutral. The target/deque above
            // still track (so the window is warm on re-enable), but env and the
            // program-dependent depth tracker must NOT accumulate reduction from
            // material the limiter isn't actually reducing — otherwise the first
            // release after re-enabling would be biased by stale history.
            if (! active)
            {
                env[c] = 1.0f;
                holdCounter[c] = 0;
                relDepth[c] = 0.0f;
                continue;
            }

            // Instant attack to the lookahead-guaranteed gain (env never sits
            // above wmin, so the output peak lands at the ceiling), then hold,
            // then a smooth one-pole release. The lookahead delay means the snap
            // happens before the peak reaches the output, so it isn't a click.
            if (wmin <= env[c])
            {
                env[c] = wmin;
                holdCounter[c] = holdOs;
            }
            else if (holdCounter[c] > 0)
            {
                --holdCounter[c];
            }
            else
            {
                // Program-dependent release: blend fast↔slow by the smoothed
                // reduction depth carried from the (deep, sustained vs brief)
                // limiting that preceded this recovery.
                const float blend = juce::jlimit (0.0f, 1.0f, relDepth[c] * kReleaseAdaptGain);
                const float coef  = releaseCoefFast + (releaseCoefSlow - releaseCoefFast) * blend;
                env[c] += (wmin - env[c]) * coef;
            }

            // Track how deep/sustained the reduction is (drives the blend above).
            relDepth[c] += ((1.0f - env[c]) - relDepth[c]) * relDepthCoef;
        }

        const int readPos = (writePos + bufLen - laOs) % bufLen;
        float oL = delayL[(size_t) readPos] * (active ? env[0] : 1.0f);
        float oR = delayR[(size_t) readPos] * (active ? env[1] : 1.0f);
        if (active)
        {
            oL = juce::jlimit (-ceiling, ceiling, oL);
            oR = juce::jlimit (-ceiling, ceiling, oR);
        }
        uL[i] = oL;
        uR[i] = oR;

        if (active)
            blockMinEnv = juce::jmin (blockMinEnv, juce::jmin (env[0], env[1]));

        writePos = (writePos + 1) % bufLen;
        ++sampleCounter;
    }

    oversampler->processSamplesDown (baseBlock);

    // Residual overshoot from the downsampling reconstruction filter - clamp at
    // base rate so the published ceiling is a hard guarantee.
    if (active)
        for (int i = 0; i < numSamples; ++i)
        {
            L[i] = juce::jlimit (-ceiling, ceiling, L[i]);
            R[i] = juce::jlimit (-ceiling, ceiling, R[i]);
        }

    const float grDb = (blockMinEnv >= 1.0f)
                        ? 0.0f
                        : juce::Decibels::gainToDecibels (blockMinEnv, -60.0f);
    currentGrDb.store (grDb, std::memory_order_relaxed);
}
} // namespace duskstudio
