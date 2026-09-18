#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"

using namespace duskstudio;

namespace
{
void setMode (Session& s, int track, Track::Mode mode)
{
    s.track (track).mode.store ((int) mode, std::memory_order_relaxed);
}
} // namespace

// Arming an audio track with no capture channels used to light ARM over a
// recording that wrote no file and reported nothing.
TEST_CASE ("An audio track cannot arm while the device offers no inputs")
{
    Session session;
    setMode (session, 0, Track::Mode::Mono);
    session.deviceCaptureChannels.store (0, std::memory_order_relaxed);

    session.setTrackArmed (0, true);

    CHECK_FALSE (session.track (0).recordArmed.load (std::memory_order_relaxed));
    CHECK_FALSE (session.anyTrackArmed());
    CHECK_FALSE (session.canArmAudioTracks());
}

TEST_CASE ("An audio track arms once the device reports capture channels")
{
    Session session;
    setMode (session, 0, Track::Mode::Stereo);
    session.deviceCaptureChannels.store (2, std::memory_order_relaxed);

    session.setTrackArmed (0, true);

    CHECK (session.track (0).recordArmed.load (std::memory_order_relaxed));
    CHECK (session.anyTrackArmed());
    CHECK (session.canArmAudioTracks());
}

// A MIDI track captures from a MIDI input, so the audio device's capture width
// says nothing about whether it can record.
TEST_CASE ("A MIDI track arms with no audio inputs")
{
    Session session;
    setMode (session, 3, Track::Mode::Midi);
    session.deviceCaptureChannels.store (0, std::memory_order_relaxed);

    session.setTrackArmed (3, true);

    CHECK (session.track (3).recordArmed.load (std::memory_order_relaxed));
    CHECK (session.anyTrackArmed());
}

TEST_CASE ("Disarming is never gated by the capture width")
{
    Session session;
    setMode (session, 1, Track::Mode::Mono);
    session.deviceCaptureChannels.store (2, std::memory_order_relaxed);
    session.setTrackArmed (1, true);
    REQUIRE (session.track (1).recordArmed.load (std::memory_order_relaxed));

    session.deviceCaptureChannels.store (0, std::memory_order_relaxed);
    session.setTrackArmed (1, false);

    CHECK_FALSE (session.track (1).recordArmed.load (std::memory_order_relaxed));
    CHECK_FALSE (session.anyTrackArmed());
}

// A device change that takes the inputs away must not leave ARM lit.
TEST_CASE ("Losing the capture channels disarms the audio tracks and spares MIDI")
{
    Session session;
    setMode (session, 0, Track::Mode::Mono);
    setMode (session, 1, Track::Mode::Stereo);
    setMode (session, 2, Track::Mode::Midi);
    session.deviceCaptureChannels.store (4, std::memory_order_relaxed);
    session.setTrackArmed (0, true);
    session.setTrackArmed (1, true);
    session.setTrackArmed (2, true);
    REQUIRE (session.anyTrackArmed());

    session.deviceCaptureChannels.store (0, std::memory_order_relaxed);
    const int disarmed = session.disarmAudioTracksWithoutInput();

    CHECK (disarmed == 2);
    CHECK_FALSE (session.track (0).recordArmed.load (std::memory_order_relaxed));
    CHECK_FALSE (session.track (1).recordArmed.load (std::memory_order_relaxed));
    CHECK (session.track (2).recordArmed.load (std::memory_order_relaxed));
    CHECK (session.anyTrackArmed());
}

TEST_CASE ("A frozen track still refuses to arm with inputs available")
{
    Session session;
    setMode (session, 0, Track::Mode::Mono);
    session.deviceCaptureChannels.store (2, std::memory_order_relaxed);
    session.track (0).frozen.store (true, std::memory_order_relaxed);

    session.setTrackArmed (0, true);

    CHECK_FALSE (session.track (0).recordArmed.load (std::memory_order_relaxed));
}

// Before any device has started there is no evidence either way, and refusing
// to arm then would block on a question the engine has not answered.
TEST_CASE ("Arming is allowed before a device has reported its capture width")
{
    Session session;
    setMode (session, 0, Track::Mode::Mono);
    REQUIRE (session.deviceCaptureChannels.load (std::memory_order_relaxed)
               == Session::kCaptureWidthUnknown);

    session.setTrackArmed (0, true);

    CHECK (session.track (0).recordArmed.load (std::memory_order_relaxed));
    CHECK (session.canArmAudioTracks());
}

// A template routes each track to its own input number, so on a one-input
// device the second track asked for a channel the device does not have: ARM lit,
// recording showed a region, and Stop wrote nothing.
TEST_CASE ("A track whose input the device does not offer cannot arm")
{
    Session session;
    setMode (session, 1, Track::Mode::Mono);
    session.deviceCaptureChannels.store (1, std::memory_order_relaxed);

    CHECK (session.missingInputForTrack (1) == 1);
    session.setTrackArmed (1, true);
    CHECK_FALSE (session.track (1).recordArmed.load (std::memory_order_relaxed));

    SECTION ("choosing an input the device has lets it arm")
    {
        session.track (1).inputSource.store (0, std::memory_order_relaxed);
        CHECK (session.missingInputForTrack (1) == Session::kInputAvailable);
        session.setTrackArmed (1, true);
        CHECK (session.track (1).recordArmed.load (std::memory_order_relaxed));
    }

    SECTION ("an explicit input past the device is refused the same way")
    {
        session.track (1).inputSource.store (5, std::memory_order_relaxed);
        CHECK (session.missingInputForTrack (1) == 5);
        session.setTrackArmed (1, true);
        CHECK_FALSE (session.track (1).recordArmed.load (std::memory_order_relaxed));
    }
}

TEST_CASE ("A track set to no input cannot arm")
{
    Session session;
    setMode (session, 0, Track::Mode::Mono);
    session.deviceCaptureChannels.store (8, std::memory_order_relaxed);
    session.track (0).inputSource.store (-1, std::memory_order_relaxed);

    CHECK (session.missingInputForTrack (0) == Session::kNoInputSelected);
    session.setTrackArmed (0, true);
    CHECK_FALSE (session.track (0).recordArmed.load (std::memory_order_relaxed));
}

TEST_CASE ("A stereo track needs both of its inputs on the device")
{
    Session session;
    setMode (session, 0, Track::Mode::Stereo);
    session.deviceCaptureChannels.store (1, std::memory_order_relaxed);

    CHECK (session.missingInputForTrack (0) == 1);
    session.setTrackArmed (0, true);
    CHECK_FALSE (session.track (0).recordArmed.load (std::memory_order_relaxed));

    session.deviceCaptureChannels.store (2, std::memory_order_relaxed);
    session.setTrackArmed (0, true);
    CHECK (session.track (0).recordArmed.load (std::memory_order_relaxed));
}

TEST_CASE ("A narrower device disarms only the tracks it cannot feed")
{
    Session session;
    for (int t = 0; t < 3; ++t)
        setMode (session, t, Track::Mode::Mono);
    session.deviceCaptureChannels.store (4, std::memory_order_relaxed);
    for (int t = 0; t < 3; ++t)
        session.setTrackArmed (t, true);
    REQUIRE (session.track (2).recordArmed.load (std::memory_order_relaxed));

    session.deviceCaptureChannels.store (1, std::memory_order_relaxed);
    const int disarmed = session.disarmAudioTracksWithoutInput();

    CHECK (disarmed == 2);
    CHECK (session.track (0).recordArmed.load (std::memory_order_relaxed));
    CHECK_FALSE (session.track (1).recordArmed.load (std::memory_order_relaxed));
    CHECK_FALSE (session.track (2).recordArmed.load (std::memory_order_relaxed));
    CHECK (session.anyTrackArmed());
}

// The strip names R after wherever L resolves, so the label the mixer shows is
// only right while this pairing is.
TEST_CASE ("A stereo track's right input follows wherever the left one resolves")
{
    Session session;
    setMode (session, 2, Track::Mode::Stereo);

    CHECK (session.resolveInputRForTrack (2) == 3);

    session.track (2).inputSource.store (0, std::memory_order_relaxed);
    CHECK (session.resolveInputRForTrack (2) == 1);

    session.deviceCaptureChannels.store (2, std::memory_order_relaxed);
    CHECK (session.missingInputForTrack (2) == Session::kInputAvailable);
}
