#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/TransportSnapshot.h"
#include "session/Session.h"

using Catch::Matchers::WithinAbs;

// Transport is a thin atomic wrapper without explicit transition
// rejection - any state can be set from any other. These tests pin the
// observable contract (default values, setter/getter round-trip,
// playhead arithmetic) so a refactor that accidentally bakes a
// transition matrix or drops one of the storage fields gets caught
// before it ships.
TEST_CASE ("Transport: defaults at construction", "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    REQUIRE (t.getState() == Transport::State::Stopped);
    REQUIRE (t.isStopped());
    REQUIRE_FALSE (t.isPlaying());
    REQUIRE_FALSE (t.isRecording());
    REQUIRE (t.getPlayhead() == 0);
    REQUIRE_FALSE (t.isLoopEnabled());
    REQUIRE_FALSE (t.isPunchEnabled());
}

TEST_CASE ("Transport: every state setter round-trips through getState", "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    for (auto s : { Transport::State::Stopped,
                     Transport::State::Playing,
                     Transport::State::Recording,
                     Transport::State::Stopped,        // back to stop
                     Transport::State::Recording })    // skip playing intermediate
    {
        t.setState (s);
        REQUIRE (t.getState() == s);
    }
}

TEST_CASE ("Transport: playhead advance accumulates across calls", "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    t.setPlayhead (0);
    t.finishAudioBlock (t.beginAudioBlock() + 480);
    t.finishAudioBlock (t.beginAudioBlock() + 480);
    t.finishAudioBlock (t.beginAudioBlock() + 39);
    REQUIRE (t.getPlayhead() == 999);

    // setPlayhead is a direct seek, not an accumulator
    t.setPlayhead (100);
    REQUIRE (t.getPlayhead() == 100);

    t.finishAudioBlock (t.beginAudioBlock() + 50);
    REQUIRE (t.getPlayhead() == 150);

    t.setState (Transport::State::Playing);
    t.requestLocate (5000);
    REQUIRE (t.getPlayhead() == 5000);
    REQUIRE (duskstudio::snapshotTransport (t, 120.0, 48000.0).timeInSamples == 150);
    REQUIRE (t.beginAudioBlock() == 5000);
    t.requestLocate (7000);
    REQUIRE (duskstudio::snapshotTransport (t, 120.0, 48000.0).timeInSamples == 5000);
    t.finishAudioBlock (5256);
    REQUIRE (duskstudio::snapshotTransport (t, 120.0, 48000.0).timeInSamples == 5256);
    REQUIRE (t.getPlayhead() == 7000);
    REQUIRE (t.beginAudioBlock() == 7000);
}

TEST_CASE ("Transport: setPlayhead accepts negative for pre-roll cases", "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    t.setPlayhead (-48000);
    REQUIRE (t.getPlayhead() == -48000);

    // Advance from negative crosses zero cleanly
    t.finishAudioBlock (t.beginAudioBlock() + 96000);
    REQUIRE (t.getPlayhead() == 48000);
}

TEST_CASE ("Transport: loop range + enable round-trip", "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    t.setLoopRange (10000, 50000);
    t.setLoopEnabled (true);
    REQUIRE (t.isLoopEnabled());
    REQUIRE (t.getLoopStart() == 10000);
    REQUIRE (t.getLoopEnd()   == 50000);

    // Disable preserves the range so the user's last loop region
    // survives an enable/disable toggle.
    t.setLoopEnabled (false);
    REQUIRE_FALSE (t.isLoopEnabled());
    REQUIRE (t.getLoopStart() == 10000);
    REQUIRE (t.getLoopEnd()   == 50000);

    // Storage of end-before-start is allowed; the audio thread treats
    // end <= start as "loop disabled" per Transport.h comment, so the
    // setter doesn't validate. Pin that behaviour.
    t.setLoopRange (200, 100);
    REQUIRE (t.getLoopStart() == 200);
    REQUIRE (t.getLoopEnd()   == 100);
}

TEST_CASE ("Transport: punch range + enable round-trip", "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    t.setPunchRange (5000, 25000);
    t.setPunchEnabled (true);
    REQUIRE (t.isPunchEnabled());
    REQUIRE (t.getPunchIn()  == 5000);
    REQUIRE (t.getPunchOut() == 25000);

    // Independent of loop state
    t.setLoopEnabled (true);
    REQUIRE (t.isPunchEnabled());
    REQUIRE (t.isLoopEnabled());
}

TEST_CASE ("Transport: a punch bracket placed by hand arms punch once in is before out",
           "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    t.placePunchRange (5000, 5000);
    REQUIRE_FALSE (t.isPunchEnabled());

    t.placePunchRange (5000, 25000);
    REQUIRE (t.isPunchEnabled());
    REQUIRE (t.getPunchIn()  == 5000);
    REQUIRE (t.getPunchOut() == 25000);

    // An in point placed at the out point collapses the pair, which would
    // record nothing, so punch goes off until the pair is a range again.
    t.placePunchRange (25000, 25000);
    REQUIRE_FALSE (t.isPunchEnabled());
}

TEST_CASE ("Transport: a loop bracket placed by hand turns loop on once in is before out",
           "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    t.placeLoopRange (1000, 1000);
    REQUIRE_FALSE (t.isLoopEnabled());

    t.placeLoopRange (1000, 9000);
    REQUIRE (t.isLoopEnabled());

    t.placeLoopRange (9000, 9000);
    REQUIRE_FALSE (t.isLoopEnabled());
}

TEST_CASE ("Transport snapshot: recording advances the plug-in timeline", "[transport][transport-snapshot]")
{
    duskstudio::Transport transport;
    transport.setState (duskstudio::Transport::State::Recording);
    const auto position = duskstudio::snapshotTransport (transport, 120.0, 48000.0);
    CHECK (position.isPlaying);
    CHECK (position.isRecording);
}

TEST_CASE ("Transport snapshot: looping requires an enabled usable range", "[transport][transport-snapshot]")
{
    duskstudio::Transport transport;
    transport.setLoopRange (24000, 96000);
    transport.setLoopEnabled (true);
    CHECK (duskstudio::snapshotTransport (transport, 120.0, 48000.0).isLooping);

    transport.setLoopEnabled (false);
    const auto disabled = duskstudio::snapshotTransport (transport, 120.0, 48000.0);
    CHECK_FALSE (disabled.isLooping);
    CHECK (disabled.loopStartSamples == 24000);
    CHECK (disabled.loopEndSamples == 96000);
    CHECK_THAT (disabled.ppqLoopStart, WithinAbs (1.0, 1.0e-12));
    CHECK_THAT (disabled.ppqLoopEnd, WithinAbs (4.0, 1.0e-12));
    transport.setLoopEnabled (true);
    transport.setLoopRange (96000, 24000);
    CHECK_FALSE (duskstudio::snapshotTransport (transport, 120.0, 48000.0).isLooping);
    transport.setLoopRange (24000, 24000);
    CHECK_FALSE (duskstudio::snapshotTransport (transport, 120.0, 48000.0).isLooping);
}

TEST_CASE ("Transport snapshot: tempo changes preserve quarter-note phase", "[transport][transport-snapshot]")
{
    duskstudio::TempoMap map;
    map.setPoints ({ { 0, 120.0f }, { 48000, 60.0f } });
    duskstudio::Transport transport;
    transport.setState (duskstudio::Transport::State::Recording);
    transport.setPlayhead (96000);
    transport.setLoopRange (24000, 96000);
    transport.setLoopEnabled (true);
    transport.requestLocate (192000);
    auto position = duskstudio::snapshotTransport (transport, 140.0, 48000.0, 7, 8);
    duskstudio::applyTransportTempoMap (position, &map, 48000.0, duskstudio::kMidiTicksPerQuarter);

    CHECK (position.timeInSamples == 96000);
    CHECK (position.isPlaying);
    CHECK (position.isRecording);
    CHECK (position.isLooping);
    CHECK (position.timeSignatureNumerator == 7);
    CHECK (position.timeSignatureDenominator == 8);
    CHECK_THAT (position.bpm, WithinAbs (60.0, 1.0e-12));
    CHECK_THAT (position.timeInSeconds, WithinAbs (2.0, 1.0e-12));
    CHECK_THAT (position.ppqPosition, WithinAbs (3.0, 1.0e-12));
    CHECK_THAT (position.ppqLoopStart, WithinAbs (1.0, 1.0e-12));
    CHECK_THAT (position.ppqLoopEnd, WithinAbs (3.0, 1.0e-12));

    CHECK (transport.beginAudioBlock() == 192000);
    position = duskstudio::snapshotTransport (transport, 140.0, 48000.0, 7, 8);
    duskstudio::applyTransportTempoMap (position, &map, 48000.0, duskstudio::kMidiTicksPerQuarter);
    CHECK_THAT (position.ppqPosition, WithinAbs (5.0, 1.0e-12));

    const auto captured = duskstudio::snapshotTransport (duskstudio::Transport::State::Stopped,
                                                         24000, 120.0, 48000.0, 6, 8, false, 0, 48000);
    CHECK_FALSE (captured.isPlaying);
    CHECK_FALSE (captured.isRecording);
    CHECK_FALSE (captured.isLooping);
    CHECK (captured.timeInSamples == 24000);
    CHECK (captured.timeSignatureNumerator == 6);
    CHECK (captured.timeSignatureDenominator == 8);
}

TEST_CASE ("Transport snapshot: empty tempo maps retain the session tempo", "[transport][transport-snapshot]")
{
    const duskstudio::TempoMap emptyMap;
    duskstudio::Transport transport;
    transport.setPlayhead (96000);
    transport.setLoopRange (24000, 96000);
    for (const auto* map : { &emptyMap, static_cast<const duskstudio::TempoMap*> (nullptr) })
    {
        auto position = duskstudio::snapshotTransport (transport, 90.0, 48000.0, 6, 8);
        duskstudio::applyTransportTempoMap (position, map, 48000.0, duskstudio::kMidiTicksPerQuarter);
        CHECK_THAT (position.bpm, WithinAbs (90.0, 1.0e-12));
        CHECK_THAT (position.ppqPosition, WithinAbs (3.0, 1.0e-12));
        CHECK_THAT (position.ppqLoopStart, WithinAbs (0.75, 1.0e-12));
        CHECK_THAT (position.ppqLoopEnd, WithinAbs (3.0, 1.0e-12));
    }
}

TEST_CASE ("Transport snapshot: pre-roll uses the first mapped tempo", "[transport][transport-snapshot]")
{
    duskstudio::TempoMap map;
    map.setPoints ({ { 24000, 90.0f }, { 48000, 60.0f } });
    duskstudio::Transport transport;
    transport.setPlayhead (-48000);
    transport.setLoopRange (-24000, 96000);
    auto position = duskstudio::snapshotTransport (transport, 120.0, 48000.0);
    duskstudio::applyTransportTempoMap (position, &map, 48000.0, duskstudio::kMidiTicksPerQuarter);
    CHECK_THAT (position.timeInSeconds, WithinAbs (-1.0, 1.0e-12));
    CHECK_THAT (position.bpm, WithinAbs (90.0, 1.0e-12));
    CHECK_THAT (position.ppqPosition, WithinAbs (-1.5, 1.0e-12));
    CHECK_THAT (position.ppqLoopStart, WithinAbs (-0.75, 1.0e-12));
    CHECK_THAT (position.ppqLoopEnd, WithinAbs (2.5, 1.0e-12));
}
