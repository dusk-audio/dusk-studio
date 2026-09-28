#pragma once

#include "../../foundation/AutoResetEvent.h"

#include <lv2/core/lv2.h>
#include <lv2/worker/worker.h>

#include <atomic>
#include <cstdint>
#include <deque>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace duskstudio::lv2
{
// Single-producer / single-consumer ring of length-prefixed byte messages. A
// message is a uint32 size followed by its payload, and may wrap the end of the
// buffer, so readers copy it out into their own scratch. Positions only grow;
// the power-of-two capacity keeps the masking correct across uint32 wrap.
class WorkerMessageRing
{
public:
    // Message thread, before either side runs. Capacity must be a power of two.
    void allocate (uint32_t capacityBytes);
    uint32_t capacity() const noexcept { return (uint32_t) bytes.size(); }

    // Producer. False, without blocking, when the message does not fit.
    bool write (const void* data, uint32_t size) noexcept;

    // Consumer. Takes the oldest message and reports its size; false when
    // empty. The payload is copied only when size <= destCapacity, which
    // holds whenever dest is capacity() bytes long.
    bool read (uint8_t* dest, uint32_t destCapacity, uint32_t& size) noexcept;

    // Consumer: hands each message present at the call to fn (data, size),
    // oldest first. Messages the producer adds meanwhile wait for the next call,
    // so one drain is bounded however fast the producer runs.
    template <typename Fn>
    void drain (uint8_t* scratch, uint32_t scratchCapacity, Fn&& fn) noexcept
    {
        const uint32_t end = writePos.load (std::memory_order_acquire);
        uint32_t size = 0;
        while (readPos.load (std::memory_order_relaxed) != end
               && read (scratch, scratchCapacity, size))
            if (size <= scratchCapacity)
                fn (scratch, size);
    }

    // Both sides quiescent: drop everything.
    void reset() noexcept;

private:
    void copyIn (uint32_t pos, const void* src, uint32_t n) noexcept;
    void copyOut (uint32_t pos, void* dst, uint32_t n) const noexcept;

    std::vector<uint8_t>  bytes;
    uint32_t              mask = 0;
    std::atomic<uint32_t> writePos { 0 };
    std::atomic<uint32_t> readPos  { 0 };
};

// Host side of the LV2 Worker extension for one plugin instance.
//
// schedule_work is routed by the calling thread:
//  - the thread inside run() (between beginRun and finishRun) writes to a
//    lock-free request ring, and never blocks, locks or allocates;
//  - the thread inside a HostCallScope (instantiate, activate, deactivate,
//    state restore, all with the audio thread fenced) runs work() and its
//    work_response() before schedule_work returns;
//  - any other thread (a plugin's save() during an autosave with the audio
//    live, or its own thread) queues under a lock the audio thread never takes.
// One non-realtime thread per instance runs work() for both queues and writes
// the replies to a second ring, which it alone produces into; the audio thread
// hands them to work_response() after run(), then calls end_run() once. work()
// runs only on that thread or inside a HostCallScope, always under workMutex,
// so never twice at once.
//
// The audio thread only touches atomics, so it cannot wake the thread; the
// thread polls its event every kFastPollMs after it has had work and every
// kIdlePollMs otherwise. An offline render is not realtime: finishRun wakes the
// thread and waits for every ring request, so the replies land in the same
// block they would in any other render of the same material, unless the render
// is cancelled, when they land in a later block.
class Lv2Worker
{
public:
    static constexpr uint32_t kRingBytes = 1u << 16;

    Lv2Worker();
    ~Lv2Worker();
    Lv2Worker (const Lv2Worker&)            = delete;
    Lv2Worker& operator= (const Lv2Worker&) = delete;

    // Marks the calling thread as making a host call with the audio thread
    // fenced, for the scope's lifetime. Nests.
    class HostCallScope
    {
    public:
        explicit HostCallScope (Lv2Worker& w) noexcept;
        ~HostCallScope();
        HostCallScope (const HostCallScope&)            = delete;
        HostCallScope& operator= (const HostCallScope&) = delete;

    private:
        Lv2Worker&      worker;
        std::thread::id previous;
    };

    // The worker:schedule host feature. Its address is stable for this object's
    // lifetime, which is what a plugin that keeps it relies on.
    const LV2_Feature* scheduleFeature() const noexcept { return &feature; }

    // Message thread, right before lilv instantiates the plugin: requests made
    // from inside instantiate are held until attach, when the handle exists.
    void beginInstantiate();

    // Message thread, after instantiate. A null (or work-less) interface means
    // the plugin does not use the worker: no thread starts and schedule_work
    // fails. Otherwise runs the requests held from instantiate, then starts the
    // thread. False (+ errorOut) when the thread cannot start.
    bool attach (LV2_Handle pluginHandle, const LV2_Worker_Interface* workerInterface,
                 std::string& errorOut);

    // Message thread, audio fenced, before the plugin's state is captured for a
    // restart: join the thread, hand over the replies it produced, then run every
    // request still queued, replies included, on this thread. A change the
    // plugin asked for just before the restart then reaches the carried state.
    void finishQueuedWork();

    // Message thread, audio fenced: join the thread and drop every pending
    // request and reply. HostCallScope schedule_work keeps working, so the
    // plugin's deactivate may still use it.
    void stop() noexcept;

    // Message thread, audio fenced: stop(), then forget the plugin before it is
    // freed.
    void detach() noexcept;

    bool isRunning() const noexcept { return threadRunning.load (std::memory_order_acquire); }

    // Audio thread, around lilv_instance_run. finishRun delivers replies and
    // calls end_run. An offlineRender first waits for the queued work, until
    // renderCancelled (when given) turns true.
    void beginRun() noexcept { runThread.store (std::this_thread::get_id(), std::memory_order_relaxed); }
    void finishRun (bool offlineRender, const std::atomic<bool>* renderCancelled = nullptr) noexcept;

private:
    static constexpr int kFastPollMs    = 1;
    static constexpr int kFastPollCount = 100;
    static constexpr int kIdlePollMs    = 20;
    static constexpr int kOfflineWaitMs = 10;

    static LV2_Worker_Status scheduleWork (LV2_Worker_Schedule_Handle, uint32_t, const void*);
    static LV2_Worker_Status respondFromThread (LV2_Worker_Respond_Handle, uint32_t, const void*);
    static LV2_Worker_Status respondSynchronously (LV2_Worker_Respond_Handle, uint32_t, const void*);

    LV2_Worker_Status queueFromRun (uint32_t size, const void* data) noexcept;
    LV2_Worker_Status queueFromOtherThread (uint32_t size, const void* data);
    LV2_Worker_Status runSynchronously (uint32_t size, const void* data);
    LV2_Worker_Status callWork (LV2_Worker_Respond_Function respond, LV2_Worker_Respond_Handle,
                                uint32_t size, const void* data);
    bool popOtherRequest (std::vector<uint8_t>& out);
    void deliverResponses() noexcept;
    void joinThread() noexcept;
    void waitForQueuedWork (const std::atomic<bool>* renderCancelled) noexcept;
    void threadMain();

    LV2_Worker_Schedule schedule {};
    LV2_Feature         feature {};

    // Written on the message thread before the thread starts or the audio
    // thread can see the instance; read-only everywhere else.
    LV2_Handle                  handle = nullptr;
    const LV2_Worker_Interface* iface  = nullptr;

    WorkerMessageRing    requests, responses;
    std::vector<uint8_t> workScratch;       // whoever consumes `requests`
    std::vector<uint8_t> responseScratch;   // whoever consumes `responses`

    // Thread identities for routing. Each is written only by the thread it
    // names (and cleared by it), so a thread reading its own id back can only
    // have stored it itself; relaxed is enough. Lock-free, so the audio thread
    // can read them without allocating or locking.
    static_assert (std::atomic<std::thread::id>::is_always_lock_free);
    std::atomic<std::thread::id> runThread {};    // inside run()
    std::atomic<std::thread::id> hostThread {};   // inside a HostCallScope
    std::atomic<std::thread::id> workThread {};   // inside work()

    std::uint64_t              requestsQueued = 0;   // ring producer
    std::atomic<std::uint64_t> requestsDone { 0 };   // ring consumer

    std::mutex                        otherLock;      // never taken on the audio thread
    std::deque<std::vector<uint8_t>>  otherRequests;  // otherLock
    std::size_t                       otherBytes = 0; // otherLock

    std::mutex           workMutex;   // never taken on the audio thread
    std::atomic<bool>    stopRequested { false };
    std::atomic<bool>    threadRunning { false };
    dusk::AutoResetEvent wake, workDone;
    std::thread          thread;

    // Message thread only.
    bool                              holdingForInstantiate = false;
    std::vector<std::vector<uint8_t>> heldRequests;
};
} // namespace duskstudio::lv2
