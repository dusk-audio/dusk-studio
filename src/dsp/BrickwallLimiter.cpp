#include "BrickwallLimiter.h"
#include <algorithm>
#include <cmath>

namespace duskstudio
{
void BrickwallLimiter::prepare (double sampleRate, int /*maxBlockSize*/, double lookaheadMs)
{
    sr = sampleRate;
    const int lookahead = (int) juce::jmax (1.0, sampleRate * lookaheadMs / 1000.0);
    lookaheadSamples.store (lookahead, std::memory_order_relaxed);

    const int len = lookahead + 1;
    delayL.assign ((size_t) len, 0.0f);
    delayR.assign ((size_t) len, 0.0f);
    gainHistory.assign ((size_t) len, 1.0f);

    writePos = 0;
    envelope = 1.0f;

    // 1-pole release time constant. We compute the per-sample coefficient
    // for the configured release time (re-applied each block in case the
    // user tweaks the knob). Stored here just to seed the first block.
    const float ms = releaseMs.load (std::memory_order_relaxed);
    releaseCoef = (ms > 0.0f && sr > 0.0)
                  ? 1.0f - std::exp (-1.0f / (ms * 0.001f * (float) sr))
                  : 1.0f;
}

void BrickwallLimiter::reset() noexcept
{
    std::fill (delayL.begin(), delayL.end(), 0.0f);
    std::fill (delayR.begin(), delayR.end(), 0.0f);
    std::fill (gainHistory.begin(), gainHistory.end(), 1.0f);
    envelope = 1.0f;
    writePos = 0;
    currentGrDb.store (0.0f, std::memory_order_relaxed);
}

void BrickwallLimiter::processInPlace (float* L, float* R, int numSamples) noexcept
{
    if (delayL.empty()) return;  // not prepared
    const int len = (int) delayL.size();

    const float drive   = juce::Decibels::decibelsToGain (
                            inputDrive.load (std::memory_order_relaxed));
    const float ceiling = juce::Decibels::decibelsToGain (
                            ceilingDb.load (std::memory_order_relaxed));
    const bool  active  = enabled.load (std::memory_order_relaxed);

    // Refresh release coefficient - cheap, lets the user automate release.
    {
        const float ms = releaseMs.load (std::memory_order_relaxed);
        releaseCoef = (ms > 0.0f && sr > 0.0)
                      ? 1.0f - std::exp (-1.0f / (ms * 0.001f * (float) sr))
                      : 1.0f;
    }

    float blockMinEnv = 1.0f;

    for (int i = 0; i < numSamples; ++i)
    {
        const float inL = L[i] * drive;
        const float inR = R[i] * drive;

        delayL[(size_t) writePos] = inL;
        delayR[(size_t) writePos] = inR;

        // Required gain for THIS new input sample.
        const float peak = juce::jmax (std::abs (inL), std::abs (inR));
        const float required = (peak > ceiling && peak > 1.0e-9f)
                                ? ceiling / peak
                                : 1.0f;
        gainHistory[(size_t) writePos] = required;

        // Window-min over the lookahead. O(N) per sample; for 3 ms @ 48 k
        // this is ~144 ops - negligible. Replace with a monotonic deque
        // if we ever push lookahead above ~10 ms.
        float windowMin = 1.0f;
        for (auto v : gainHistory)
            if (v < windowMin) windowMin = v;

        if (windowMin < envelope)
            envelope = windowMin;                       // instant attack
        else
            envelope += (windowMin - envelope) * releaseCoef;  // smooth release

        // Output the lookahead-delayed sample, scaled by envelope. The
        // oldest sample sits at writePos + 1 (one slot ahead in the
        // circular buffer because we just wrote the newest).
        const int readPos = (writePos + 1) % len;
        const float gain = active ? envelope : 1.0f;
        L[i] = delayL[(size_t) readPos] * gain;
        R[i] = delayR[(size_t) readPos] * gain;

        if (active && envelope < blockMinEnv) blockMinEnv = envelope;

        writePos = (writePos + 1) % len;
    }

    // Block GR meter - dB form of (1 - blockMinEnv) is what the eye expects.
    const float grDb = (blockMinEnv >= 1.0f)
                        ? 0.0f
                        : juce::Decibels::gainToDecibels (blockMinEnv, -60.0f);
    currentGrDb.store (grDb, std::memory_order_relaxed);
}
} // namespace duskstudio
