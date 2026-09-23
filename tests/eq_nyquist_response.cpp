#include <catch2/catch_test_macros.hpp>

#include <FourKEQDSP.hpp>

#include <algorithm>
#include <cmath>
#include <vector>

// Cramping check for duskaudio::FourKEQDSP, the console EQ on every strip and
// bus. A band placed high rolls off early when its filters are designed close to
// Nyquist. The 4x render is the reference: there the top of the audio band is a
// small fraction of the design rate. At 2x the curve must stay on it.

namespace
{
constexpr double kTwoPi = 6.28318530717958647692;

enum class Band { HighMid, High };

// Steady-state gain of a quiet sine, quiet so the always-on console character
// stays in its linear region.
double gainDb (double sampleRate, int oversampling, Band band, float bandFreq, float frequency)
{
    duskaudio::FourKEQDSP eq;
    eq.setOversampling (oversampling);
    eq.setMsMode (false);
    eq.setAutoGain (false);
    eq.setBypass (false);
    eq.setEqType (0);
    eq.setHpfEnabled (false);
    eq.setLpfEnabled (false);
    eq.setLfGain (0.0f);
    eq.setLmGain (0.0f);
    eq.setHmGain (band == Band::HighMid ? 12.0f : 0.0f);
    eq.setHmFreq (band == Band::HighMid ? bandFreq : 2000.0f);
    eq.setHmQ (1.0f);
    eq.setHfGain (band == Band::High ? 12.0f : 0.0f);
    eq.setHfFreq (band == Band::High ? bandFreq : 8000.0f);
    eq.setHfBell (true);
    eq.setInputGainDb (0.0f);
    eq.setOutputGainDb (0.0f);
    eq.setSaturation (22.0f);

    constexpr int kBlock = 256;
    eq.prepare (sampleRate, kBlock);
    eq.reset();

    const int total = (int) (sampleRate / 2.0);
    std::vector<float> in ((size_t) total), outL ((size_t) total), outR ((size_t) total);
    for (int i = 0; i < total; ++i)
        in[(size_t) i] = 0.01f * (float) std::sin (kTwoPi * frequency * i / sampleRate);
    outL = in;
    outR = in;
    for (int offset = 0; offset < total; offset += kBlock)
    {
        float* channels[2] = { outL.data() + offset, outR.data() + offset };
        eq.processBlock (channels, channels, 2, std::min (kBlock, total - offset));
    }

    double inEnergy = 0.0, outEnergy = 0.0;
    for (int i = total / 2; i < total; ++i)
    {
        inEnergy  += (double) in[(size_t) i] * in[(size_t) i];
        outEnergy += (double) outL[(size_t) i] * outL[(size_t) i];
    }
    return 10.0 * std::log10 (outEnergy / inEnergy);
}
} // namespace

TEST_CASE ("4K EQ at 2x oversampling holds its 4x curve up to 20 kHz")
{
    struct Setting { Band band; float freq; };
    for (const double sampleRate : { 44100.0, 48000.0 })
        for (const auto setting : { Setting { Band::HighMid, 13000.0f }, Setting { Band::High, 16000.0f } })
        {
            CAPTURE (sampleRate, setting.freq);
            for (const float frequency : { 1000.0f, 6000.0f, 10000.0f, 13000.0f, 16000.0f, 18000.0f, 20000.0f })
            {
                CAPTURE (frequency);
                const double reference = gainDb (sampleRate, 2, setting.band, setting.freq, frequency);
                const double doubled   = gainDb (sampleRate, 1, setting.band, setting.freq, frequency);
                CHECK (std::abs (doubled - reference) < 1.0);
            }
            CHECK (gainDb (sampleRate, 2, setting.band, setting.freq, setting.band == Band::High ? 13000.0f : 6000.0f) > 9.0);
        }
}

// The documented limit at the default 1x: a boost set high falls several dB
// short of its 4x curve at the top of the band.
TEST_CASE ("4K EQ at 1x rolls a high boost off early near Nyquist")
{
    for (const double sampleRate : { 44100.0, 48000.0 })
        for (const auto band : { Band::HighMid, Band::High })
        {
            CAPTURE (sampleRate);
            const float bandFreq = band == Band::High ? 16000.0f : 13000.0f;
            CHECK (gainDb (sampleRate, 2, band, bandFreq, 20000.0f)
                   - gainDb (sampleRate, 0, band, bandFreq, 20000.0f) > 3.0);
        }
}
