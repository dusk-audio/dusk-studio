#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "engine/multisample/DuskMultisampleProcessor.h"

using Catch::Matchers::WithinAbs;

// DuskMultisampleProcessor persists its file path + override params
// (master volume / tune / polyphony) via getStateInformation +
// setStateInformation. A session save / load that loses any of
// these would silently reset the user's mix on reopen.
TEST_CASE ("DuskMultisampleProcessor round-trips overrides through state", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor a;
    auto& aov = a.getOverrides();
    aov.masterVolDb    .store (-6.0f, std::memory_order_relaxed);
    aov.masterTuneCents.store ( 25.0f, std::memory_order_relaxed);
    aov.polyphony      .store ( 32,    std::memory_order_relaxed);

    juce::MemoryBlock block;
    a.getStateInformation (block);
    REQUIRE (block.getSize() > 0);

    duskstudio::DuskMultisampleProcessor b;
    b.setStateInformation (block.getData(), (int) block.getSize());
    const auto& bov = b.getOverrides();

    REQUIRE_THAT (bov.masterVolDb    .load (std::memory_order_relaxed),
                  WithinAbs (-6.0f, 1e-4f));
    REQUIRE_THAT (bov.masterTuneCents.load (std::memory_order_relaxed),
                  WithinAbs (25.0f, 1e-4f));
    REQUIRE (bov.polyphony.load (std::memory_order_relaxed) == 32);
}

// Out-of-range values in the serialised state (hand-edited
// session.json, future-version sessions) must be clamped on load
// so the audio thread can't trip on NaN / negative polyphony.
TEST_CASE ("DuskMultisampleProcessor clamps out-of-range state", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor a;
    auto& aov = a.getOverrides();
    aov.masterVolDb    .store (1000.0f, std::memory_order_relaxed);   // ridiculous
    aov.masterTuneCents.store (-9999.0f, std::memory_order_relaxed);  // ditto
    aov.polyphony      .store (0,        std::memory_order_relaxed);   // invalid

    juce::MemoryBlock block;
    a.getStateInformation (block);

    duskstudio::DuskMultisampleProcessor b;
    b.setStateInformation (block.getData(), (int) block.getSize());
    const auto& bov = b.getOverrides();

    REQUIRE (bov.masterVolDb    .load() >= -60.0f);
    REQUIRE (bov.masterVolDb    .load() <=  12.0f);
    REQUIRE (bov.masterTuneCents.load() >= -100.0f);
    REQUIRE (bov.masterTuneCents.load() <=  100.0f);
    REQUIRE (bov.polyphony      .load() >=  1);
    REQUIRE (bov.polyphony      .load() <=  256);
}

// Empty state (fresh processor with no save data) must not crash.
TEST_CASE ("DuskMultisampleProcessor accepts empty state", "[multisample]")
{
    duskstudio::DuskMultisampleProcessor p;
    p.setStateInformation (nullptr, 0);
    p.setStateInformation ("", 0);
    REQUIRE_FALSE (p.hasLoadedFile());
}
