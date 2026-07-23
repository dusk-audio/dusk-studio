#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audiofile/FileReader.h"
#include "engine/audiofile/FileWriter.h"

#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <cmath>
#include <filesystem>
#include <system_error>
#include <vector>

using namespace dusk::audio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int     kChannels = 2;
constexpr int64_t kFrames   = 4096;

float sample (int ch, int64_t f)
{
    return 0.9f * std::sin ((float) f * 0.01f + (float) ch * 1.3f);
}
} // namespace

// FileReader must agree with juce::AudioFormatReader on the same file, and JUCE
// must be able to read what FileWriter produced (existing sessions hold
// JUCE-written WAVs; the swap has to be transparent both directions). The
// tolerance absorbs the different int->float normalisation constants the two
// libraries use; it is tight enough to catch channel swaps, off-by-one, or
// endianness bugs.
TEST_CASE ("FileReader matches juce::AudioFormatReader", "[audiofile][abnull]")
{
    struct Case { const char* file; int bits; float tol; };
    const Case cases[] = {
        { "dusk_abnull_16.wav", 16, 2.0e-4f },
        { "dusk_abnull_24.wav", 24, 1.0e-4f },
        { "dusk_abnull_32.wav", 32, 1.0e-6f },
    };

    for (const auto& tc : cases)
    {
        SECTION (tc.file)
        {
            const auto path = std::filesystem::temp_directory_path() / tc.file;

            std::vector<std::vector<float>> ramp ((size_t) kChannels, std::vector<float> ((size_t) kFrames));
            std::vector<const float*> srcP;
            for (int c = 0; c < kChannels; ++c)
            {
                for (int64_t f = 0; f < kFrames; ++f) ramp[(size_t) c][(size_t) f] = sample (c, f);
                srcP.push_back (ramp[(size_t) c].data());
            }

            WriteSpec spec; spec.sampleRate = 48000.0; spec.numChannels = kChannels; spec.bitsPerSample = tc.bits;
            {
                auto w = FileWriter::create (path, spec);
                REQUIRE (w != nullptr);
                REQUIRE (w->write (srcP.data(), kChannels, kFrames));
            }

            juce::AudioFormatManager fmt;
            fmt.registerBasicFormats();
            std::unique_ptr<juce::AudioFormatReader> jr (fmt.createReaderFor (juce::File (path.string())));
            REQUIRE (jr != nullptr);
            REQUIRE ((int) jr->numChannels == kChannels);
            REQUIRE ((int64_t) jr->lengthInSamples == kFrames);

            juce::AudioBuffer<float> jbuf (kChannels, (int) kFrames);
            REQUIRE (jr->read (&jbuf, 0, (int) kFrames, 0, true, true));

            auto dr = FileReader::open (path);
            REQUIRE (dr != nullptr);
            std::vector<std::vector<float>> dout ((size_t) kChannels, std::vector<float> ((size_t) kFrames, 0.0f));
            std::vector<float*> doutP; for (auto& v : dout) doutP.push_back (v.data());
            REQUIRE (dr->read (doutP.data(), kChannels, 0, kFrames) == kFrames);

            for (int c = 0; c < kChannels; ++c)
                for (int64_t f = 0; f < kFrames; ++f)
                {
                    const float j = jbuf.getSample (c, (int) f);
                    REQUIRE_THAT (dout[(size_t) c][(size_t) f], WithinAbs (j, tc.tol));      // dusk vs juce
                    REQUIRE_THAT (j, WithinAbs (sample (c, f), tc.tol));                     // juce reads our output
                }

            jr.reset();
            dr.reset();

            // Non-throwing: a virus-scanner lock on the Windows CI runner must
            // not fail a run whose assertions all passed.
            std::error_code ec;
            std::filesystem::remove (path, ec);
        }
    }
}
