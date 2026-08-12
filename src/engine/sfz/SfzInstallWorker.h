#pragma once

#include "SfzInstall.h"

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace duskstudio::sfz
{
// Answers "is the studio busy with something the user would not want competing
// with a download". Polled from the worker thread, so an implementation must
// answer from state the message thread published lock-free, and must never
// reach into the audio thread.
class InstallActivityGate
{
public:
    virtual ~InstallActivityGate() = default;
    virtual bool shouldPauseBackgroundWork() = 0;
};

enum class InstallJobState
{
    queued,
    // Held back because the activity gate is closed. A job that was already
    // transferring keeps its partial download and resumes where it stopped.
    paused,
    running,
    finished
};

struct InstallJobProgress
{
    std::uint64_t id { 0 };
    std::string packId;
    std::string releaseId;
    InstallJobState state { InstallJobState::queued };
    InstallPhase phase { InstallPhase::downloading };
    std::uint64_t completed { 0 };
    std::uint64_t total { 0 };
    // Only meaningful once state is finished.
    InstallStatus status { InstallStatus::cancelled };
    std::string error;
};

// One owned background thread that installs packs one at a time. Public methods
// are message-thread only, except where noted; the worker never touches the
// audio thread and holds no engine state.
class InstallWorker
{
public:
    InstallWorker (Transport& transport, StoreLayout layout, InstallLimits limits = {},
                   InstallActivityGate* gate = nullptr);
    ~InstallWorker();

    InstallWorker (const InstallWorker&) = delete;
    InstallWorker& operator= (const InstallWorker&) = delete;

    // Called on the worker thread once a job reaches a new state, with the
    // final record when it finishes. Marshal onto the message thread before
    // touching anything the UI owns. Set before the first enqueue. The listener
    // must not destroy this worker: the destructor joins the worker thread, so
    // tearing it down from inside a listener self-joins and deadlocks.
    void setListener (std::function<void (const InstallJobProgress&)> listener);

    std::uint64_t enqueue (CatalogPack pack);
    void cancel (std::uint64_t jobId);

    // Wakes the worker after recording, bounce or freeze starts or stops, so a
    // paused job resumes without waiting out the poll interval. Safe from any
    // thread.
    void notifyActivityChanged();

    std::vector<InstallJobProgress> snapshot() const;

private:
    struct Job
    {
        CatalogPack pack;
        std::shared_ptr<std::atomic<bool>> cancelled;
        InstallJobProgress progress;
    };

    void run();
    bool gateIsClosed();

    Transport& transport;
    StoreLayout layout;
    InstallLimits limits;
    InstallActivityGate* gate { nullptr };
    std::function<void (const InstallJobProgress&)> listener;

    mutable std::mutex lock;
    std::condition_variable wake;
    std::deque<std::shared_ptr<Job>> pending;
    std::uint64_t nextJobId { 1 };
    bool activityChanged { false };
    std::atomic<bool> quit { false };
    std::thread worker;
};
} // namespace duskstudio::sfz
