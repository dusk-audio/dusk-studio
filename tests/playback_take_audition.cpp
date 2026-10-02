#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "TestTempDirectory.h"
#include "engine/PlaybackEngine.h"
#include "engine/Transport.h"
#include "engine/audiofile/FileWriter.h"
#include "foundation/Decibels.h"
#include "session/Session.h"

#include <filesystem>
#include <memory>
#include <vector>

using namespace duskstudio;
using Catch::Matchers::WithinAbs;

namespace
{
juce::File writeConstantMono (const std::filesystem::path& dir, const char* name, float level, int frames)
{
    const auto path = dir / "audio" / name;
    std::filesystem::create_directories (path.parent_path());
    auto writer = dusk::audio::FileWriter::create (path, { 48000.0, 1, 24 });
    REQUIRE (writer != nullptr);
    const std::vector<float> samples ((std::size_t) frames, level);
    const float* channels[] { samples.data() };
    REQUIRE (writer->write (channels, 1, frames));
    writer.reset();
    return juce::File (juce::String::fromUTF8 (path.u8string().c_str()));
}
} // namespace

// An auditioned take replaces its track's regions with the take alone, whole,
// unfaded and at unity; every other read of the track is silence. Read through
// the loop-start pre-cache, which preparePlayback fills synchronously, so the
// result does not race the reader's background prefetch.
TEST_CASE ("an auditioned take plays alone in place of its track's regions",
           "[playback][take]")
{
    constexpr int kFrames    = 4096;
    constexpr int kTakeStart = 1024;
    constexpr int kTakeLen   = 1024;
    constexpr float kLevelA  = 0.5f;
    constexpr float kLevelB  = -0.25f;

    const test::TempDirectory dir ("dusk-audition-");
    const auto fileA = writeConstantMono (dir.path(), "a.wav", kLevelA, kFrames);
    const auto fileB = writeConstantMono (dir.path(), "b.wav", kLevelB, kFrames);

    auto session = std::make_unique<Session>();
    session->setSessionDirectory (fileA.getParentDirectory().getParentDirectory());
    auto& track = session->track (2);
    {
        AudioRegion r;
        r.file            = fileA;
        r.timelineStart   = 0;
        r.lengthInSamples = kFrames;
        r.gainDb          = -6.0f;
        r.takeId          = 1;
        track.regions.push_back (r);

        AudioTake a;
        a.id = 1;
        a.file = fileA;
        a.lengthInSamples = kFrames;
        track.takes.push_back (a);

        AudioTake b;
        b.id = 2;
        b.file = fileB;
        b.timelineStart   = kTakeStart;
        b.lengthInSamples = kTakeLen;
        b.sourceOffset    = 100;
        track.takes.push_back (b);
    }
    const float regionGain = dusk::audio::decibelsToGain (-6.0f);

    Transport transport;
    transport.setLoopRange (0, kFrames);
    transport.setLoopEnabled (true);
    PlaybackEngine pe (*session);
    pe.bindTransport (transport);
    pe.prepare (kFrames);

    std::vector<float> out ((size_t) kFrames);
    auto read = [&]
    {
        std::fill (out.begin(), out.end(), 99.0f);
        pe.readForTrack (2, 0, out.data(), nullptr, kFrames, 0, kFrames);
    };

    session->takeAudition = { 2, 2 };
    pe.preparePlayback (PlaybackEngine::Audition::Honour);
    read();
    CHECK_THAT (out[0],                          WithinAbs (0.0f, 1e-6));
    CHECK_THAT (out[kTakeStart - 1],             WithinAbs (0.0f, 1e-6));
    CHECK_THAT (out[kTakeStart],                 WithinAbs (kLevelB, 2e-4));
    CHECK_THAT (out[kTakeStart + kTakeLen / 2],  WithinAbs (kLevelB, 2e-4));
    CHECK_THAT (out[kTakeStart + kTakeLen - 1],  WithinAbs (kLevelB, 2e-4));
    CHECK_THAT (out[kTakeStart + kTakeLen],      WithinAbs (0.0f, 1e-6));
    CHECK_THAT (out[kFrames - 1],                WithinAbs (0.0f, 1e-6));

    SECTION ("a render prepare never hears the audition")
    {
        pe.preparePlayback();
        read();
        CHECK_THAT (out[kTakeStart + kTakeLen / 2], WithinAbs (kLevelA * regionGain, 2e-4));
    }

    SECTION ("the live gain refresh leaves the auditioned take at unity")
    {
        track.regions[0].timelineStart = kTakeStart;
        track.regions[0].lengthInSamples = kTakeLen;
        track.regions[0].file = fileB;
        track.regions[0].muted = true;
        pe.refreshLiveRegionParams();
        read();
        CHECK_THAT (out[kTakeStart + kTakeLen / 2], WithinAbs (kLevelB, 2e-4));
    }

    SECTION ("an audition naming a take the track lacks plays the regions")
    {
        session->takeAudition = { 2, 42 };
        pe.preparePlayback (PlaybackEngine::Audition::Honour);
        read();
        CHECK_THAT (out[kTakeStart + kTakeLen / 2], WithinAbs (kLevelA * regionGain, 2e-4));
    }

    SECTION ("cleared, the track plays its regions again")
    {
        session->takeAudition = {};
        pe.preparePlayback (PlaybackEngine::Audition::Honour);
        read();
        CHECK_THAT (out[0],                         WithinAbs (kLevelA * regionGain, 2e-4));
        CHECK_THAT (out[kTakeStart + kTakeLen / 2], WithinAbs (kLevelA * regionGain, 2e-4));
        CHECK_THAT (out[kFrames - 1],               WithinAbs (kLevelA * regionGain, 2e-4));
    }

    pe.stopPlayback();
}
