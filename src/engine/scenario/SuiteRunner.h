#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Scenario.h"

namespace duskstudio
{
class Session;
class AudioEngine;

namespace scenario
{
class ScenarioContext;
class ScenarioWorld;

// How a GUI suite ends the process. The app installs it before the window
// exists; the GUI runner, which cannot reach the application object without the
// GUI framework, calls it with the suite's exit code.
std::function<void (int exitCode)>& guiSuiteExit();

// Drives a selection of scenarios to completion on the message thread, one at a
// time, and reports an exit code through onFinished. Scenarios may finish inside
// their run() or defer, so the chain advances from a callback rather than a loop.
//
// Selector grammar: "all" | "list" | a comma-separated mix of scenario names and
// "tag:<x>" terms. An unrecognised name is fatal: nothing runs and the exit code
// is 2, so a typo in CI is never mistaken for a green suite. For the same reason
// a run where nothing passed or failed exits 3, whether every scenario skipped
// or the selection matched none: a missing fixture must not read as a pass.
//
// The GUI form is the same grammar behind a "gui" prefix: "gui" for every GUI
// scenario, "gui:<terms>" for a subset. "all" never picks a GUI scenario up -
// only the window-backed runner can run one.
class SuiteRunner
{
public:
    SuiteRunner (std::string selector, std::function<void (int exitCode)> onFinished);
    // GUI mode. Runs the selected Needs::Gui scenarios against the live
    // window's session and engine - no world of its own, and no reset between
    // scenarios: each GUI case puts back what it changed.
    SuiteRunner (std::string selector, GuiHost& host,
                 Session& session, AudioEngine& engine,
                 std::function<void (int exitCode)> onFinished);
    ~SuiteRunner();

    void start();

private:
    struct Summary
    {
        int pass = 0;
        int fail = 0;
        int skip = 0;
    };

    bool select();
    void runNext();
    void beginCurrent();
    void finishCurrent (ScenarioResult result);
    void skipCurrent (std::string reason);
    void report (const Scenario&, const ScenarioResult&, long long elapsedMs);
    void finishSuite();

    std::string selectorText;
    std::function<void (int)> onFinishedFn;

    // Null in headless mode, which owns a ScenarioWorld instead.
    GuiHost* guiHost = nullptr;
    Session* liveSession = nullptr;
    AudioEngine* liveEngine = nullptr;

    std::vector<const Scenario*> selected;
    std::size_t index = 0;
    unsigned generation = 0;

    Summary summary;

    std::unique_ptr<ScenarioWorld> world;
    std::unique_ptr<ScenarioContext> context;
    long long scenarioStartMs = 0;

    // Weak-checked by the watchdog so a timer outliving the runner is inert.
    std::shared_ptr<char> aliveToken;

    SuiteRunner (const SuiteRunner&) = delete;
    SuiteRunner& operator= (const SuiteRunner&) = delete;
};
} // namespace scenario
} // namespace duskstudio
