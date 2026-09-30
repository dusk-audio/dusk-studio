#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "engine/PlaybackEngine.h"
#include "engine/Transport.h"
#include "engine/audiofile/FileWriter.h"
#include "session/Session.h"

#include <filesystem>
#include <memory>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
constexpr int kFrames = 8192;
constexpr int kBlock = 256;
constexpr int kTrack = 2;
constexpr float kLevelA = 0.5f;
constexpr float kLevelB = -0.25f;
constexpr float kLevelC = 0.25f;

juce::File writeConstantMono (const std::filesystem::path& dir, const char* name, float level)
{
    const auto path = dir / "audio" / name;
    std::filesystem::create_directories (path.parent_path());
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 1, 32 });
    REQUIRE (writer != nullptr);
    const std::vector<float> samples ((std::size_t) kFrames, level);
    const float* channels[] { samples.data() };
    REQUIRE (writer->write (channels, 1, kFrames));
    writer.reset();
    return juce::File (juce::String::fromUTF8 (path.u8string().c_str()));
}

AudioRegion wholeRegion (const juce::File& file)
{
    AudioRegion r;
    r.file = file;
    r.lengthInSamples = kFrames;
    return r;
}

// A session whose track plays file A, rolling, with readers that fill as they are
// built so a rebuild is ready the moment it is made.
struct Rig
{
    test::TempDirectory dir { "dusk-live-swap-" };
    juce::File a = writeConstantMono (dir.path(), "a.wav", kLevelA);
    juce::File b = writeConstantMono (dir.path(), "b.wav", kLevelB);
    juce::File c = writeConstantMono (dir.path(), "c.wav", kLevelC);
    std::unique_ptr<Session> session = std::make_unique<Session>();
    Transport transport;
    PlaybackEngine engine { *session };
    std::int64_t playhead = 0;

    Rig()
    {
        session->setSessionDirectory (a.getParentDirectory().getParentDirectory());
        session->track (kTrack).regions = { wholeRegion (a) };
        engine.bindTransport (transport);
        engine.setSynchronousReadersForTest (true);
        engine.prepare (kBlock);
        engine.preparePlayback();
    }

    ~Rig() { engine.stopPlayback(); }

    // The next `blocks` blocks from the playhead, as the audio thread reads them.
    std::vector<float> play (int blocks)
    {
        std::vector<float> out ((std::size_t) (blocks * kBlock));
        for (int i = 0; i < blocks; ++i)
        {
            transport.setPlayhead (playhead);
            engine.readForTrack (kTrack, playhead, out.data() + i * kBlock, nullptr, kBlock);
            playhead += kBlock;
        }
        return out;
    }

    void rebuildWith (const juce::File& file)
    {
        transport.setPlayhead (playhead);
        session->track (kTrack).regions = { wholeRegion (file) };
        engine.refreshTrackPlayback (kTrack, PlaybackEngine::Audition::Honour);
    }
};

// The first sample of the fade is the old audio, the last the new, and between
// them each sample lies between the two, moving one way only.
void requireFade (const std::vector<float>& out, std::size_t at, float from, float to)
{
    constexpr std::size_t fade = 512;
    REQUIRE (out.size() >= at + fade + 1);
    CHECK_THAT (out[at], WithinAbs (from, 1e-4));
    CHECK_THAT (out[at + fade / 2], WithinAbs ((from + to) * 0.5f, 1e-3));
    CHECK_THAT (out[at + fade], WithinAbs (to, 1e-4));
    for (std::size_t i = at + 1; i <= at + fade; ++i)
    {
        INFO ("sample " << i);
        REQUIRE ((to < from ? out[i] <= out[i - 1] + 1e-6f : out[i] >= out[i - 1] - 1e-6f));
    }
}
} // namespace

TEST_CASE ("A track rebuilt while playing crossfades from its old audio to its new",
           "[playback][live-swap]")
{
    Rig rig;
    const auto before = rig.play (1);
    CHECK_THAT (before.front(), WithinAbs (kLevelA, 1e-4));
    const int streams = PlaybackEngine::liveStreamCountForTest();

    rig.rebuildWith (rig.b);
    const auto out = rig.play (4);
    requireFade (out, 0, kLevelA, kLevelB);
    CHECK_THAT (out.back(), WithinAbs (kLevelB, 1e-4));

    // The old streams went back to the message thread, which frees them.
    rig.engine.service();
    CHECK (PlaybackEngine::liveStreamCountForTest() == streams);
}

TEST_CASE ("A rebuild that lands during a fade waits for the fade to finish",
           "[playback][live-swap]")
{
    Rig rig;
    rig.play (1);
    rig.rebuildWith (rig.b);
    auto out = rig.play (1);
    rig.rebuildWith (rig.c);
    const auto more = rig.play (5);
    out.insert (out.end(), more.begin(), more.end());

    requireFade (out, 0, kLevelA, kLevelB);
    requireFade (out, 512, kLevelB, kLevelC);
    CHECK_THAT (out.back(), WithinAbs (kLevelC, 1e-4));
}

TEST_CASE ("A rebuild the audio thread has not taken yet gives way to a newer one",
           "[playback][live-swap]")
{
    Rig rig;
    rig.play (1);
    const int streams = PlaybackEngine::liveStreamCountForTest();
    rig.rebuildWith (rig.b);
    rig.rebuildWith (rig.c);
    CHECK (PlaybackEngine::liveStreamCountForTest() == streams + 1);

    const auto out = rig.play (4);
    requireFade (out, 0, kLevelA, kLevelC);
}

TEST_CASE ("A track rebuilt with nothing to play fades to silence", "[playback][live-swap]")
{
    Rig rig;
    rig.play (1);
    rig.session->track (kTrack).regions.clear();
    rig.engine.refreshTrackPlayback (kTrack, PlaybackEngine::Audition::Honour);
    const auto out = rig.play (4);
    requireFade (out, 0, kLevelA, 0.0f);
    CHECK_THAT (out.back(), WithinAbs (0.0f, 1e-6));
}

TEST_CASE ("Stopping frees every stream a track holds, taken or waiting", "[playback][live-swap]")
{
    const int outside = PlaybackEngine::liveStreamCountForTest();
    Rig rig;
    rig.play (1);
    rig.rebuildWith (rig.b);
    rig.play (1);
    rig.rebuildWith (rig.c);
    rig.engine.stopPlayback();
    CHECK (PlaybackEngine::liveStreamCountForTest() == outside);

    // Stopped, a rebuild does nothing: Play builds the streams afresh.
    rig.rebuildWith (rig.a);
    CHECK (PlaybackEngine::liveStreamCountForTest() == outside);
}
