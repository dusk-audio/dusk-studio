#pragma once

#include "NativeMultisampleSlot.h"
#include "../sfz/ManagedSfzLocator.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <filesystem>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace duskstudio
{
class MultisampleLoader
{
public:
    struct Request
    {
        int trackIndex = -1;
        std::uint64_t requestGeneration = 0, sessionGeneration = 0, slotGeneration = 0;
        double sampleRate = 0.0;
        int maxBlock = 0, presetIndex = -1;
        std::filesystem::path path;
        std::optional<sfz::ManagedSfzLocator> locator;
        std::vector<std::uint8_t> state;
        bool restoring = false;
        std::function<void (bool, std::string)> onDone;
        std::function<void()> beforeCommit;
    };
    struct Result
    {
        Request request;
        NativeMultisampleSlot::PrimedLoad primed;
        std::string error;
    };

    using PrimingFactory = std::function<Result (Request)>;
    explicit MultisampleLoader (PrimingFactory factory = {});
    ~MultisampleLoader();
    void submit (Request);
    void cancelAll();
    void setPaused (bool value) noexcept { paused.store (value, std::memory_order_release); }
    std::vector<Result> takeCompleted();
    bool hasPendingWork() const;

private:
    static Result primeRequest (Request);
    void run();
    static constexpr int kTrackCount = 24;
    mutable std::mutex mutex;
    std::condition_variable wake;
    std::deque<Request> requests;
    std::vector<Result> completed;
    std::array<std::uint64_t, kTrackCount> trackTickets {};
    std::uint64_t cancelEpoch = 0;
    std::atomic<bool> stopping { false }, paused { false };
    bool inFlight = false;
    PrimingFactory primingFactory;
    std::thread worker;
};
} // namespace duskstudio
