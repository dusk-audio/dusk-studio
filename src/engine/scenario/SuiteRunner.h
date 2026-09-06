#pragma once

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "Scenario.h"

namespace duskstudio::scenario
{
class ScenarioContext;
class ScenarioWorld;

// Drives a selection of scenarios to completion on the message thread, one at a
// time, and reports an exit code through onFinished. Scenarios may finish inside
// their run() or defer, so the chain advances from a callback rather than a loop.
//
// Selector grammar: "all" | "list" | a comma-separated mix of scenario names and
// "tag:<x>" terms. An unrecognised name is fatal: nothing runs and the exit code
// is 2, so a typo in CI is never mistaken for a green suite.
class SuiteRunner
{
public:
    SuiteRunner (std::string selector, std::function<void (int exitCode)> onFinished);
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
} // namespace duskstudio::scenario
