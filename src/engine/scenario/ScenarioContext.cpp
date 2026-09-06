#include "ScenarioContext.h"

#include "../AudioEngine.h"
#include "../device/IODeviceCallback.h"
#include "../../foundation/Fs.h"
#include "../../foundation/MessageThread.h"
#include "../../session/Session.h"

#include <algorithm>
#include <cmath>
#include <system_error>
#include <utility>

namespace duskstudio::scenario
{
namespace
{
using SessionFile = decltype (std::declval<const Session&>().getSessionDirectory());
} // namespace

void applySessionDirectory (Session& session, const std::filesystem::path& dir)
{
    session.setSessionDirectory (SessionFile (dir.u8string().c_str()));
}

std::filesystem::path currentSessionDirectory (const Session& session)
{
    return std::filesystem::u8path (
        session.getSessionDirectory().getFullPathName().toStdString());
}

ScenarioContext::ScenarioContext (Session& s, AudioEngine& e,
                                  std::function<void (ScenarioResult)> onComplete)
    : sessionRef (s), engineRef (e), onCompleteFn (std::move (onComplete)),
      aliveToken (std::make_shared<char>())
{}

ScenarioContext::~ScenarioContext()
{
    if (! scratchDir.empty())
    {
        std::error_code error;
        std::filesystem::remove_all (scratchDir, error);
    }
}

void ScenarioContext::ensureBuffers()
{
    if (! inputs.empty()) return;

    inputs.assign ((std::size_t) kNumInputs, std::vector<float> ((std::size_t) kBlockSize, 0.0f));
    outputs.assign ((std::size_t) kNumOutputs, std::vector<float> ((std::size_t) kBlockSize, 0.0f));

    inputPtrs.resize ((std::size_t) kNumInputs);
    for (int c = 0; c < kNumInputs; ++c)
        inputPtrs[(std::size_t) c] = inputs[(std::size_t) c].data();

    outputPtrs.resize ((std::size_t) kNumOutputs);
    for (int c = 0; c < kNumOutputs; ++c)
        outputPtrs[(std::size_t) c] = outputs[(std::size_t) c].data();
}

float ScenarioContext::pump (int numBlocks)
{
    ensureBuffers();

    device::CallbackContext callbackContext {};
    float peak = 0.0f;

    for (int b = 0; b < numBlocks; ++b)
    {
        for (auto& channel : outputs)
            std::fill (channel.begin(), channel.end(), 0.0f);

        engineRef.audioDeviceIOCallback (inputPtrs.data(), kNumInputs,
                                         outputPtrs.data(), kNumOutputs,
                                         kBlockSize, callbackContext);

        for (const auto& channel : outputs)
            for (const float sample : channel)
                peak = std::max (peak, std::abs (sample));
    }

    return peak;
}

float ScenarioContext::pumpWithMidi (int inputIdx, dusk::MidiBuffer events)
{
    engineRef.stageTestMidiInjection (inputIdx, std::move (events));
    return pump (1);
}

std::optional<std::filesystem::path> ScenarioContext::fixture (const std::string& logical) const
{
    return resolveFixture (logical);
}

const std::filesystem::path& ScenarioContext::tempDir()
{
    if (scratchDir.empty())
        scratchDir = dusk::fs::createUniqueTempDirectory ("duskstudio-scenario-");
    return scratchDir;
}

void ScenarioContext::setSessionDirectory (const std::filesystem::path& dir)
{
    scenarioSessionDir = dir;
    applySessionDirectory (sessionRef, dir);
}

void ScenarioContext::later (int ms, std::function<void()> fn)
{
    std::weak_ptr<char> guard = aliveToken;
    dusk::Timer::callAfterDelay (ms, [this, guard, fn = std::move (fn)]
    {
        if (guard.expired()) return;
        fn();
    });
}

void ScenarioContext::waitUntil (std::function<bool()> pred, int timeoutMs,
                                 std::function<void()> onReady, std::string timeoutMessage)
{
    // The pending timer holds the poller; the poller holds itself weakly, so it
    // lives exactly as long as it has another poll to schedule.
    auto poller = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakPoller = poller;

    *poller = [this, pred = std::move (pred), timeoutMs, onReady = std::move (onReady),
               timeoutMessage = std::move (timeoutMessage), weakPoller] (int elapsedMs)
    {
        if (completed) return;

        if (pred())
        {
            onReady();
            return;
        }

        if (elapsedMs >= timeoutMs)
        {
            complete (ScenarioResult::fail (timeoutMessage));
            return;
        }

        if (auto next = weakPoller.lock())
            later (kPollMs, [next, elapsedMs] { (*next) (elapsedMs + kPollMs); });
    };

    (*poller) (0);
}

void ScenarioContext::complete (ScenarioResult result)
{
    if (completed) return;
    completed = true;
    if (onCompleteFn) onCompleteFn (std::move (result));
}

bool ScenarioContext::expect (bool condition, std::string message)
{
    if (! condition)
    {
        if (firstFailure.empty()) firstFailure = message;
        note (std::move (message));
    }
    return condition;
}

ScenarioResult ScenarioContext::verdict() const
{
    return firstFailure.empty() ? ScenarioResult::pass()
                                : ScenarioResult::fail (firstFailure);
}

void ScenarioContext::note (std::string line)
{
    log.push_back (std::move (line));
}
} // namespace duskstudio::scenario
