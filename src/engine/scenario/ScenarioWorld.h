#pragma once

#include <array>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

#include "../../session/SessionLayout.h"

namespace duskstudio
{
class Session;
class AudioEngine;
class ChannelStrip;

namespace scenario
{
// The Session + AudioEngine every scenario runs against, prepared offline: the
// device is detached and closed at construction so a suite run never holds real
// hardware, and blocks are driven by ScenarioContext::pump instead.
//
// reset() returns the world to its as-constructed state between scenarios, so
// one scenario's routing, plugins or transport position cannot reach the next.
// What a scenario should have put back itself and did not, reset() still puts
// back but also names, so the runner can fail the scenario that left it.
class ScenarioWorld
{
public:
    ScenarioWorld();
    ~ScenarioWorld();

    Session&     session() noexcept { return *sessionPtr; }
    AudioEngine& engine()  noexcept { return *enginePtr; }

    std::vector<std::string> reset();

private:
    void prepareOffline();

    // Engine declared second so it is destroyed first, matching the ordering
    // the GUI path tears down in.
    std::unique_ptr<Session> sessionPtr;
    std::unique_ptr<AudioEngine> enginePtr;

    std::filesystem::path bootstrapSessionDir;

    // The strip each track slot ran at construction. A track move hands the
    // strips to other slots, and only another move hands them back.
    std::array<const ChannelStrip*, SessionLayout::kNumTracks> constructedStripOrder {};

    ScenarioWorld (const ScenarioWorld&) = delete;
    ScenarioWorld& operator= (const ScenarioWorld&) = delete;
};
} // namespace scenario
} // namespace duskstudio
