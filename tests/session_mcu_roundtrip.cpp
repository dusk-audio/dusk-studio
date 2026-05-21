#include <catch2/catch_test_macros.hpp>

#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

namespace
{
juce::File makeTempSessionDir()
{
    auto dir = juce::File::getSpecialLocation (juce::File::tempDirectory)
                  .getChildFile ("dusk-studio-mcu-roundtrip-"
                                    + juce::String (juce::Random::getSystemRandom().nextInt()));
    dir.createDirectory();
    return dir;
}
} // namespace

// MCU surface identifiers + the user's last assign-mode choice persist
// in session.json. Bank + selected channel are deliberately ephemeral
// (a fresh launch starts on bank 0 / ch 0) so this test asserts the
// serializer round-trips the persistent fields AND that the ephemeral
// ones reset to their constructor defaults on a fresh Session load.
TEST_CASE ("SessionSerializer round-trips MCU input/output identifiers + assign mode",
           "[session][serializer][mcu]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    Session a;
    a.mcu.inputIdentifier  = "mackie-control-input";
    a.mcu.outputIdentifier = "mackie-control-output";
    a.mcu.assignMode.store (5, std::memory_order_relaxed);    // EQ
    a.mcu.bank.store (1, std::memory_order_relaxed);          // ephemeral
    a.mcu.selectedChannel.store (12, std::memory_order_relaxed); // ephemeral

    REQUIRE (SessionSerializer::save (a, target));

    Session b;
    REQUIRE (SessionSerializer::load (b, target));

    REQUIRE (b.mcu.inputIdentifier  == "mackie-control-input");
    REQUIRE (b.mcu.outputIdentifier == "mackie-control-output");
    REQUIRE (b.mcu.assignMode.load (std::memory_order_relaxed) == 5);

    // Ephemeral fields stay at constructor defaults regardless of what
    // was on the saving session. Bank and selected channel always
    // reset to 0 / 0 on a fresh launch so the controller doesn't
    // wake up mismatched with the on-screen mixer's reset state.
    REQUIRE (b.mcu.bank.load (std::memory_order_relaxed) == 0);
    REQUIRE (b.mcu.selectedChannel.load (std::memory_order_relaxed) == 0);

    // Resolved indices are runtime state recomputed at device open
    // (rebuildMidiInputBank / rebuildMidiOutputBank). Always -1 on a
    // freshly-loaded Session with no engine attached.
    REQUIRE (b.mcu.resolvedInputIdx.load  (std::memory_order_relaxed) == -1);
    REQUIRE (b.mcu.resolvedOutputIdx.load (std::memory_order_relaxed) == -1);
}

// Assign mode clamped to [0..6] on load so a hand-edited session.json
// (or a future-version file with a higher assign-mode value) can't
// crash the encoder routing.
TEST_CASE ("SessionSerializer clamps out-of-range MCU assign mode",
           "[session][serializer][mcu]")
{
    using duskstudio::Session;
    using duskstudio::SessionSerializer;

    const auto dir = makeTempSessionDir();
    const auto target = dir.getChildFile ("session.json");

    // Hand-write a session.json with an out-of-range assign mode.
    const juce::String legacy =
        R"({
            "version": 1,
            "transport": { "mcu_assign_mode": 99 }
        })";
    REQUIRE (target.replaceWithText (legacy));

    Session s;
    REQUIRE (SessionSerializer::load (s, target));
    const int m = s.mcu.assignMode.load (std::memory_order_relaxed);
    REQUIRE (m >= 0);
    REQUIRE (m <= 6);
}
