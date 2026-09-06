#pragma once

#include <algorithm>
#include <functional>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The scenario suite's vocabulary: one named, self-describing check per
// user-visible situation the release checklist used to test by hand. Cases live
// in cases/*.cpp, register themselves with a file-static ScenarioRegistrar, and
// are driven by SuiteRunner. Nothing here touches the GUI or the audio device.
namespace duskstudio::scenario
{
class ScenarioContext;
// The live window, as a GUI scenario drives it. Declared in src/ui/GuiHost.h;
// opaque here so the scenario vocabulary stays free of the GUI tower.
class GuiHost;

enum class ScenarioStatus { Pass, Fail, Skip };

struct ScenarioResult
{
    ScenarioStatus status = ScenarioStatus::Pass;
    std::string message;

    static ScenarioResult pass()                     { return { ScenarioStatus::Pass, {} }; }
    static ScenarioResult fail (std::string reason)  { return { ScenarioStatus::Fail, std::move (reason) }; }
    static ScenarioResult skip (std::string reason)  { return { ScenarioStatus::Skip, std::move (reason) }; }
};

// What has to be available before a scenario can run. A runner that cannot
// satisfy a bit skips the scenario instead of failing it.
enum Needs : unsigned
{
    Engine = 1,
    Gui    = 2,
    Oop    = 4
};

struct Scenario
{
    std::string name;
    std::vector<std::string> tags;
    unsigned needs = Needs::Engine;
    // Logical fixture names (see ScenarioFixtures.cpp). Any that does not
    // resolve turns the scenario into a SKIP before run is called.
    std::vector<std::string> fixtures;
    // Returning nullopt defers: the scenario has armed its own continuation and
    // finishes later through ScenarioContext::complete.
    std::function<std::optional<ScenarioResult> (ScenarioContext&)> run;
    int timeoutMs = 30000;
    // A Needs::Gui scenario leaves `run` empty and fills this in instead: only
    // the runner built over a live window can supply the host, so the headless
    // runner skips it.
    std::function<std::optional<ScenarioResult> (GuiHost&, ScenarioContext&)> runGui;
};

namespace detail
{
inline std::vector<Scenario>& registry()
{
    static std::vector<Scenario> scenarios;
    return scenarios;
}
} // namespace detail

struct ScenarioRegistrar
{
    ScenarioRegistrar (Scenario s) { detail::registry().push_back (std::move (s)); }
};

// Every registered scenario, ordered by name. Registration happens during
// static init, so the one-time sort here always sees the complete set.
inline const std::vector<Scenario>& allScenarios()
{
    static const std::vector<Scenario>* const sorted = []
    {
        auto& scenarios = detail::registry();
        std::sort (scenarios.begin(), scenarios.end(),
                   [] (const Scenario& a, const Scenario& b) { return a.name < b.name; });
        return &scenarios;
    }();
    return *sorted;
}
} // namespace duskstudio::scenario
