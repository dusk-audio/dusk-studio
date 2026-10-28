#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/FileImporter.h"
#include "engine/RecordManager.h"
#include "engine/audiofile/FileReader.h"
#include "engine/audiofile/FileWriter.h"
#include "session/Session.h"

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <limits>
#include <memory>
#include <optional>
#include <tuple>
#include <vector>

using Catch::Matchers::WithinAbs;

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-recording-accuracy-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}

enum class FailureStage { Drain, Flush };

struct SinkObservation
{
    std::int64_t persisted = 0;
    int flushCalls = 0;
    bool nativeOkay = true;
};

class FailingFileSink final : public dusk::audio::IFileWriteSink
{
public:
    FailingFileSink (std::unique_ptr<dusk::audio::FileWriter> targetIn,
                     SinkObservation& observationIn, FailureStage stageIn,
                     std::int64_t prefixFramesIn, int bufferFrames)
        : target (std::move (targetIn)), observation (observationIn), stage (stageIn),
          prefixFrames (prefixFramesIn),
          buffered ((size_t) bufferFrames * (size_t) target->numChannels()) {}

    int numChannels() const noexcept override { return target->numChannels(); }

    bool writeInterleaved (const float* data, std::int64_t frames) noexcept override
    {
        if (stage == FailureStage::Drain)
        {
            const auto keep = std::min (frames, prefixFrames);
            observation.nativeOkay = target->writeInterleaved (data, keep);
            if (observation.nativeOkay) observation.persisted = keep;
            return false;
        }
        const auto samples = (size_t) frames * (size_t) numChannels();
        if (samples > buffered.size() - bufferedSamples)
        {
            observation.nativeOkay = false;
            return false;
        }
        std::copy_n (data, samples, buffered.data() + bufferedSamples);
        bufferedSamples += samples;
        return true;
    }

    bool flush() override
    {
        if (finished) return false;
        finished = true;
        ++observation.flushCalls;
        const auto available = (std::int64_t) (bufferedSamples / (size_t) numChannels());
        const auto keep = std::min (available, prefixFrames);
        observation.nativeOkay = target->writeInterleaved (buffered.data(), keep)
                              && target->flush();
        if (observation.nativeOkay) observation.persisted = keep;
        return false;
    }

private:
    std::unique_ptr<dusk::audio::FileWriter> target;
    SinkObservation& observation;
    FailureStage stage;
    std::int64_t prefixFrames;
    std::vector<float> buffered;
    size_t bufferedSamples = 0;
    bool finished = false;
};

duskstudio::RecordManager::WriteSinkFactory failingWriterFactory (
    SinkObservation& observation, FailureStage stage, std::int64_t prefixFrames,
    int bufferFrames)
{
    return [&observation, stage, prefixFrames, bufferFrames] (
        const std::filesystem::path& path, const dusk::audio::WriteSpec& spec)
        -> std::unique_ptr<dusk::audio::IFileWriteSink>
    {
        auto writer = dusk::audio::FileWriter::create (path, spec);
        REQUIRE (writer != nullptr);
        if (path.filename().u8string().find ("track01_") == 0)
            return std::make_unique<FailingFileSink> (
                std::move (writer), observation, stage, prefixFrames, bufferFrames);
        return writer;
    };
}

std::int64_t fileFrames (const juce::File& file)
{
    auto reader = dusk::audio::FileReader::open (
        std::filesystem::u8path (file.getFullPathName().toStdString()));
    return reader != nullptr ? reader->info().numFrames : -1;
}
} // namespace

// RecordManager is the path between the audio thread's writeInputBlock
// and a WAV on disk. A regression that mangles channel ordering, drops
// samples, or writes the wrong sample count is invisible at the
// transport-level UI — the user only finds out when they bounce and
// hear a click or a missing chunk. Tests below drive a known ramp
// through the public API and verify the finalised WAV reads back to
// the same values within float epsilon.

TEST_CASE ("RecordManager writes mono input back accurately",
           "[recording][recordmanager]")
{
    using duskstudio::RecordManager;
    using duskstudio::Session;
    using duskstudio::Track;

    constexpr double kSampleRate = 48000.0;
    constexpr int    kNumBlocks  = 8;
    constexpr int    kBlockSize  = 256;
    constexpr int    kTotal      = kNumBlocks * kBlockSize;

    const auto dir = makeTempSessionDir();
    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, 0));

    // Push a known ramp: sample n carries value n * 1e-5f. Tiny scale
    // keeps every sample within [-1, +1] without saturating WAV's
    // 24-bit int range.
    std::vector<float> ramp ((size_t) kBlockSize, 0.0f);
    for (int b = 0; b < kNumBlocks; ++b)
    {
        for (int i = 0; i < kBlockSize; ++i)
            ramp[(size_t) i] = float (b * kBlockSize + i) * 1e-5f;
        rm.writeInputBlock (0, ramp.data(), nullptr, kBlockSize);
    }

    rm.stopRecording (kTotal);

    // stopRecording calls writers[t].reset() which destroys the
    // ThreadedWriter. JUCE's destructor blocks until the queue drains,
    // so the WAV is on disk by the time stopRecording returns — no
    // sleep needed (and CLAUDE.md test-style rules forbid them).

    // Find the produced WAV — RecordManager names it
    // track01_<stamp>.wav. We grab the first .wav in the audio dir.
    auto audioDir = session.getAudioDirectory();
    REQUIRE (audioDir.isDirectory());
    auto wavs = audioDir.findChildFiles (juce::File::findFiles, false, "*.wav");
    REQUIRE (wavs.size() == 1);

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fmt.createReaderFor (wavs[0]));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 1);
    REQUIRE ((int) reader->lengthInSamples == kTotal);

    juce::AudioBuffer<float> buf (1, kTotal);
    REQUIRE (reader->read (&buf, 0, kTotal, 0, true, false));
    for (int n = 0; n < kTotal; ++n)
    {
        const float expected = float (n) * 1e-5f;
        // 24-bit int round-trip noise floor ≈ 1 / 2^23 ≈ 1.2e-7.
        REQUIRE_THAT (buf.getSample (0, n), WithinAbs (expected, 2e-7f));
    }

    dir.deleteRecursively();
}

TEST_CASE ("RecordManager writes stereo L+R interleaving in order",
           "[recording][recordmanager]")
{
    using duskstudio::RecordManager;
    using duskstudio::Session;
    using duskstudio::Track;

    constexpr double kSampleRate = 48000.0;
    constexpr int    kNumBlocks  = 4;
    constexpr int    kBlockSize  = 128;
    constexpr int    kTotal      = kNumBlocks * kBlockSize;

    const auto dir = makeTempSessionDir();
    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, 0));

    // L = +1e-4 * n, R = -1e-4 * n. Sign disagreement catches a swap
    // bug; per-channel ramp catches a stride-by-one bug.
    std::vector<float> L ((size_t) kBlockSize, 0.0f);
    std::vector<float> R ((size_t) kBlockSize, 0.0f);
    for (int b = 0; b < kNumBlocks; ++b)
    {
        for (int i = 0; i < kBlockSize; ++i)
        {
            const int n = b * kBlockSize + i;
            L[(size_t) i] =  float (n) * 1e-4f;
            R[(size_t) i] = -float (n) * 1e-4f;
        }
        rm.writeInputBlock (0, L.data(), R.data(), kBlockSize);
    }

    rm.stopRecording (kTotal);
    // ThreadedWriter destructor (in stopRecording's teardown loop)
    // drains the queue synchronously — see comment in the mono test.

    auto wavs = session.getAudioDirectory()
                    .findChildFiles (juce::File::findFiles, false, "*.wav");
    REQUIRE (wavs.size() == 1);

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    std::unique_ptr<juce::AudioFormatReader> reader (fmt.createReaderFor (wavs[0]));
    REQUIRE (reader != nullptr);
    REQUIRE (reader->numChannels == 2);
    REQUIRE ((int) reader->lengthInSamples == kTotal);

    juce::AudioBuffer<float> buf (2, kTotal);
    REQUIRE (reader->read (&buf, 0, kTotal, 0, true, true));
    for (int n = 0; n < kTotal; ++n)
    {
        REQUIRE_THAT (buf.getSample (0, n), WithinAbs ( float (n) * 1e-4f, 2e-7f));
        REQUIRE_THAT (buf.getSample (1, n), WithinAbs (-float (n) * 1e-4f, 2e-7f));
    }

    dir.deleteRecursively();
}

TEST_CASE ("RecordManager destruction discards an active take",
           "[recording][recordmanager][teardown]")
{
    using duskstudio::RecordManager;
    using duskstudio::Session;
    using duskstudio::Track;

    constexpr double kSampleRate = 48000.0;
    constexpr int    kBlockSize  = 256;

    const auto dir = makeTempSessionDir();
    Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);

    {
        RecordManager rm (session);
        REQUIRE (rm.startRecording (kSampleRate, 0));

        std::vector<float> block ((size_t) kBlockSize, 0.25f);
        rm.writeInputBlock (0, block.data(), nullptr, kBlockSize);
    }

    REQUIRE (session.track (0).regions.empty());
    REQUIRE (session.getAudioDirectory().findChildFiles (
                 juce::File::findFiles, false, "*.wav").isEmpty());

    dir.deleteRecursively();
}

TEST_CASE ("RecordManager saves each take as a 24-bit WAV in audio/ named by track and time",
           "[recording][recordmanager]")
{
    using duskstudio::RecordManager;
    using duskstudio::Session;
    using duskstudio::Track;

    // A rate other than the 48 kHz default, so the header has to carry the
    // session's rate rather than a constant.
    constexpr double kSampleRate = 44100.0;
    constexpr int    kBlockSize  = 256;
    constexpr int    kNumBlocks  = 4;
    constexpr int    kTotal      = kNumBlocks * kBlockSize;

    const auto dir = makeTempSessionDir();
    Session session;
    session.setSessionDirectory (dir);
    session.track (2).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    session.track (11).mode.store ((int) Track::Mode::Stereo, std::memory_order_relaxed);
    session.setTrackArmed (2, true);
    session.setTrackArmed (11, true);

    const auto stampFormat = "%Y%m%d-%H%M%S";
    const auto earliest = juce::Time::getCurrentTime().formatted (stampFormat);
    RecordManager rm (session);
    REQUIRE (rm.startRecording (kSampleRate, 0));
    const auto latest = juce::Time::getCurrentTime().formatted (stampFormat);

    std::vector<float> block ((size_t) kBlockSize, 0.25f);
    for (int b = 0; b < kNumBlocks; ++b)
    {
        rm.writeInputBlock (2, block.data(), nullptr, kBlockSize);
        rm.writeInputBlock (11, block.data(), block.data(), kBlockSize);
    }
    rm.stopRecording (kTotal);

    REQUIRE (session.getAudioDirectory() == dir.getChildFile ("audio"));
    const auto wavs = session.getAudioDirectory().findChildFiles (juce::File::findFiles, false, "*");
    REQUIRE (wavs.size() == 2);

    juce::AudioFormatManager fmt;
    fmt.registerBasicFormats();
    for (const auto& [index, number, channels] : { std::tuple { 2, "03", 1 }, std::tuple { 11, "12", 2 } })
    {
        CAPTURE (index);
        const auto& regions = session.track (index).regions;
        REQUIRE (regions.size() == 1);
        const auto file = regions[0].file;
        REQUIRE (wavs.contains (file));

        // trackNN_YYYYMMDD-HHMMSS.wav, stamped while the take was being set up.
        const auto name = file.getFileName();
        CAPTURE (name);
        REQUIRE (name.startsWith (juce::String ("track") + number + "_"));
        REQUIRE (name.endsWith (".wav"));
        const auto stamp = name.fromFirstOccurrenceOf ("_", false, false).upToLastOccurrenceOf (".wav", false, false);
        REQUIRE (stamp.length() == 15);
        REQUIRE (stamp[8] == '-');
        REQUIRE (stamp.removeCharacters ("-").containsOnly ("0123456789"));
        REQUIRE (stamp.compare (earliest) >= 0);
        REQUIRE (stamp.compare (latest) <= 0);

        std::unique_ptr<juce::AudioFormatReader> reader (fmt.createReaderFor (file));
        REQUIRE (reader != nullptr);
        REQUIRE (reader->getFormatName() == "WAV file");
        REQUIRE (reader->bitsPerSample == 24);
        REQUIRE_FALSE (reader->usesFloatingPointData);
        REQUIRE_THAT (reader->sampleRate, WithinAbs (kSampleRate, 1e-9));
        REQUIRE ((int) reader->numChannels == channels);
        REQUIRE ((int) reader->lengthInSamples == kTotal);
    }

    dir.deleteRecursively();
}

TEST_CASE ("RecordManager bounds partial takes after final sink failures",
           "[recording][recordmanager][finalization]")
{
    auto stage = FailureStage::Drain;
    SECTION ("the last accepted block gets a short sink write") { stage = FailureStage::Drain; }
    SECTION ("the final flush persists only a prefix") { stage = FailureStage::Flush; }

    constexpr int frames = 256;
    const auto dir = makeTempSessionDir();
    duskstudio::Session session;
    session.setSessionDirectory (dir);
    for (int track : { 0, 1 })
    {
        session.track (track).mode.store ((int) duskstudio::Track::Mode::Mono,
                                          std::memory_order_relaxed);
        session.setTrackArmed (track, true);
    }
    SinkObservation observation;
    duskstudio::RecordManager recorder (
        session, failingWriterFactory (observation, stage, frames / 2, frames));
    REQUIRE (recorder.startRecording (48000.0, 0));
    REQUIRE (recorder.getLastSetupFailures().empty());
    const std::vector<float> block (frames, 0.25f);
    recorder.writeInputBlock (0, block.data(), nullptr, frames);
    recorder.writeInputBlock (1, block.data(), nullptr, frames);
    recorder.stopRecording (frames);

    CHECK (observation.nativeOkay);
    CHECK (observation.persisted == frames / 2);
    CHECK (observation.flushCalls == (stage == FailureStage::Flush ? 1 : 0));
    const auto& partial = session.track (0);
    REQUIRE (partial.takes.size() == 1);
    REQUIRE (partial.regions.size() == 1);
    CHECK (partial.takes.front().lengthInSamples == frames / 2);
    CHECK (partial.regions.front().lengthInSamples == frames / 2);
    CHECK (fileFrames (partial.takes.front().file) == frames / 2);
    const auto& good = session.track (1);
    REQUIRE (good.takes.size() == 1);
    REQUIRE (good.regions.size() == 1);
    CHECK (good.takes.front().lengthInSamples == frames);
    CHECK (fileFrames (good.takes.front().file) == frames);
    const auto& errors = recorder.getLastRecordErrors();
    REQUIRE (errors.size() == 1);
    CHECK (errors.front().trackIndex == 0);
    CHECK (errors.front().kind == duskstudio::RecordManager::RecordErrorKind::WavWrite);
    CHECK (errors.front().count == 1);
    dir.deleteRecursively();
}

TEST_CASE ("RecordManager keeps only complete persisted loop passes after a final flush failure",
           "[recording][recordmanager][finalization]")
{
    constexpr int passFrames = 64;
    int prefixFrames = passFrames + passFrames / 2;
    SECTION ("the final persisted pass is partial") { prefixFrames = passFrames + passFrames / 2; }
    SECTION ("the prefix ends at a complete pass") { prefixFrames = passFrames; }
    SECTION ("no complete pass persisted") { prefixFrames = passFrames / 2; }

    const auto dir = makeTempSessionDir();
    duskstudio::Session session;
    session.setSessionDirectory (dir);
    for (int track : { 0, 1 })
    {
        session.track (track).mode.store ((int) duskstudio::Track::Mode::Mono,
                                          std::memory_order_relaxed);
        session.setTrackArmed (track, true);
    }
    SinkObservation observation;
    duskstudio::RecordManager recorder (session, failingWriterFactory (
        observation, FailureStage::Flush, prefixFrames, 2 * passFrames));
    const duskstudio::RecordManager::LoopCapturePlan plan { true, 0, passFrames, 0, passFrames };
    REQUIRE (recorder.startRecording (48000.0, 0, 0, plan));
    const std::vector<float> block (passFrames, 0.25f);
    for (int pass = 1; pass <= 2; ++pass)
    {
        const auto span = recorder.coordinateLoopCaptureSpan (pass, 0, 0, passFrames);
        REQUIRE (span.numSamples == passFrames);
        recorder.beginLoopCaptureSpan (span);
        for (int track : { 0, 1 })
            recorder.writeInputBlock (track, block.data(), nullptr, passFrames, &span);
    }
    recorder.stopRecording (passFrames);

    CHECK (observation.nativeOkay);
    CHECK (observation.persisted == prefixFrames);
    CHECK (observation.flushCalls == 1);
    const auto& partial = session.track (0);
    CHECK (partial.takes.size() == (size_t) (prefixFrames / passFrames));
    CHECK (partial.regions.size() == (size_t) (prefixFrames / passFrames));
    if (! partial.takes.empty())
    {
        CHECK (partial.takes.front().lengthInSamples == passFrames);
        CHECK (partial.takes.front().sourceOffset == 0);
        CHECK (partial.regions.front().provenance.loopPassOrdinal == 1);
        CHECK (fileFrames (partial.takes.front().file) == prefixFrames);
    }
    const auto& good = session.track (1);
    REQUIRE (good.takes.size() == 2);
    REQUIRE (good.regions.size() == 1);
    CHECK (good.regions.front().provenance.loopPassOrdinal == 2);
    CHECK (fileFrames (good.takes.front().file) == 2 * passFrames);
    const auto& errors = recorder.getLastRecordErrors();
    REQUIRE (errors.size() == 1);
    CHECK (errors.front().trackIndex == 0);
    CHECK (errors.front().kind == duskstudio::RecordManager::RecordErrorKind::WavWrite);
    CHECK (errors.front().count == 1);
    dir.deleteRecursively();
}

// Stop reopens every finished take to count the frames that reached disk. On
// Windows an indexer or antivirus scan can hold a just-closed file, and any
// platform can run out of descriptors; a check that cannot run must not cost
// the user a complete take.
TEST_CASE ("RecordManager keeps a finished take its file check cannot reopen",
           "[recording][recordmanager][finalization]")
{
    constexpr int frames = 256;
    int refusals = 0;
    bool sinkFails = false;
    SECTION ("the file opens on a later attempt") { refusals = 2; }
    SECTION ("the file never opens") { refusals = std::numeric_limits<int>::max(); }
    SECTION ("the file never opens and the sink reported a failure")
    {
        refusals = std::numeric_limits<int>::max();
        sinkFails = true;
    }

    const auto dir = makeTempSessionDir();
    duskstudio::Session session;
    session.setSessionDirectory (dir);
    for (int track : { 0, 1 })
    {
        session.track (track).mode.store ((int) duskstudio::Track::Mode::Mono,
                                          std::memory_order_relaxed);
        session.setTrackArmed (track, true);
    }
    SinkObservation observation;
    duskstudio::RecordManager::WriteSinkFactory sinks;
    if (sinkFails)
        sinks = failingWriterFactory (observation, FailureStage::Flush, frames, frames);
    int attempts = 0;
    duskstudio::RecordManager recorder (
        session, sinks,
        [&] (const std::filesystem::path& path) -> std::optional<std::int64_t>
        {
            if (path.filename().u8string().find ("track01_") == 0 && attempts++ < refusals)
                return std::nullopt;
            auto reader = dusk::audio::FileReader::open (path);
            if (reader == nullptr) return std::nullopt;
            return reader->info().numFrames;
        });
    REQUIRE (recorder.startRecording (48000.0, 0));
    const std::vector<float> block (frames, 0.25f);
    recorder.writeInputBlock (0, block.data(), nullptr, frames);
    recorder.writeInputBlock (1, block.data(), nullptr, frames);
    recorder.stopRecording (frames);

    const auto wavs = session.getAudioDirectory().findChildFiles (
        juce::File::findFiles, false, "track01_*.wav");
    REQUIRE (wavs.size() == 1);
    CHECK (fileFrames (wavs.getFirst()) == frames);
    const auto& checked = session.track (0);
    const auto& errors = recorder.getLastRecordErrors();
    if (sinkFails)
    {
        CHECK (checked.takes.empty());
        CHECK (checked.regions.empty());
        REQUIRE (errors.size() == 1);
        CHECK (errors.front().trackIndex == 0);
        CHECK (errors.front().kind == duskstudio::RecordManager::RecordErrorKind::WavWrite);
    }
    else
    {
        CHECK (attempts == std::min (refusals,
                                     duskstudio::fileimport::detail::kTransientFileAttempts - 1) + 1);
        REQUIRE (checked.takes.size() == 1);
        REQUIRE (checked.regions.size() == 1);
        CHECK (checked.takes.front().file == wavs.getFirst());
        CHECK (checked.takes.front().lengthInSamples == frames);
        CHECK (checked.regions.front().lengthInSamples == frames);
        CHECK (errors.empty());
    }
    const auto& good = session.track (1);
    REQUIRE (good.takes.size() == 1);
    CHECK (good.takes.front().lengthInSamples == frames);
    dir.deleteRecursively();
}

// The writer and the read-back must name the same file. A session folder whose
// name leaves ASCII is where a narrow-string path conversion would part them.
TEST_CASE ("RecordManager records into a session folder named outside ASCII",
           "[recording][recordmanager][finalization]")
{
    const auto parent = makeTempSessionDir();
    const auto dir = parent.getChildFile (juce::CharPointer_UTF8 ("Jos\xc3\xa9 \xe6\x97\xa5\xe6\x9c\xac"));
    REQUIRE (dir.createDirectory().wasOk());
    duskstudio::Session session;
    session.setSessionDirectory (dir);
    session.track (0).mode.store ((int) duskstudio::Track::Mode::Mono, std::memory_order_relaxed);
    session.setTrackArmed (0, true);
    std::filesystem::path opened;
    duskstudio::RecordManager recorder (
        session,
        [&opened] (const std::filesystem::path& path, const dusk::audio::WriteSpec& spec)
            -> std::unique_ptr<dusk::audio::IFileWriteSink>
        {
            opened = path;
            return dusk::audio::FileWriter::create (path, spec);
        });
    REQUIRE (recorder.startRecording (48000.0, 0));
    REQUIRE (recorder.getLastSetupFailures().empty());
    constexpr int frames = 128;
    const std::vector<float> block (frames, 0.25f);
    recorder.writeInputBlock (0, block.data(), nullptr, frames);
    recorder.stopRecording (frames);

    const auto& track = session.track (0);
    REQUIRE (track.takes.size() == 1);
    const auto take = track.takes.front().file;
    CHECK (opened == std::filesystem::u8path (take.getFullPathName().toStdString()));
    CHECK (take.getParentDirectory() == dir.getChildFile ("audio"));
    CHECK (fileFrames (take) == frames);
    CHECK (recorder.getLastRecordErrors().empty());
    parent.deleteRecursively();
}
