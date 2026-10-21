#include <catch2/catch_test_macros.hpp>

#include "engine/multisample/MultisampleLoader.h"
#include "foundation/Fs.h"

#include <sfizz.h>

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <random>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace duskstudio;

TEST_CASE ("SFZ label text remains owned by the loaded instrument", "[multisample][sfz][sfizz-labels]")
{
    std::unique_ptr<sfizz_synth_t, decltype (&sfizz_free)> synth (sfizz_create_synth(), sfizz_free);
    REQUIRE (synth);
    const std::string longLabel (96, 'L');
    const auto source = "<control> label_cc1=Modulation label_cc74=" + longLabel
        + " label_key60=Modulation label_key61=" + longLabel + "\n<region> sample=*sine key=60\n";
    REQUIRE (sfizz_load_string (synth.get(), "label-lifetime.sfz", source.c_str()));
    auto count = sfizz_get_num_cc_labels;
    auto number = sfizz_get_cc_label_number;
    auto text = sfizz_get_cc_label_text;
    int first = 1, second = 74;
    SECTION ("Controller labels") {}
    SECTION ("Key labels")
    {
        count = sfizz_get_num_key_labels;
        number = sfizz_get_key_label_number;
        text = sfizz_get_key_label_text;
        first = 60;
        second = 61;
    }
    const auto numLabels = count (synth.get());
    int firstIndex = -1, secondIndex = -1;
    for (unsigned i = 0; i < numLabels; ++i)
    {
        const auto value = number (synth.get(), static_cast<int> (i));
        if (value == first) firstIndex = static_cast<int> (i);
        if (value == second) secondIndex = static_cast<int> (i);
    }
    REQUIRE (firstIndex >= 0);
    REQUIRE (secondIndex >= 0);
    const auto* shortText = text (synth.get(), firstIndex);
    const auto* longText = text (synth.get(), secondIndex);
    REQUIRE (shortText != nullptr);
    REQUIRE (longText != nullptr);
    REQUIRE (std::string (shortText) == "Modulation");
    REQUIRE (std::string (longText) == longLabel);
    const std::vector<std::string> allocations (64, std::string (96, 'X'));
    REQUIRE (std::string (shortText) == "Modulation");
    REQUIRE (std::string (longText) == longLabel);
    REQUIRE (allocations.front() != longText);
    REQUIRE (text (synth.get(), -1) == nullptr);
    REQUIRE (text (synth.get(), static_cast<int> (numLabels)) == nullptr);
}

namespace
{
using Clock = std::chrono::steady_clock;

struct PrimeGate
{
    std::mutex mutex;
    std::condition_variable condition;
    std::vector<std::uint64_t> calls;
    std::size_t allowed = 0;
    bool allReleased = false;

    void enter (std::uint64_t generation)
    {
        std::unique_lock<std::mutex> lock (mutex);
        calls.push_back (generation);
        const auto ordinal = calls.size();
        condition.notify_all();
        if (! condition.wait_for (lock, std::chrono::seconds (2), [&]
            { return allReleased || allowed >= ordinal; }))
            throw std::runtime_error ("test priming barrier timed out");
    }
    bool waitForCalls (std::size_t count)
    {
        std::unique_lock<std::mutex> lock (mutex);
        return condition.wait_for (lock, std::chrono::seconds (2), [&] { return calls.size() >= count; });
    }
    void release (std::size_t count)
    {
        std::lock_guard<std::mutex> lock (mutex);
        allowed = count;
        condition.notify_all();
    }
    void releaseAll()
    {
        std::lock_guard<std::mutex> lock (mutex);
        allReleased = true;
        condition.notify_all();
    }
};

struct ReleaseGateAtExit
{
    PrimeGate& gate;
    ~ReleaseGateAtExit() { gate.releaseAll(); }
};

MultisampleLoader::Request request (int track, std::uint64_t generation)
{
    MultisampleLoader::Request result;
    result.trackIndex = track;
    result.requestGeneration = generation;
    result.sessionGeneration = 9;
    result.slotGeneration = 13;
    result.sampleRate = 48000.0;
    result.maxBlock = 256;
    result.path = std::filesystem::u8path ("local-instrument.sfz");
    return result;
}

MultisampleLoader::Result outcome (MultisampleLoader::Request input)
{
    MultisampleLoader::Result result;
    result.request = std::move (input);
    result.error = "factory outcome";
    return result;
}

std::vector<MultisampleLoader::Result> waitForResults (MultisampleLoader& loader, std::size_t count)
{
    std::vector<MultisampleLoader::Result> results;
    const auto deadline = Clock::now() + std::chrono::seconds (2);
    while (results.size() < count && Clock::now() < deadline)
    {
        for (auto& result : loader.takeCompleted()) results.push_back (std::move (result));
        if (results.size() < count) std::this_thread::yield();
    }
    REQUIRE (results.size() == count);
    return results;
}
} // namespace

TEST_CASE ("Multisample loader serializes track priming and leaves callbacks to its consumer",
           "[multisample][loader][ipc]")
{
    PrimeGate gate;
    const auto consumerThread = std::this_thread::get_id();
    std::atomic<bool> factoryWasConsumer { false };
    std::atomic<int> callbacks { 0 };
    MultisampleLoader loader ([&] (MultisampleLoader::Request input)
    {
        factoryWasConsumer.store (std::this_thread::get_id() == consumerThread);
        gate.enter (input.requestGeneration);
        return outcome (std::move (input));
    });
    ReleaseGateAtExit release { gate };
    auto first = request (0, 1);
    first.onDone = [&] (bool, std::string) { REQUIRE (std::this_thread::get_id() == consumerThread); ++callbacks; };
    auto second = request (1, 2);
    second.onDone = first.onDone;
    loader.submit (std::move (first));
    REQUIRE (gate.waitForCalls (1));
    loader.submit (std::move (second));
    {
        std::lock_guard<std::mutex> lock (gate.mutex);
        REQUIRE (gate.calls == std::vector<std::uint64_t> { 1 });
    }
    REQUIRE (loader.hasPendingWork());
    REQUIRE (callbacks.load() == 0);
    gate.release (1);
    REQUIRE (gate.waitForCalls (2));
    gate.releaseAll();
    auto results = waitForResults (loader, 2);
    REQUIRE_FALSE (factoryWasConsumer.load());
    REQUIRE (callbacks.load() == 0);
    REQUIRE (results[0].request.requestGeneration == 1);
    REQUIRE (results[1].request.requestGeneration == 2);
    for (auto& result : results) result.request.onDone (false, result.error);
    REQUIRE (callbacks.load() == 2);
    REQUIRE_FALSE (loader.hasPendingWork());
}

TEST_CASE ("Multisample loader pauses before priming and replaces queued tracks with their newest request",
           "[multisample][loader][ipc]")
{
    PrimeGate gate;
    MultisampleLoader loader ([&] (MultisampleLoader::Request input)
    {
        gate.enter (input.requestGeneration);
        return outcome (std::move (input));
    });
    ReleaseGateAtExit release { gate };
    loader.setPaused (true);
    int notifications = 0;
    auto oldest = request (0, 1);
    oldest.onDone = [&] (bool ok, std::string) { REQUIRE_FALSE (ok); ++notifications; };
    loader.submit (std::move (oldest));
    loader.submit (request (0, 2));
    loader.submit (request (1, 3));
    const auto cancelled = loader.takeCompleted();
    REQUIRE (cancelled.size() == 1);
    REQUIRE (cancelled[0].request.requestGeneration == 1);
    REQUIRE_FALSE (cancelled[0].error.empty());
    REQUIRE (cancelled[0].request.onDone);
    cancelled[0].request.onDone (false, cancelled[0].error);
    REQUIRE (notifications == 1);
    {
        std::unique_lock<std::mutex> lock (gate.mutex);
        REQUIRE_FALSE (gate.condition.wait_for (lock, std::chrono::milliseconds (50), [&]
            { return ! gate.calls.empty(); }));
    }
    loader.setPaused (false);
    REQUIRE (gate.waitForCalls (1));
    gate.releaseAll();
    const auto results = waitForResults (loader, 2);
    REQUIRE (results[0].request.requestGeneration == 2);
    REQUIRE (results[1].request.requestGeneration == 3);
}

TEST_CASE ("Multisample loader cancellation preserves queued and in-flight completion notifications",
           "[multisample][loader][ipc]")
{
    PrimeGate gate;
    MultisampleLoader loader ([&] (MultisampleLoader::Request input)
    {
        gate.enter (input.requestGeneration);
        return outcome (std::move (input));
    });
    ReleaseGateAtExit release { gate };
    int callbacks = 0;
    auto first = request (0, 1);
    first.onDone = [&] (bool ok, std::string error)
    { REQUIRE_FALSE (ok); REQUIRE_FALSE (error.empty()); ++callbacks; };
    auto second = request (1, 2);
    second.onDone = first.onDone;
    loader.submit (std::move (first));
    REQUIRE (gate.waitForCalls (1));
    loader.submit (std::move (second));
    loader.cancelAll();
    auto results = loader.takeCompleted();
    REQUIRE (results.size() == 1);
    REQUIRE (results[0].request.requestGeneration == 2);
    REQUIRE (callbacks == 0);
    gate.releaseAll();
    for (auto& result : waitForResults (loader, 1)) results.push_back (std::move (result));
    REQUIRE (results[1].request.requestGeneration == 1);
    REQUIRE (results[1].error == "Soundfont load cancelled");
    for (auto& result : results)
    {
        REQUIRE (result.request.onDone);
        result.request.onDone (false, result.error);
    }
    REQUIRE (callbacks == 2);
    REQUIRE_FALSE (loader.hasPendingWork());
}

TEST_CASE ("Multisample loader catches worker exceptions without losing the request envelope",
           "[multisample][loader][ipc]")
{
    MultisampleLoader loader ([] (MultisampleLoader::Request input) -> MultisampleLoader::Result
    {
        if (input.trackIndex == 0) throw std::runtime_error ("decoder exception");
        throw 42;
    });
    int callbacks = 0;
    for (int track = 0; track < 2; ++track)
    {
        auto input = request (track, (std::uint64_t) track + 1);
        input.state = { 1, 2, 3 };
        input.restoring = true;
        input.onDone = [&] (bool ok, std::string) { REQUIRE_FALSE (ok); ++callbacks; };
        loader.submit (std::move (input));
    }
    auto results = waitForResults (loader, 2);
    REQUIRE (results[0].error == "decoder exception");
    REQUIRE (results[1].error == "Unable to prepare the soundfont");
    for (auto& result : results)
    {
        REQUIRE (result.request.sessionGeneration == 9);
        REQUIRE (result.request.slotGeneration == 13);
        REQUIRE (result.request.state == std::vector<std::uint8_t> { 1, 2, 3 });
        REQUIRE (result.request.restoring);
        REQUIRE (result.request.onDone);
        result.request.onDone (false, result.error);
    }
    REQUIRE (callbacks == 2);
}

TEST_CASE ("Multisample loader default priming reports a missing local file without a download",
           "[multisample][loader][ipc]")
{
    MultisampleLoader loader;
    auto input = request (0, 1);
    input.path = std::filesystem::temp_directory_path()
        / ("dusk-missing-loader-" + std::to_string (std::random_device {}())) / "instrument.sfz";
    loader.submit (std::move (input));
    const auto results = waitForResults (loader, 1);
    REQUIRE_FALSE (results[0].primed);
    REQUIRE_FALSE (results[0].error.empty());
    REQUIRE_FALSE (results[0].request.locator.has_value());
    REQUIRE_FALSE (loader.hasPendingWork());
}

TEST_CASE ("Multisample loader completion retains an identified managed file and its resolved path",
           "[multisample][loader][ipc]")
{
    const sfz::ManagedSfzLocator locator { "sfzinstruments", "piano", "revision",
        std::string (64, 'a'), "bright", "bright.sfz" };
    const auto resolved = std::filesystem::u8path ("library/packs/version/bright.sfz");
    MultisampleLoader loader ([&] (MultisampleLoader::Request input)
    {
        input.path = resolved;
        input.locator = locator;
        return outcome (std::move (input));
    });
    loader.submit (request (0, 1));
    const auto results = waitForResults (loader, 1);
    REQUIRE (results[0].request.path == resolved);
    REQUIRE (results[0].request.locator.has_value());
    REQUIRE (*results[0].request.locator == locator);
    REQUIRE (results[0].request.requestGeneration == 1);
}

TEST_CASE ("Multisample loader replacements override the old state file while manual restores preserve it",
           "[multisample][loader][ipc]")
{
    struct TemporaryDirectory
    {
        std::filesystem::path path = dusk::fs::createUniqueTempDirectory ("dusk-loader-state-");
        ~TemporaryDirectory()
        {
            std::error_code ignored;
            std::filesystem::remove_all (path, ignored);
        }
    } temporary;
    REQUIRE_FALSE (temporary.path.empty());
    const auto oldPath = temporary.path / "old.sfz";
    const auto newPath = temporary.path / "replacement.sfz";
    {
        std::ofstream old (oldPath), replacement (newPath);
        old << "<region> key=60 sample=*sine\n";
        replacement << "<region> key=64 sample=*sine\n<region> key=67 sample=*sine\n";
        REQUIRE (old.good());
        REQUIRE (replacement.good());
    }
    DuskMultisampleProcessor source;
    juce::String error;
    REQUIRE (source.loadSfzFile (juce::File (juce::String::fromUTF8 (oldPath.u8string().c_str())), error));
    std::vector<std::uint8_t> state;
    REQUIRE (source.saveState (state));
    MultisampleLoader loader;
    for (int track = 0; track < 2; ++track)
    {
        auto input = request (track, (std::uint64_t) track + 1);
        input.path = newPath;
        input.state = state;
        input.restoring = track == 1;
        loader.submit (std::move (input));
    }
    const auto results = waitForResults (loader, 2);
    REQUIRE (results[0].primed);
    REQUIRE (results[0].primed.path == newPath.u8string());
    REQUIRE (results[0].primed.instance->getNumRegions() == 2);
    REQUIRE (results[1].primed);
    REQUIRE (results[1].primed.path == oldPath.u8string());
    REQUIRE (results[1].primed.instance->getNumRegions() == 1);
}

TEST_CASE ("Multisample loader destroys superseded prepared occupants after unlocking its queue",
           "[multisample][loader][ipc]")
{
    struct DestructionProbe
    {
        std::mutex mutex;
        std::condition_variable condition;
        std::thread probe;
        bool entered = false, finished = false, blockedByQueue = false;
    };
    struct JoinProbeAtExit
    {
        std::shared_ptr<DestructionProbe> observation;
        ~JoinProbeAtExit() { if (observation->probe.joinable()) observation->probe.join(); }
    };
    struct OwnedObserver
    {
        std::shared_ptr<DestructionProbe> observation;
        MultisampleLoader* loader;
        OwnedObserver (std::shared_ptr<DestructionProbe> state, MultisampleLoader* owner)
            : observation (std::move (state)), loader (owner) {}
        ~OwnedObserver()
        {
            const auto shared = observation;
            shared->probe = std::thread ([shared, owner = loader]
            {
                (void) owner->hasPendingWork();
                std::lock_guard<std::mutex> lock (shared->mutex);
                shared->entered = true;
                shared->condition.notify_all();
            });
            std::unique_lock<std::mutex> lock (shared->mutex);
            shared->blockedByQueue = ! shared->condition.wait_for (lock, std::chrono::milliseconds (500),
                [&] { return shared->entered; });
            shared->finished = true;
            shared->condition.notify_all();
        }
    };
    PrimeGate gate;
    auto observation = std::make_shared<DestructionProbe>();
    JoinProbeAtExit joinProbe { observation };
    MultisampleLoader* owner = nullptr;
    MultisampleLoader loader ([&] (MultisampleLoader::Request input)
    {
        auto result = outcome (input);
        if (input.requestGeneration == 1)
        {
            result.primed.instance = std::make_unique<DuskMultisampleProcessor>();
            auto observer = std::make_shared<OwnedObserver> (observation, owner);
            result.primed.instance->setReplacementLoader (
                [observer] (std::filesystem::path, int, DuskMultisampleProcessor::ReplacementCompletion) {});
            gate.enter (input.requestGeneration);
        }
        return result;
    });
    owner = &loader;
    ReleaseGateAtExit release { gate };
    loader.submit (request (0, 1));
    REQUIRE (gate.waitForCalls (1));
    loader.submit (request (0, 2));
    gate.releaseAll();
    auto results = waitForResults (loader, 2);
    {
        std::unique_lock<std::mutex> lock (observation->mutex);
        REQUIRE (observation->condition.wait_for (lock, std::chrono::seconds (2), [&]
            { return observation->finished && observation->entered; }));
        REQUIRE_FALSE (observation->blockedByQueue);
    }
    observation->probe.join();
    REQUIRE_FALSE (results[0].primed);
    REQUIRE (results[0].error == "Soundfont load superseded by a newer request");
    REQUIRE (results[1].request.requestGeneration == 2);
}
