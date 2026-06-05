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
    dqVal.assign ((size_t) cap, 1.0f);
    dqIdx.assign ((size_t) cap, 0);
    dqHead = dqCount = 0;
    sampleCounter = 0;

    env         = 1.0f;
    holdOs      = juce::jmax (0, (int) (osRate * 5.0 / 1000.0));   // 5 ms hold
    holdCounter = 0;

    lastReleaseMs = -1.0f;   // force release-coef recompute on first block
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
    dqHead = dqCount = 0;
    sampleCounter = 0;
    env = 1.0f;
    holdCounter = 0;
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
        const float relMs = releaseMs.load (std::memory_order_relaxed);
        if (! juce::approximatelyEqual (relMs, lastReleaseMs))
        {
            lastReleaseMs = relMs;
            releaseCoef   = onePoleCoef (relMs, osRate);
        }
    }

    if (! juce::approximatelyEqual (drive, 1.0f))
        for (int i = 0; i < numSamples; ++i) { L[i] *= drive; R[i] *= drive; }

    float* basePtrs[2] = { L, R };
    juce::dsp::AudioBlock<float> baseBlock (basePtrs, 2, (size_t) numSamples);
    auto up = oversampler->processSamplesUp (baseBlock);

    const int osN = (int) up.getNumSamples();
    float* uL = up.getChannelPointer (0);
    float* uR = up.getChannelPointer (1);

    const int cap = (int) dqVal.size();
    float blockMinEnv = 1.0f;

    for (int i = 0; i < osN; ++i)
    {
        const float inL = uL[i];
        const float inR = uR[i];
        delayL[(size_t) writePos] = inL;
        delayR[(size_t) writePos] = inR;

        const float peak     = juce::jmax (std::abs (inL), std::abs (inR));
        const float target   = (peak > ceiling && peak > 1.0e-9f) ? ceiling / peak : 1.0f;

        // Push target into the monotonic min deque (back holds the largest).
        while (dqCount > 0 && dqVal[(size_t) ((dqHead + dqCount - 1) % cap)] >= target)
            --dqCount;
        {
            const int slot = (dqHead + dqCount) % cap;
            dqVal[(size_t) slot] = target;
            dqIdx[(size_t) slot] = sampleCounter;
            ++dqCount;
        }
        // Drop entries that have fallen out of the lookahead window.
        while (dqCount > 0 && dqIdx[(size_t) dqHead] <= sampleCounter - windowLen)
        {
            dqHead = (dqHead + 1) % cap;
            --dqCount;
        }
        const float wmin = dqVal[(size_t) dqHead];

        // Instant attack to the lookahead-guaranteed gain (so env never sits
        // above wmin and the output peak lands exactly at the ceiling), then
        // hold, then a smooth one-pole release. The lookahead delay means this
        // snap happens before the peak reaches the output, so it is not heard
        // as a click.
        if (wmin <= env)
        {
            env = wmin;
            holdCounter = holdOs;
        }
        else if (holdCounter > 0)
        {
            --holdCounter;
        }
        else
        {
            env += (wmin - env) * releaseCoef;
        }

        const int   readPos = (writePos + 1) % delayLen;
        const float gain    = active ? env : 1.0f;
        float oL = delayL[(size_t) readPos] * gain;
        float oR = delayR[(size_t) readPos] * gain;
        if (active)
        {
            oL = juce::jlimit (-ceiling, ceiling, oL);
            oR = juce::jlimit (-ceiling, ceiling, oR);
        }
        uL[i] = oL;
        uR[i] = oR;

        if (active && env < blockMinEnv) blockMinEnv = env;

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
