#include "SuiteRunner.h"

#include "ScenarioContext.h"
#include "ScenarioWorld.h"
#include "../../foundation/MessageThread.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <utility>

namespace duskstudio::scenario
{
namespace
{
long long nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds> (
               std::chrono::steady_clock::now().time_since_epoch()).count();
}

std::vector<std::string> splitTerms (const std::string& text)
{
    std::vector<std::string> terms;
    std::size_t start = 0;
    while (start <= text.size())
    {
        const auto end = text.find (',', start);
        auto piece = text.substr (start, end == std::string::npos ? std::string::npos
                                                                  : end - start);
        const auto first = piece.find_first_not_of (" \t");
        if (first != std::string::npos)
        {
            const auto last = piece.find_last_not_of (" \t");
            terms.push_back (piece.substr (first, last - first + 1));
        }
        if (end == std::string::npos) break;
        start = end + 1;
    }
    return terms;
}

bool hasTag (const Scenario& scenario, const std::string& tag)
{
    return std::find (scenario.tags.begin(), scenario.tags.end(), tag) != scenario.tags.end();
}

std::string joinTags (const std::vector<std::string>& tags)
{
    std::string joined;
    for (const auto& tag : tags)
    {
        if (! joined.empty()) joined += ",";
        joined += tag;
    }
    return joined;
}
} // namespace

std::function<void (int)>& guiSuiteExit()
{
    static std::function<void (int)> exitFn;
    return exitFn;
}

SuiteRunner::SuiteRunner (std::string selector, std::function<void (int)> onFinished)
    : selectorText (std::move (selector)), onFinishedFn (std::move (onFinished)),
      aliveToken (std::make_shared<char>())
{}

SuiteRunner::SuiteRunner (std::string selector, GuiHost& host,
                          Session& session, AudioEngine& engine,
                          std::function<void (int)> onFinished)
    : selectorText (std::move (selector)), onFinishedFn (std::move (onFinished)),
      guiHost (&host), liveSession (&session), liveEngine (&engine),
      aliveToken (std::make_shared<char>())
{}

SuiteRunner::~SuiteRunner() = default;

void SuiteRunner::start()
{
    if (selectorText == "list")
    {
        for (const auto& scenario : allScenarios())
        {
            const auto tags = joinTags (scenario.tags);
            if (tags.empty())
                std::fprintf (stdout, "%s\n", scenario.name.c_str());
            else
                std::fprintf (stdout, "%s [%s]\n", scenario.name.c_str(), tags.c_str());
        }
        std::fflush (stdout);
        if (onFinishedFn) onFinishedFn (0);
        return;
    }

    if (! select())
    {
        if (onFinishedFn) onFinishedFn (2);
        return;
    }

    if (guiHost == nullptr)
        world = std::make_unique<ScenarioWorld>();
    runNext();
}

bool SuiteRunner::select()
{
    const auto& scenarios = allScenarios();
    const bool guiMode = guiHost != nullptr;

    std::string terms = selectorText;
    if (guiMode)
    {
        if (selectorText == "gui") terms = "all";
        else if (selectorText.rfind ("gui:", 0) == 0) terms = selectorText.substr (4);
        else
        {
            std::fprintf (stderr, "not a gui selector: %s\n", selectorText.c_str());
            std::fflush (stderr);
            return false;
        }
        if (terms.empty()) terms = "all";
    }

    for (const auto& term : splitTerms (terms))
    {
        if (term == "all")
        {
            // Helpers exist to set something up for another leg of the suite, so
            // they are not part of a verdict. Naming one, or its tag, still runs
            // it. A GUI scenario is likewise only ever picked up by the runner
            // that can actually drive it.
            for (const auto& scenario : scenarios)
                if (! hasTag (scenario, "helper")
                    && (((scenario.needs & Needs::Gui) != 0) == guiMode))
                    selected.push_back (&scenario);
            continue;
        }

        if (term.rfind ("tag:", 0) == 0)
        {
            const auto tag = term.substr (4);
            for (const auto& scenario : scenarios)
                if (hasTag (scenario, tag))
                    selected.push_back (&scenario);
            continue;
        }

        const auto match = std::find_if (scenarios.begin(), scenarios.end(),
                                         [&term] (const Scenario& s) { return s.name == term; });
        if (match == scenarios.end())
        {
            std::fprintf (stderr, "unknown scenario: %s\n", term.c_str());
            std::fflush (stderr);
            selected.clear();
            return false;
        }
        selected.push_back (&*match);
    }

    // A term list may name the same scenario twice (directly and through a tag).
    std::vector<const Scenario*> unique;
    for (const auto* scenario : selected)
        if (std::find (unique.begin(), unique.end(), scenario) == unique.end())
            unique.push_back (scenario);
    selected.swap (unique);

    return true;
}

void SuiteRunner::runNext()
{
    if (index >= selected.size())
    {
        finishSuite();
        return;
    }

    const auto& scenario = *selected[index];

    // Named before anything runs and flushed, so a crash inside the scenario
    // still says which one it was.
    std::fprintf (stderr, "[RUN] %s\n", scenario.name.c_str());
    std::fflush (stderr);

    const bool guiMode = guiHost != nullptr;
    const bool needsGui = (scenario.needs & Needs::Gui) != 0;

    if (needsGui != guiMode)
    {
        skipCurrent (needsGui ? "needs gui" : "needs the headless runner");
        return;
    }

    if (guiMode ? ! scenario.runGui : ! scenario.run)
    {
        skipCurrent (guiMode ? "no gui entry point" : "no headless entry point");
        return;
    }

    for (const auto& logical : scenario.fixtures)
    {
        if (! resolveFixture (logical).has_value())
        {
            skipCurrent ("missing fixture: " + logical);
            return;
        }
    }

    beginCurrent();
}

void SuiteRunner::beginCurrent()
{
    const auto& scenario = *selected[index];

    scenarioStartMs = nowMs();
    ++generation;

    if (guiHost != nullptr)
    {
        // The window owns the session directory a GUI run acts on, so unlike the
        // headless path this does not repoint it at scratch space.
        context = std::make_unique<ScenarioContext> (
            *liveSession, *liveEngine,
            [this] (ScenarioResult result) { finishCurrent (std::move (result)); });
    }
    else
    {
        context = std::make_unique<ScenarioContext> (
            world->session(), world->engine(),
            [this] (ScenarioResult result) { finishCurrent (std::move (result)); });

        // A scratch session of its own for every scenario: plugin file state,
        // saved sessions and recorded audio all key off this directory, and it
        // goes away with the context.
        context->setSessionDirectory (context->tempDir() / "session");
    }

    const unsigned armed = generation;
    const int timeoutMs = scenario.timeoutMs > 0 ? scenario.timeoutMs : 30000;
    std::weak_ptr<char> guard = aliveToken;
    dusk::Timer::callAfterDelay (timeoutMs, [this, guard, armed, timeoutMs]
    {
        if (guard.expired() || armed != generation || context == nullptr) return;
        context->complete (ScenarioResult::fail ("timed out after " + std::to_string (timeoutMs) + " ms"));
    });

    auto immediate = guiHost != nullptr ? scenario.runGui (*guiHost, *context)
                                        : scenario.run (*context);
    if (immediate)
        context->complete (std::move (*immediate));
}

void SuiteRunner::finishCurrent (ScenarioResult result)
{
    const auto& scenario = *selected[index];
    const auto elapsed = nowMs() - scenarioStartMs;

    report (scenario, result, elapsed);

    // Off the completing scenario's stack: run() may have completed inline, and
    // recursing into the next scenario from inside it would grow the stack by
    // the length of the suite.
    ++generation;
    ++index;
    std::weak_ptr<char> guard = aliveToken;
    dusk::callAsync ([this, guard]
    {
        if (guard.expired()) return;
        context.reset();
        if (world != nullptr) world->reset();
        runNext();
    });
}

void SuiteRunner::skipCurrent (std::string reason)
{
    const auto& scenario = *selected[index];
    std::fprintf (stdout, "[SKIP] %s: %s\n", scenario.name.c_str(), reason.c_str());
    std::fflush (stdout);
    ++summary.skip;

    ++index;
    std::weak_ptr<char> guard = aliveToken;
    dusk::callAsync ([this, guard]
    {
        if (guard.expired()) return;
        runNext();
    });
}

void SuiteRunner::report (const Scenario& scenario, const ScenarioResult& result,
                          long long elapsedMs)
{
    switch (result.status)
    {
        case ScenarioStatus::Pass:
            std::fprintf (stdout, "[PASS] %s (%lld ms)\n", scenario.name.c_str(), elapsedMs);
            ++summary.pass;
            break;

        case ScenarioStatus::Fail:
            std::fprintf (stdout, "[FAIL] %s: %s (%lld ms)\n", scenario.name.c_str(),
                          result.message.c_str(), elapsedMs);
            if (context != nullptr)
                for (const auto& line : context->notes())
                    std::fprintf (stdout, "       %s\n", line.c_str());
            ++summary.fail;
            break;

        case ScenarioStatus::Skip:
            std::fprintf (stdout, "[SKIP] %s: %s\n", scenario.name.c_str(),
                          result.message.c_str());
            ++summary.skip;
            break;
    }
    std::fflush (stdout);
}

void SuiteRunner::finishSuite()
{
    std::fprintf (stdout, "=== scenarios: %d pass, %d fail, %d skip ===\n",
                  summary.pass, summary.fail, summary.skip);
    std::fflush (stdout);

    int exitCode = summary.fail == 0 ? 0 : 1;
    if (summary.pass == 0 && summary.fail == 0 && summary.skip > 0)
    {
        std::fprintf (stderr, "every selected scenario skipped, so nothing was verified\n");
        std::fflush (stderr);
        exitCode = 3;
    }
    if (onFinishedFn) onFinishedFn (exitCode);
}
} // namespace duskstudio::scenario
