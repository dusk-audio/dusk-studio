#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include "engine/FileImporter.h"

#include <cmath>
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
                    Gen&& gen)
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
                              (unsigned int) numChannels, 24, {}, 0));
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
        dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-fileimporter-tests")
                  .getChildFile (juce::Uuid().toDashedString());
        const auto result = dir.createDirectory();
        if (result.failed())
            throw std::runtime_error ("TempScope failed to create temp dir '"
                                       + dir.getFullPathName().toStdString()
                                       + "': " + result.getErrorMessage().toStdString());
    }
    ~TempScope() { dir.deleteRecursively(); }
};
}

TEST_CASE ("FileImporter: 44.1k mono -> 48k session preserves length", "[FileImporter]")
{
    TempScope tmp;
    const auto src = tmp.dir.getChildFile ("source.wav");
    constexpr double kSrcSr = 44100.0;
    constexpr int    kSrcLen = (int) kSrcSr;   // 1 second
    writeTestWav (src, kSrcSr, 1, kSrcLen, [&] (int, int n)
    {
        return 0.5f * (float) std::sin (2.0 * kPi * 440.0 * (double) n / kSrcSr);
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
    REQUIRE (res.errorMessage.isEmpty());

    const auto rb = readWav (res.region.file);
    REQUIRE (rb.sampleRate == 48000.0);
    REQUIRE (rb.numChannels == 1);
    // 1 s of audio at 48 kHz: 48000 samples ± tolerance for the
    // interpolator's edge behaviour.
    REQUIRE (std::abs (rb.lengthInSamples - 48000) <= 64);
    REQUIRE (res.region.lengthInSamples == rb.lengthInSamples);
    REQUIRE (res.region.numChannels == 1);
}

TEST_CASE ("FileImporter: 96k mono -> 48k session preserves length", "[FileImporter]")
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

TEST_CASE ("FileImporter: stereo -> mono sums L+R at 0.5 each", "[FileImporter]")
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

TEST_CASE ("FileImporter: mono -> stereo duplicates to L and R", "[FileImporter]")
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
