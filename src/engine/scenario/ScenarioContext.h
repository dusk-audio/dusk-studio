#pragma once

#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "Scenario.h"
#include "../../foundation/MidiBuffer.h"

namespace duskstudio
{
class Session;
class AudioEngine;

namespace scenario
{
// Resolution of a logical fixture name against DUSKSTUDIO_FIXTURE_DIR.
// Defined in ScenarioFixtures.cpp.
std::optional<std::filesystem::path> resolveFixture (const std::string& logical);

// Session owns a framework file object, so building one is the one thing a
// scenario cannot express in std::filesystem terms. These two are the only
// place in the suite that names its type, through Session's own getter.
void applySessionDirectory (Session& session, const std::filesystem::path& dir);
std::filesystem::path currentSessionDirectory (const Session& session);

// What a running scenario is handed: the world it acts on, a way to drive audio
// blocks without a device, fixtures, scratch space, and the deferred-completion
// primitives. Message thread only.
class ScenarioContext
{
public:
    static constexpr double kSampleRate = 48000.0;
    static constexpr int    kBlockSize  = 256;

    ScenarioContext (Session& session, AudioEngine& engine,
                     std::function<void (ScenarioResult)> onComplete);
    ~ScenarioContext();

    Session&     session() noexcept { return sessionRef; }
    AudioEngine& engine()  noexcept { return engineRef; }

    // Drives numBlocks callbacks with silent inputs and returns the peak
    // absolute output sample seen across them.
    float pump (int numBlocks);
    // Stages events on an input, then drives exactly one block.
    float pumpWithMidi (int inputIdx, dusk::MidiBuffer events);

    std::optional<std::filesystem::path> fixture (const std::string& logical) const;

    // Per-scenario scratch directory, created on first use and removed when the
    // scenario ends.
    const std::filesystem::path& tempDir();

    // Points the session at dir (creating it) and remembers it. The runner
    // gives every scenario one under tempDir() before run(); a scenario that
    // needs its own - a session it mints for another leg - calls this instead
    // of touching Session directly.
    void setSessionDirectory (const std::filesystem::path& dir);
    const std::filesystem::path& sessionDir() const noexcept { return scenarioSessionDir; }

    // Deferred continuations. Both drop their work once the scenario has
    // completed or the context has gone, and neither ever blocks the loop.
    void later (int ms, std::function<void()> fn);
    void waitUntil (std::function<bool()> pred, int timeoutMs,
                    std::function<void()> onReady, std::string timeoutMessage);

    // First call wins; later ones are ignored, so a watchdog racing a late
    // continuation cannot report twice.
    void complete (ScenarioResult result);
    bool isComplete() const noexcept { return completed; }

    // Runs once when the scenario completes by any path - verdict, skip or the
    // watchdog - before the result is reported, so what a deferred case owns
    // is released even when a wait times out. Latest registered runs first.
    void cleanup (std::function<void()> fn);

    // Records message as a note when condition is false and remembers the
    // first such message, so one broken expectation does not hide the shape of
    // the rest of the run. Returns condition, so a caller can still bail.
    bool expect (bool condition, std::string message);

    // Pass unless expect() saw a failure, in which case the first one is the
    // reason.
    ScenarioResult verdict() const;

    // Diagnostic breadcrumb. Printed only when the scenario fails.
    void note (std::string line);
    const std::vector<std::string>& notes() const noexcept { return log; }

private:
    void ensureBuffers();
    void runCleanups();

    static constexpr int kNumInputs  = 16;
    static constexpr int kNumOutputs = 2;
    static constexpr int kPollMs     = 5;

    Session& sessionRef;
    AudioEngine& engineRef;
    std::function<void (ScenarioResult)> onCompleteFn;

    bool completed = false;
    std::vector<std::string> log;
    std::string firstFailure;
    std::vector<std::function<void()>> cleanups;

    std::vector<std::vector<float>> inputs, outputs;
    std::vector<const float*> inputPtrs;
    std::vector<float*> outputPtrs;

    std::filesystem::path scratchDir;
    std::filesystem::path scenarioSessionDir;

    // Weak-checked by every deferred callback so a timer that outlives the
    // scenario becomes a no-op instead of touching freed state.
    std::shared_ptr<char> aliveToken;

    ScenarioContext (const ScenarioContext&) = delete;
    ScenarioContext& operator= (const ScenarioContext&) = delete;
};
} // namespace scenario
} // namespace duskstudio
