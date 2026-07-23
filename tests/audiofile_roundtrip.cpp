#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/audiofile/FileReader.h"
#include "engine/audiofile/FileWriter.h"
#include "engine/audiofile/ThreadedFileWriter.h"

#include <sndfile.h>

#include <cmath>
#include <filesystem>
#include <memory>
#include <system_error>
#include <vector>

using namespace dusk::audio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int    kChannels = 2;
constexpr int64_t kFrames  = 4096;

float sample (int ch, int64_t f)
{
    // Distinct, bounded, non-trivial per channel so channel swaps or off-by-one
    // reads show up as failures rather than passing by symmetry.
    return 0.9f * std::sin ((float) f * 0.01f + (float) ch * 1.3f);
}

std::filesystem::path tmp (const char* name)
{
    return std::filesystem::temp_directory_path() / name;
}

// Non-throwing: the Windows CI runner's virus scanner intermittently holds a
// just-closed temp file, and a cleanup hiccup must not fail a run whose
// assertions all passed.
void discard (const std::filesystem::path& p)
{
    std::error_code ec;
    std::filesystem::remove (p, ec);
}

std::vector<std::vector<float>> makeRamp()
{
    std::vector<std::vector<float>> ch ((size_t) kChannels, std::vector<float> ((size_t) kFrames));
    for (int c = 0; c < kChannels; ++c)
        for (int64_t f = 0; f < kFrames; ++f)
            ch[(size_t) c][(size_t) f] = sample (c, f);
    return ch;
}

std::vector<const float*> ptrs (const std::vector<std::vector<float>>& ch)
{
    std::vector<const float*> p;
    for (auto& v : ch) p.push_back (v.data());
    return p;
}
} // namespace

TEST_CASE ("FileWriter/FileReader round-trip preserves samples", "[audiofile]")
{
    struct Case { const char* file; WriteSpec::Format fmt; int bits; bool isFloat; float tol; };
    const Case cases[] = {
        { "dusk_af_wav16.wav",   WriteSpec::Format::Wav,  16, false, 2.0e-4f },
        { "dusk_af_wav24.wav",   WriteSpec::Format::Wav,  24, false, 1.0e-5f },
        { "dusk_af_wav32.wav",   WriteSpec::Format::Wav,  32, true,  1.0e-6f },
        { "dusk_af_flac24.flac", WriteSpec::Format::Flac, 24, false, 1.0e-5f },
        { "dusk_af_aiff24.aiff", WriteSpec::Format::Aiff, 24, false, 1.0e-5f },
    };

    const auto ramp = makeRamp();
    const auto srcP = ptrs (ramp);

    for (const auto& tc : cases)
    {
        SECTION (tc.file)
        {
            const auto path = tmp (tc.file);

            WriteSpec spec;
            spec.sampleRate    = 48000.0;
            spec.numChannels   = kChannels;
            spec.bitsPerSample = tc.bits;
            spec.format        = tc.fmt;

            {
                auto w = FileWriter::create (path, spec);
                REQUIRE (w != nullptr);
                REQUIRE (w->write (srcP.data(), kChannels, kFrames));
            }

            auto r = FileReader::open (path);
            REQUIRE (r != nullptr);
            REQUIRE (r->info().numChannels == kChannels);
            REQUIRE (r->info().numFrames   == kFrames);
            REQUIRE (r->info().sampleRate  == 48000.0);
            REQUIRE (r->info().isFloat     == tc.isFloat);

            std::vector<std::vector<float>> out ((size_t) kChannels, std::vector<float> ((size_t) kFrames, 0.0f));
            std::vector<float*> outP;
            for (auto& v : out) outP.push_back (v.data());

            REQUIRE (r->read (outP.data(), kChannels, 0, kFrames) == kFrames);

            for (int c = 0; c < kChannels; ++c)
                for (int64_t f = 0; f < kFrames; ++f)
                    REQUIRE_THAT (out[(size_t) c][(size_t) f], WithinAbs (sample (c, f), tc.tol));

            r.reset();
            discard (path);
        }
    }
}

TEST_CASE ("FileReader seek matches a full-buffer slice", "[audiofile]")
{
    const auto ramp = makeRamp();
    const auto srcP = ptrs (ramp);
    const auto path = tmp ("dusk_af_seek.wav");

    WriteSpec spec; spec.sampleRate = 48000.0; spec.numChannels = kChannels; spec.bitsPerSample = 32;
    {
        auto w = FileWriter::create (path, spec);
        REQUIRE (w != nullptr);
        REQUIRE (w->write (srcP.data(), kChannels, kFrames));
    }

    auto r = FileReader::open (path);
    REQUIRE (r != nullptr);

    const int64_t start = 1000, len = 512;
    std::vector<std::vector<float>> out ((size_t) kChannels, std::vector<float> ((size_t) len, 0.0f));
    std::vector<float*> outP; for (auto& v : out) outP.push_back (v.data());

    REQUIRE (r->read (outP.data(), kChannels, start, len) == len);
    for (int c = 0; c < kChannels; ++c)
        for (int64_t f = 0; f < len; ++f)
            REQUIRE_THAT (out[(size_t) c][(size_t) f], WithinAbs (sample (c, start + f), 1.0e-6f));

    r.reset();
    discard (path);
}

TEST_CASE ("FileReader distinguishes 32-bit PCM from floating point", "[audiofile]")
{
    const auto path = tmp ("dusk_af_pcm32.wav");
    SF_INFO info {};
    info.samplerate = 48000;
    info.channels = 1;
    info.format = SF_FORMAT_WAV | SF_FORMAT_PCM_32;

    std::unique_ptr<SNDFILE, decltype (&sf_close)> file (
        sf_open (path.string().c_str(), SFM_WRITE, &info), &sf_close);
    REQUIRE (file != nullptr);

    std::vector<float> samples ((size_t) kFrames, 0.25f);
    REQUIRE (sf_writef_float (file.get(), samples.data(), kFrames) == kFrames);
    file.reset();

    auto reader = FileReader::open (path);
    REQUIRE (reader != nullptr);
    REQUIRE (reader->info().bitsPerSample == 32);
    REQUIRE_FALSE (reader->info().isFloat);

    reader.reset();
    discard (path);
}

TEST_CASE ("FileReader zero-fills channels beyond the file", "[audiofile]")
{
    // Mono file, read into a stereo destination: channel 1 must be silent.
    const auto path = tmp ("dusk_af_mono.wav");
    std::vector<float> mono ((size_t) kFrames);
    for (int64_t f = 0; f < kFrames; ++f) mono[(size_t) f] = sample (0, f);
    const float* mp = mono.data();

    WriteSpec spec; spec.sampleRate = 48000.0; spec.numChannels = 1; spec.bitsPerSample = 32;
    {
        auto w = FileWriter::create (path, spec);
        REQUIRE (w != nullptr);
        REQUIRE (w->write (&mp, 1, kFrames));
    }

    auto r = FileReader::open (path);
    REQUIRE (r != nullptr);
    std::vector<std::vector<float>> out (2, std::vector<float> ((size_t) kFrames, 1.0f));
    std::vector<float*> outP { out[0].data(), out[1].data() };

    REQUIRE (r->read (outP.data(), 2, 0, kFrames) == kFrames);
    for (int64_t f = 0; f < kFrames; ++f)
        REQUIRE (out[1][(size_t) f] == 0.0f);

    r.reset();
    discard (path);
}

TEST_CASE ("ThreadedFileWriter drains all pushed frames to disk", "[audiofile]")
{
    const auto ramp = makeRamp();
    const auto srcP = ptrs (ramp);
    const auto path = tmp ("dusk_af_threaded.wav");

    WriteSpec spec; spec.sampleRate = 48000.0; spec.numChannels = kChannels; spec.bitsPerSample = 32;

    {
        auto fw = FileWriter::create (path, spec);
        REQUIRE (fw != nullptr);
        ThreadedFileWriter tw (std::move (fw), 8192);

        const int64_t block = 256;
        for (int64_t off = 0; off < kFrames; off += block)
        {
            const float* p[kChannels] = { ramp[0].data() + off, ramp[1].data() + off };
            REQUIRE (tw.push (p, kChannels, block));
        }
    } // dtor joins worker + flushes

    auto r = FileReader::open (path);
    REQUIRE (r != nullptr);
    REQUIRE (r->info().numFrames == kFrames);

    std::vector<std::vector<float>> out ((size_t) kChannels, std::vector<float> ((size_t) kFrames, 0.0f));
    std::vector<float*> outP; for (auto& v : out) outP.push_back (v.data());
    REQUIRE (r->read (outP.data(), kChannels, 0, kFrames) == kFrames);

    for (int c = 0; c < kChannels; ++c)
        for (int64_t f = 0; f < kFrames; ++f)
            REQUIRE_THAT (out[(size_t) c][(size_t) f], WithinAbs (sample (c, f), 1.0e-6f));

    r.reset();
    discard (path);
}

TEST_CASE ("ThreadedFileWriter drops a block when the ring is full", "[audiofile]")
{
    const auto path = tmp ("dusk_af_overflow.wav");
    WriteSpec spec; spec.sampleRate = 48000.0; spec.numChannels = kChannels; spec.bitsPerSample = 32;
    auto fw = FileWriter::create (path, spec);
    REQUIRE (fw != nullptr);

    // Tiny ring, no consumer progress guaranteed: at least one oversized push
    // must be refused rather than corrupting the buffer.
    {
        ThreadedFileWriter tw (std::move (fw), 64);
        std::vector<float> a (256, 0.5f), b (256, -0.5f);
        const float* p[kChannels] = { a.data(), b.data() };
        REQUIRE_FALSE (tw.push (p, kChannels, 256));
    }

    discard (path);
}
