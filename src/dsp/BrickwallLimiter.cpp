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
} // namespace

void BrickwallLimiter::prepare (double sampleRate, int maxBlockSize, double lookaheadMs)
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

    lookaheadOs = juce::jmax (1, (int) (osRate * lookaheadMs / 1000.0));
    delayLen    = lookaheadOs + 1;
    delayL.assign ((size_t) delayLen, 0.0f);
    delayR.assign ((size_t) delayLen, 0.0f);
    writePos = 0;

    windowLen = lookaheadOs + 1;
    const int cap = windowLen + 1;
    for (int c = 0; c < 2; ++c)
    {
        dqVal[c].assign ((size_t) cap, 1.0f);
        dqIdx[c].assign ((size_t) cap, 0);
        dqHead[c] = dqCount[c] = 0;
        env[c] = 1.0f;
        holdCounter[c] = 0;
    }
    sampleCounter = 0;

    holdOs        = juce::jmax (0, (int) (osRate * 5.0 / 1000.0));   // default (Modern)
    lastReleaseMs = -1.0f;   // force release/hold recompute on first block
    lastMode      = -1;
    releaseCoef   = onePoleCoef (releaseMs.load (std::memory_order_relaxed), osRate);

    const int osLat = (int) std::lround (oversampler->getLatencyInSamples());
    const int lookaheadBase = (lookaheadOs + osFactor / 2) / osFactor;   // round to nearest base sample
    latencySamplesBase.store (osLat + lookaheadBase, std::memory_order_relaxed);

    currentGrDb.store (0.0f, std::memory_order_relaxed);
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
    }
    if (oversampler != nullptr)
        oversampler->reset();
    currentGrDb.store (0.0f, std::memory_order_relaxed);
}

void BrickwallLimiter::processInPlace (float* L, float* R, int numSamples) noexcept
{
    if (oversampler == nullptr || delayLen == 0 || numSamples == 0)
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
            releaseCoef = onePoleCoef (effRelMs, osRate);
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
    float blockMinEnv = 1.0f;

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
                env[c] += (wmin - env[c]) * releaseCoef;
            }
        }

        const int readPos = (writePos + 1) % delayLen;
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

        writePos = (writePos + 1) % delayLen;
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
