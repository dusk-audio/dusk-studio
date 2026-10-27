#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "engine/FileImporter.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <memory>
#include <stdexcept>

using Catch::Matchers::WithinAbs;

namespace
{
constexpr double kPi = 3.14159265358979323846;

// Helper: write a WAV file with the given channel layout / SR. Each
// channel is filled by the supplied generator (channel, sample) -> float.
template <typename Gen>
void writeTestWav (const juce::File& outFile,
                    double sampleRate,
                    int numChannels,
                    int numSamples,
                    Gen&& gen,
                    int bitsPerSample = 24)
{
    juce::AudioBuffer<float> buf (numChannels, numSamples);
    for (int c = 0; c < numChannels; ++c)
    {
        auto* dst = buf.getWritePointer (c);
        for (int n = 0; n < numSamples; ++n)
            dst[n] = gen (c, n);
    }

    std::unique_ptr<juce::FileOutputStream> stream (outFile.createOutputStream());
    REQUIRE (stream != nullptr);
    REQUIRE (stream->openedOk());

    juce::WavAudioFormat wav;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wav.createWriterFor (stream.get(), sampleRate,
                              (unsigned int) numChannels, bitsPerSample, {}, 0));
    REQUIRE (writer != nullptr);
    stream.release();   // writer owns it now
    REQUIRE (writer->writeFromAudioSampleBuffer (buf, 0, numSamples));
    writer.reset();     // flush + close
}

// Helper: read a WAV file fully into an AudioBuffer plus its sample rate.
struct ReadbackResult
{
    juce::AudioBuffer<float> buffer;
    double sampleRate = 0.0;
    juce::int64 lengthInSamples = 0;
    int numChannels = 0;
    int bitsPerSample = 0;
};

ReadbackResult readWav (const juce::File& file)
{
    juce::AudioFormatManager fm;
    fm.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fm.createReaderFor (file));
    REQUIRE (reader != nullptr);

    ReadbackResult r;
    r.sampleRate      = reader->sampleRate;
    r.lengthInSamples = (juce::int64) reader->lengthInSamples;
    r.numChannels     = (int) reader->numChannels;
    r.bitsPerSample   = (int) reader->bitsPerSample;
    r.buffer.setSize (r.numChannels, (int) r.lengthInSamples);
    REQUIRE (reader->read (&r.buffer, 0, (int) r.lengthInSamples, 0, true, r.numChannels > 1));
    return r;
}

float bufferPeak (const juce::AudioBuffer<float>& buf, int channel)
{
    const auto* p = buf.getReadPointer (channel);
    float peak = 0.0f;
    for (int i = 0; i < buf.getNumSamples(); ++i)
        peak = std::max (peak, std::abs (p[i]));
    return peak;
}

struct TempScope
{
    juce::File dir;
    TempScope()
    {
        const auto parent = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getChildFile ("dusk-studio-fileimporter-tests");
        // Tests run as parallel processes and share this parent: creating it can lose
        // a race with another test ("File exists"), which is fine once it exists.
        (void) parent.createDirectory();
        dir = parent.getChildFile (juce::Uuid().toDashedString());
        const auto result = dir.createDirectory();
        if (result.failed() && ! dir.isDirectory())
            throw std::runtime_error ("TempScope failed to create temp dir '"
                                       + dir.getFullPathName().toStdString()
                                       + "': " + result.getErrorMessage().toStdString());
    }
    ~TempScope() { dir.deleteRecursively(); }
};
}

TEST_CASE ("FileImporter: transient file locks retry but remain bounded",
           "[FileImporter][issue-377]")
{
    SECTION ("a later attempt succeeds")
    {
        int attempts = 0;
        int waits = 0;
        const bool opened = duskstudio::fileimport::detail::retryTransientFileOperation (
            [&]
            {
                ++attempts;
                return attempts == 3;
            },
            [&] { ++waits; });

        REQUIRE (opened);
        REQUIRE (attempts == 3);
        REQUIRE (waits == 2);
    }

    SECTION ("a persistent lock exhausts the fixed attempt budget")
    {
        int attempts = 0;
        int waits = 0;
        const bool opened = duskstudio::fileimport::detail::retryTransientFileOperation (
            [&]
            {
                ++attempts;
                return false;
            },
            [&] { ++waits; });

        REQUIRE_FALSE (opened);
        REQUIRE (attempts == duskstudio::fileimport::detail::kTransientFileAttempts);
        REQUIRE (waits == attempts - 1);
    }
}

TEST_CASE ("FileImporter: 44.1k mono -> 48k session preserves length",
           "[FileImporter][issue-377]")
{
    TempScope tmp;
    const auto src = tmp.dir.getChildFile ("source.wav");
    constexpr double kSrcSr = 44100.0;
    constexpr int    kSrcLen = (int) kSrcSr;   // 1 second
    writeTestWav (src, kSrcSr, 1, kSrcLen, [&] (int, int n)
    {
        return 0.5f * (float) std::sin (2.0 * kPi * 440.0 * (double) n / kSrcSr);
    }, /*bitsPerSample*/ 16);

    duskstudio::fileimport::AudioImportRequest req;
    req.source            = src;
    req.audioDir          = tmp.dir;
    req.trackIndex        = 0;
    req.sessionSampleRate = 48000.0;
    req.targetChannels    = 1;
    req.timelineStart     = 0;

    const auto res = duskstudio::fileimport::importAudio (req);
    REQUIRE (res.ok);
    REQUIRE (res.errorMessage.empty());

    // A converted import is written as 24-bit WAV whatever the source depth.
    const auto rb = readWav (res.region.file);
    REQUIRE (res.region.file.hasFileExtension ("wav"));
    REQUIRE (rb.bitsPerSample == 24);
    REQUIRE (rb.sampleRate == 48000.0);
    REQUIRE (rb.numChannels == 1);
    // 1 s of audio at 48 kHz: 48000 samples ± tolerance for the
    // interpolator's edge behaviour.
    REQUIRE (std::abs (rb.lengthInSamples - 48000) <= 64);
    REQUIRE (res.region.lengthInSamples == rb.lengthInSamples);
    REQUIRE (res.region.numChannels == 1);
}

TEST_CASE ("FileImporter: 44.1k -> 48k upsample fills the tail with audio, not a held sample",
           "[FileImporter][issue-377]")
{
    // Regression: importAudio mistook CatmullRomInterpolator::process()'s return
    // (number of INPUT samples consumed, ~= srcLength) for the produced-output
    // count. On any upsample that's always < outLength, so it padded the last
    // ~8% of the output with a held sample — imported audio went silent/DC at
    // ~92% of its length (a ~5 min file died at ~4:30). Length stayed correct,
    // so the existing length test missed it; assert the TAIL is still live.
    TempScope tmp;
    const auto src = tmp.dir.getChildFile ("source.wav");
    constexpr double kSrcSr  = 44100.0;
    constexpr int    kSrcLen = (int) kSrcSr;   // 1 second
    constexpr double kFreq   = 440.0;
    writeTestWav (src, kSrcSr, 1, kSrcLen, [&] (int, int n)
    {
        return 0.5f * (float) std::sin (2.0 * kPi * kFreq * (double) n / kSrcSr);
    });

    duskstudio::fileimport::AudioImportRequest req;
    req.source            = src;
    req.audioDir          = tmp.dir;
    req.trackIndex        = 0;
    req.sessionSampleRate = 48000.0;
    req.targetChannels    = 1;
    req.timelineStart     = 0;

    const auto res = duskstudio::fileimport::importAudio (req);
    REQUIRE (res.ok);

    const auto rb = readWav (res.region.file);
    const int n = rb.buffer.getNumSamples();
    REQUIRE (n > 2000);

    // Last ~1000 samples sit deep in the old frozen-pad zone (pad began at
    // ~44100/48000). A live 440 Hz sine there spans several cycles, so
    // peak-to-peak ~= 2*amp; a held sample would be ~0.
    const auto* p = rb.buffer.getReadPointer (0);
    float lo = p[n - 1], hi = p[n - 1];
    for (int i = n - 1000; i < n; ++i)
    {
        lo = std::min (lo, p[i]);
        hi = std::max (hi, p[i]);
    }
    REQUIRE ((hi - lo) > 0.5f);
}

TEST_CASE ("FileImporter: 96k mono -> 48k session preserves length",
           "[FileImporter][issue-377]")
{
    TempScope tmp;
    const auto src = tmp.dir.getChildFile ("source.wav");
    constexpr double kSrcSr = 96000.0;
    constexpr int    kSrcLen = (int) kSrcSr;
    writeTestWav (src, kSrcSr, 1, kSrcLen, [&] (int, int n)
    {
        return 0.5f * (float) std::sin (2.0 * kPi * 220.0 * (double) n / kSrcSr);
    });

    duskstudio::fileimport::AudioImportRequest req;
    req.source            = src;
    req.audioDir          = tmp.dir;
    req.trackIndex        = 0;
    req.sessionSampleRate = 48000.0;
    req.targetChannels    = 1;
    req.timelineStart     = 0;

    const auto res = duskstudio::fileimport::importAudio (req);
    REQUIRE (res.ok);
    const auto rb = readWav (res.region.file);
    REQUIRE (rb.sampleRate == 48000.0);
    REQUIRE (std::abs (rb.lengthInSamples - 48000) <= 64);
}

TEST_CASE ("FileImporter: downsampling rejects ultrasonic aliases without changing the passband",
           "[FileImporter][resample][antialias]")
{
    struct Rates { double source, target, ultrasonic; };
    for (const auto rates : { Rates { 96000.0, 48000.0, 36000.0 },
                              Rates { 192000.0, 48000.0, 68000.0 },
                              Rates { 88200.0, 44100.0, 30000.0 },
                              Rates { 96000.0, 44100.0, 36000.0 },
                              Rates { 48000.0, 44100.0, 23900.0 } })
    {
        CAPTURE (rates.source, rates.target, rates.ultrasonic);
        TempScope tmp;
        const auto src = tmp.dir.getChildFile ("spectral.wav");
        writeTestWav (src, rates.source, 2, (int) (2.0 * rates.source), [&] (int channel, int n)
        {
            const double frequency = channel == 0 ? 1000.0 : rates.ultrasonic;
            return 0.25f * (float) std::sin (2.0 * kPi * frequency * (double) n / rates.source);
        }, 32);
        duskstudio::fileimport::AudioImportRequest req;
        req.source = src;
        req.audioDir = tmp.dir;
        req.sessionSampleRate = rates.target;
        req.targetChannels = 2;
        const auto imported = duskstudio::fileimport::importAudio (req);
        REQUIRE (imported.ok);
        const auto output = readWav (imported.region.file);
        REQUIRE (output.lengthInSamples == (std::int64_t) (2.0 * rates.target));
        REQUIRE (output.numChannels == 2);
        const auto amplitude = [&output, rates] (int channel, double frequency)
        {
            const int skip = (int) (rates.target / 10.0);
            const int end = output.buffer.getNumSamples() - skip;
            const float* samples = output.buffer.getReadPointer (channel);
            double sine = 0.0, cosine = 0.0;
            for (int n = skip; n < end; ++n)
            {
                const double phase = 2.0 * kPi * frequency * (double) n / rates.target;
                sine += (double) samples[n] * std::sin (phase);
                cosine += (double) samples[n] * std::cos (phase);
            }
            return 2.0 * std::hypot (sine, cosine) / (double) (end - skip);
        };
        const double alias = std::abs (std::remainder (rates.ultrasonic, rates.target));
        CHECK_THAT (amplitude (0, 1000.0), WithinAbs (0.25, 0.001));
        CHECK (amplitude (1, alias) < 0.00005);
        CHECK (amplitude (0, alias) < 0.00005);
        CHECK (amplitude (1, 1000.0) < 0.00005);
    }
}

TEST_CASE ("FileImporter: downsampling preserves DC gain",
           "[FileImporter][resample][antialias]")
{
    for (const double targetRate : { 48000.0, 44100.0 })
    {
        CAPTURE (targetRate);
        TempScope tmp;
        const auto src = tmp.dir.getChildFile ("dc.wav");
        writeTestWav (src, 96000.0, 1, 96000, [] (int, int) { return 0.125f; }, 32);
        duskstudio::fileimport::AudioImportRequest req;
        req.source = src;
        req.audioDir = tmp.dir;
        req.sessionSampleRate = targetRate;
        req.targetChannels = 1;
        const auto imported = duskstudio::fileimport::importAudio (req);
        REQUIRE (imported.ok);
        const auto output = readWav (imported.region.file);
        REQUIRE (output.lengthInSamples == (std::int64_t) targetRate);
        float worst = 0.0f;
        for (int n = 256; n < output.buffer.getNumSamples() - 256; ++n)
            worst = std::max (worst, std::abs (output.buffer.getSample (0, n) - 0.125f));
        CHECK_THAT (worst, WithinAbs (0.0f, 0.00002f));
    }
}

TEST_CASE ("FileImporter: downsampled impulses retain timeline and grain-boundary alignment",
           "[FileImporter][resample][antialias]")
{
    for (const double sourceRate : { 96000.0, 88200.0 })
    {
        CAPTURE (sourceRate);
        constexpr double targetRate = 48000.0;
        const double ratio = sourceRate / targetRate;
        const int length = (int) (sourceRate * 2.0) + 123;
        const std::array<int, 3> positions { 128, (int) std::llround (65536.0 * ratio) + 8, length - 128 };
        TempScope tmp;
        const auto src = tmp.dir.getChildFile ("impulses.wav");
        writeTestWav (src, sourceRate, 2, length, [&] (int channel, int n)
        {
            for (const int at : positions)
                if (n == at + channel * 31) return channel == 0 ? 0.5f : -0.75f;
            return 0.0f;
        }, 32);
        duskstudio::fileimport::AudioImportRequest req;
        req.source = src;
        req.audioDir = tmp.dir;
        req.sessionSampleRate = targetRate;
        req.targetChannels = 2;
        req.timelineStart = 9876;
        const auto imported = duskstudio::fileimport::importAudio (req);
        REQUIRE (imported.ok);
        const auto output = readWav (imported.region.file);
        REQUIRE (output.lengthInSamples == (std::int64_t) std::llround ((double) length / ratio));
        CHECK (imported.region.timelineStart == 9876);
        CHECK (imported.region.sourceOffset == 0);
        for (int channel = 0; channel < 2; ++channel)
            for (const int at : positions)
            {
                const int expected = (int) std::llround ((double) (at + channel * 31) / ratio);
                int peakAt = expected - 8;
                const auto* samples = output.buffer.getReadPointer (channel);
                for (int n = expected - 7; n <= expected + 8; ++n)
                    if (std::abs (samples[n]) > std::abs (samples[peakAt])) peakAt = n;
                CHECK (std::abs (peakAt - expected) <= 1);
                CHECK (std::abs (samples[peakAt]) > 0.05f);
                CHECK ((samples[peakAt] > 0.0f) == (channel == 0));
            }
    }
}

TEST_CASE ("FileImporter: stereo -> mono sums L+R at 0.5 each",
           "[FileImporter][issue-377]")
{
    TempScope tmp;
    const auto src = tmp.dir.getChildFile ("source.wav");
    constexpr double kSr = 48000.0;
    constexpr int    kLen = 4800;   // 0.1 s

    // L = +0.5 constant, R = -0.5 constant => mono should be near zero
    // everywhere after the 0.5/0.5 sum.
    writeTestWav (src, kSr, 2, kLen, [] (int c, int)
    {
        return c == 0 ? 0.5f : -0.5f;
    });

    duskstudio::fileimport::AudioImportRequest req;
    req.source            = src;
    req.audioDir          = tmp.dir;
    req.sessionSampleRate = kSr;
    req.targetChannels    = 1;

    const auto res = duskstudio::fileimport::importAudio (req);
    REQUIRE (res.ok);
    const auto rb = readWav (res.region.file);
    REQUIRE (rb.numChannels == 1);
    REQUIRE_THAT (bufferPeak (rb.buffer, 0), WithinAbs (0.0f, 1.0e-3f));
}

TEST_CASE ("FileImporter: mono -> stereo duplicates to L and R",
           "[FileImporter][issue-377]")
{
    TempScope tmp;
    const auto src = tmp.dir.getChildFile ("source.wav");
    constexpr double kSr = 48000.0;
    constexpr int    kLen = 4800;
    constexpr float  kAmp = 0.4f;

    writeTestWav (src, kSr, 1, kLen, [&] (int, int) { return kAmp; });

    duskstudio::fileimport::AudioImportRequest req;
    req.source            = src;
    req.audioDir          = tmp.dir;
    req.sessionSampleRate = kSr;
    req.targetChannels    = 2;

    const auto res = duskstudio::fileimport::importAudio (req);
    REQUIRE (res.ok);
    const auto rb = readWav (res.region.file);
    REQUIRE (rb.numChannels == 2);
    // 24-bit WAV round-trip loses ~1 ULP at this amplitude.
    REQUIRE_THAT (bufferPeak (rb.buffer, 0), WithinAbs (kAmp, 1.0e-4f));
    REQUIRE_THAT (bufferPeak (rb.buffer, 1), WithinAbs (kAmp, 1.0e-4f));
}

TEST_CASE ("FileImporter: matching rate and channels copies the source verbatim",
           "[FileImporter][issue-377]")
{
    // When the source already matches the session rate AND the requested
    // channel layout, the importer must copy it byte-for-byte — no decode,
    // resample, or re-encode — so it never alters audio the user didn't ask
    // to change. Asserts the imported file is bit-identical to the source.
    TempScope tmp;
    const auto src = tmp.dir.getChildFile ("match.wav");
    constexpr double kSr  = 48000.0;
    constexpr int    kLen = 4800;
    writeTestWav (src, kSr, 2, kLen, [&] (int c, int n)
    {
        return (c == 0 ? 0.3f : -0.2f)
             * (float) std::sin (2.0 * kPi * 330.0 * (double) n / kSr);
    });

    const auto audioDir = tmp.dir.getChildFile ("audio");
    audioDir.createDirectory();

    duskstudio::fileimport::AudioImportRequest req;
    req.source            = src;
    req.audioDir          = audioDir;
    req.sessionSampleRate = kSr;   // matches source
    req.targetChannels    = 2;     // matches source channels
    req.timelineStart     = 7;

    const auto res = duskstudio::fileimport::importAudio (req);
    REQUIRE (res.ok);
    REQUIRE (res.region.file.existsAsFile());
    REQUIRE (res.region.file != src);                              // copied in, not referenced
    REQUIRE (res.region.file.getParentDirectory() == audioDir);    // into the session audio dir

    juce::MemoryBlock srcBytes, outBytes;
    REQUIRE (src.loadFileAsData (srcBytes));
    REQUIRE (res.region.file.loadFileAsData (outBytes));
    REQUIRE (srcBytes.getSize() == outBytes.getSize());
    REQUIRE (srcBytes == outBytes);

    REQUIRE (res.region.numChannels     == 2);
    REQUIRE (res.region.lengthInSamples == kLen);
    REQUIRE (res.region.timelineStart   == 7);
}

TEST_CASE ("FileImporter: a matching 32-bit source is not truncated to 24-bit",
           "[FileImporter][issue-377]")
{
    // The old path decoded to float and always re-wrote 24-bit, silently
    // truncating a >24-bit source. The verbatim copy preserves the original
    // bytes, so a 32-bit file survives intact.
    TempScope tmp;
    const auto src = tmp.dir.getChildFile ("wide.wav");
    constexpr double kSr  = 48000.0;
    constexpr int    kLen = 4800;
    writeTestWav (src, kSr, 1, kLen, [&] (int, int n)
    {
        return 0.123456789f * (float) std::sin (2.0 * kPi * 100.0 * (double) n / kSr);
    }, /*bitsPerSample*/ 32);

    duskstudio::fileimport::AudioImportRequest req;
    req.source            = src;
    req.audioDir          = tmp.dir;
    req.sessionSampleRate = kSr;
    req.targetChannels    = 1;

    const auto res = duskstudio::fileimport::importAudio (req);
    REQUIRE (res.ok);

    juce::MemoryBlock srcBytes, outBytes;
    REQUIRE (src.loadFileAsData (srcBytes));
    REQUIRE (res.region.file.loadFileAsData (outBytes));
    REQUIRE (srcBytes == outBytes);   // 32-bit data preserved, not down-converted
}
