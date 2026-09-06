#pragma once

#include <filesystem>
#include <memory>

namespace duskstudio
{
class Session;
class AudioEngine;

namespace scenario
{
// The Session + AudioEngine every scenario runs against, prepared offline: the
// device is detached and closed at construction so a suite run never holds real
// hardware, and blocks are driven by ScenarioContext::pump instead.
//
// reset() returns the world to its as-constructed state between scenarios, so
// one scenario's routing, plugins or transport position cannot reach the next.
class ScenarioWorld
{
public:
    ScenarioWorld();
    ~ScenarioWorld();

    Session&     session() noexcept { return *sessionPtr; }
    AudioEngine& engine()  noexcept { return *enginePtr; }

    void reset();

private:
    void prepareOffline();

    // Engine declared second so it is destroyed first, matching the ordering
    // the GUI path tears down in.
    std::unique_ptr<Session> sessionPtr;
    std::unique_ptr<AudioEngine> enginePtr;

    std::filesystem::path bootstrapSessionDir;

    ScenarioWorld (const ScenarioWorld&) = delete;
    ScenarioWorld& operator= (const ScenarioWorld&) = delete;
};
} // namespace scenario
} // namespace duskstudio
