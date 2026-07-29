#include <catch2/catch_test_macros.hpp>

#include "session/RegionEditActions.h"

using namespace duskstudio;

namespace
{
ClonePluginSnapshot snapshot (std::string name, juce::String legacy,
                              juce::String state)
{
    PluginDescriptor descriptor;
    descriptor.name = std::move (name);
    descriptor.formatName = "Future";
    descriptor.location = "/missing/plugin";
    return { descriptor, std::move (legacy), std::move (state) };
}
} // namespace

TEST_CASE ("clone plugin snapshot replays descriptor legacy fallback and state")
{
    Track destination;
    const auto before = snapshot ("Before", "<BROKEN before", "YmVmb3Jl");
    const auto after = snapshot ("After", "<BROKEN after", "YWZ0ZXI=");
    before.publishTo (destination);

    after.publishTo (destination);   // perform
    CHECK (ClonePluginSnapshot::fromTrack (destination) == after);

    before.publishTo (destination);  // undo
    CHECK (ClonePluginSnapshot::fromTrack (destination) == before);

    after.publishTo (destination);   // redo
    CHECK (ClonePluginSnapshot::fromTrack (destination) == after);
}
