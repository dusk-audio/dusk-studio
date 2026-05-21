#include <catch2/catch_test_macros.hpp>

#include "engine/Transport.h"

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
    t.advancePlayhead (480);
    t.advancePlayhead (480);
    t.advancePlayhead (39);
    REQUIRE (t.getPlayhead() == 999);

    // setPlayhead is a direct seek, not an accumulator
    t.setPlayhead (100);
    REQUIRE (t.getPlayhead() == 100);

    t.advancePlayhead (50);
    REQUIRE (t.getPlayhead() == 150);
}

TEST_CASE ("Transport: setPlayhead accepts negative for pre-roll cases", "[transport]")
{
    using duskstudio::Transport;

    Transport t;
    t.setPlayhead (-48000);
    REQUIRE (t.getPlayhead() == -48000);

    // Advance from negative crosses zero cleanly
    t.advancePlayhead (96000);
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
