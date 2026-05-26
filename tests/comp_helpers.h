#pragma once

#include "UniversalCompressor.h"
#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>

#include <cmath>
#include <memory>

namespace duskstudio::comp_test
{

inline std::unique_ptr<UniversalCompressor> makeComp (double sampleRate, int blockSize)
{
    auto c = std::make_unique<UniversalCompressor>();
    c->setPlayConfigDetails (2, 2, sampleRate, blockSize);
    c->prepareToPlay (sampleRate, blockSize);
    return c;
}

inline std::atomic<float>* atom (UniversalCompressor& c, const juce::String& id)
{
    return c.getParameters().getRawParameterValue (id);
}

inline void setParam (UniversalCompressor& c, const juce::String& id, float v)
{
    if (auto* a = atom (c, id))
        a->store (v, std::memory_order_relaxed);
}

inline void setChoice (UniversalCompressor& c, const juce::String& id, int index)
{
    setParam (c, id, (float) index);
}

inline void clearBuffer (juce::AudioBuffer<float>& b)
{
    b.clear();
}

inline void fillSine (juce::AudioBuffer<float>& b, double freq, double sampleRate, float linearAmp)
{
    const int n = b.getNumSamples();
    const float w = (float) (2.0 * juce::MathConstants<double>::pi * freq / sampleRate);
    for (int ch = 0; ch < b.getNumChannels(); ++ch)
    {
        auto* d = b.getWritePointer (ch);
        for (int i = 0; i < n; ++i)
            d[i] = linearAmp * std::sin (w * (float) i);
    }
}

inline float rmsDb (const juce::AudioBuffer<float>& b, int channel, int start, int n)
{
    const auto* d = b.getReadPointer (channel) + start;
    double sumSq = 0.0;
    for (int i = 0; i < n; ++i)
        sumSq += (double) d[i] * (double) d[i];
    const float rms = (float) std::sqrt (sumSq / juce::jmax (1, n));
    return juce::Decibels::gainToDecibels (rms, -120.0f);
}

inline float peakDb (const juce::AudioBuffer<float>& b, int channel, int start, int n)
{
    const auto* d = b.getReadPointer (channel) + start;
    float pk = 0.0f;
    for (int i = 0; i < n; ++i)
        pk = juce::jmax (pk, std::abs (d[i]));
    return juce::Decibels::gainToDecibels (pk, -120.0f);
}

inline void runBlocks (UniversalCompressor& c,
                       juce::AudioBuffer<float>& work,
                       int totalSamples,
                       int blockSize,
                       std::function<void (int blockIndex, juce::AudioBuffer<float>& block)> fillBlock,
                       std::function<void (int blockIndex, const juce::AudioBuffer<float>& block)> onOutput = {})
{
    juce::MidiBuffer midi;
    juce::AudioBuffer<float> blk (2, blockSize);
    int produced = 0;
    int blockIdx = 0;
    while (produced < totalSamples)
    {
        const int n = juce::jmin (blockSize, totalSamples - produced);
        blk.setSize (2, n, false, false, true);
        blk.clear();
        fillBlock (blockIdx, blk);
        c.processBlock (blk, midi);
        if (onOutput) onOutput (blockIdx, blk);
        for (int ch = 0; ch < blk.getNumChannels() && ch < work.getNumChannels(); ++ch)
            work.copyFrom (ch, produced, blk, ch, 0, n);
        produced += n;
        ++blockIdx;
    }
}

inline void runSteadySine (UniversalCompressor& c,
                           double freq,
                           double sampleRate,
                           float amplitude,
                           int totalSamples,
                           int blockSize,
                           juce::AudioBuffer<float>& outCapture)
{
    outCapture.setSize (2, totalSamples, false, false, true);
    outCapture.clear();

    const double w = 2.0 * juce::MathConstants<double>::pi * freq / sampleRate;
    double phase = 0.0;

    runBlocks (c, outCapture, totalSamples, blockSize,
        [&] (int /*idx*/, juce::AudioBuffer<float>& blk)
        {
            const int n = blk.getNumSamples();
            for (int ch = 0; ch < blk.getNumChannels(); ++ch)
            {
                auto* d = blk.getWritePointer (ch);
                double p = phase;
                for (int i = 0; i < n; ++i)
                {
                    d[i] = amplitude * (float) std::sin (p);
                    p += w;
                }
            }
            phase += w * (double) n;
            phase = std::fmod (phase, 2.0 * juce::MathConstants<double>::pi);
        });
}

} // namespace duskstudio::comp_test
