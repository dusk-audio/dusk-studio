#pragma once

#include <juce_core/juce_core.h>

#include <atomic>
#include <functional>
#include <memory>
#include <vector>

namespace duskstudio
{
// Fixed pool of real-time worker threads for the per-block strip-DSP fan-out
// (opt-in; see AudioEngine). The audio thread calls runBlock(); job(lane) then
// runs on each of the `workers` worker threads for lane in [0, workers), and on
// the audio thread itself for lane == workers. So laneCount() == workers + 1.
//
// Workers park on an auto-reset juce::WaitableEvent between blocks and are woken
// by the audio thread's signal() — a bounded, uncontended, ns-scale event that
// is the one deliberate exception to the no-lock-on-the-audio-thread rule, used
// ONLY in this opt-in parallel mode. The join is lock-free: the audio thread
// spin-waits on an atomic completion counter (it has to wait for the workers
// regardless, and the wait is bounded by the slowest lane's DSP time).
class AudioWorkerPool
{
public:
    AudioWorkerPool();
    ~AudioWorkerPool();

    // Message thread only. Spawns `workers` real-time threads; `job` is stored
    // once (never reallocated per block) and invoked as job(lane). A count <= 0
    // leaves the pool inactive (runBlock then runs job(0) inline on the caller).
    void start (int workers, std::function<void (int lane)> job);
    void stop();   // message thread only; joins every worker.

    bool isActive()  const noexcept { return numWorkers > 0; }
    int  laneCount() const noexcept { return numWorkers + 1; }

    // Audio thread. Dispatches lanes [0, workers) to the worker threads, runs
    // lane `workers` on the caller, and returns once every lane has finished.
    void runBlock() noexcept;

private:
    struct Worker;
    std::vector<std::unique_ptr<Worker>> workers_;
    std::function<void (int)> job_;
    int numWorkers = 0;
    std::atomic<int>  done { 0 };
    std::atomic<bool> quit { false };

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AudioWorkerPool)
};
} // namespace duskstudio
