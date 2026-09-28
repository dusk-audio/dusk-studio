#include "Lv2Worker.h"

#include <algorithm>
#include <cstring>
#include <system_error>
#include <utility>

namespace duskstudio::lv2
{
void WorkerMessageRing::allocate (uint32_t capacityBytes)
{
    bytes.assign (capacityBytes, 0);
    mask = capacityBytes - 1;
    reset();
}

void WorkerMessageRing::reset() noexcept
{
    writePos.store (0, std::memory_order_relaxed);
    readPos.store (0, std::memory_order_relaxed);
}

void WorkerMessageRing::copyIn (uint32_t pos, const void* src, uint32_t n) noexcept
{
    if (n == 0) return;
    const uint32_t at = pos & mask;
    const uint32_t first = std::min (n, capacity() - at);
    std::memcpy (bytes.data() + at, src, first);
    if (first < n)
        std::memcpy (bytes.data(), static_cast<const uint8_t*> (src) + first, n - first);
}

void WorkerMessageRing::copyOut (uint32_t pos, void* dst, uint32_t n) const noexcept
{
    if (n == 0) return;
    const uint32_t at = pos & mask;
    const uint32_t first = std::min (n, capacity() - at);
    std::memcpy (dst, bytes.data() + at, first);
    if (first < n)
        std::memcpy (static_cast<uint8_t*> (dst) + first, bytes.data(), n - first);
}

bool WorkerMessageRing::write (const void* data, uint32_t size) noexcept
{
    const uint32_t w = writePos.load (std::memory_order_relaxed);
    const uint32_t r = readPos.load (std::memory_order_acquire);
    const uint64_t needed = (uint64_t) sizeof (uint32_t) + size;
    if (needed > (uint64_t) (capacity() - (w - r)))
        return false;
    copyIn (w, &size, (uint32_t) sizeof (uint32_t));
    copyIn (w + (uint32_t) sizeof (uint32_t), data, size);
    writePos.store (w + (uint32_t) needed, std::memory_order_release);
    return true;
}

bool WorkerMessageRing::read (uint8_t* dest, uint32_t destCapacity, uint32_t& size) noexcept
{
    const uint32_t r = readPos.load (std::memory_order_relaxed);
    const uint32_t w = writePos.load (std::memory_order_acquire);
    if (w - r < sizeof (uint32_t))
        return false;
    uint32_t n = 0;
    copyOut (r, &n, (uint32_t) sizeof (uint32_t));
    if (n <= destCapacity)
        copyOut (r + (uint32_t) sizeof (uint32_t), dest, n);
    readPos.store (r + (uint32_t) sizeof (uint32_t) + n, std::memory_order_release);
    size = n;
    return true;
}

Lv2Worker::HostCallScope::HostCallScope (Lv2Worker& w) noexcept
    : worker (w), previous (w.hostThread.load (std::memory_order_relaxed))
{
    worker.hostThread.store (std::this_thread::get_id(), std::memory_order_relaxed);
}

Lv2Worker::HostCallScope::~HostCallScope()
{
    worker.hostThread.store (previous, std::memory_order_relaxed);
}

Lv2Worker::Lv2Worker()
{
    schedule.handle        = this;
    schedule.schedule_work = &Lv2Worker::scheduleWork;
    feature = { LV2_WORKER__schedule, &schedule };
}

Lv2Worker::~Lv2Worker() { detach(); }

void Lv2Worker::beginInstantiate()
{
    heldRequests.clear();
    holdingForInstantiate = true;
}

bool Lv2Worker::attach (LV2_Handle pluginHandle, const LV2_Worker_Interface* workerInterface,
                        std::string& errorOut)
{
    stop();
    holdingForInstantiate = false;
    handle = pluginHandle;
    iface  = (workerInterface != nullptr && workerInterface->work != nullptr)
               ? workerInterface : nullptr;
    auto held = std::exchange (heldRequests, {});
    if (iface == nullptr)
        return true;

    if (requests.capacity() == 0)
    {
        requests.allocate (kRingBytes);
        responses.allocate (kRingBytes);
        workScratch.assign (kRingBytes, 0);
        responseScratch.assign (kRingBytes, 0);
    }

    for (const auto& request : held)
        runSynchronously ((uint32_t) request.size(), request.data());

    try
    {
        stopRequested.store (false, std::memory_order_relaxed);
        threadRunning.store (true, std::memory_order_release);
        thread = std::thread ([this] { threadMain(); });
    }
    catch (const std::system_error& e)
    {
        threadRunning.store (false, std::memory_order_release);
        errorOut = std::string ("could not start the LV2 worker thread: ") + e.what();
        return false;
    }
    return true;
}

void Lv2Worker::joinThread() noexcept
{
    if (thread.joinable())
    {
        stopRequested.store (true, std::memory_order_release);
        wake.signal();
        thread.join();
    }
    threadRunning.store (false, std::memory_order_release);
    stopRequested.store (false, std::memory_order_relaxed);
}

void Lv2Worker::finishQueuedWork()
{
    joinThread();
    if (iface != nullptr)
    {
        // With the thread joined and the audio fenced this thread is the only
        // consumer of both rings. Replies first: their requests came first.
        const HostCallScope scope (*this);
        deliverResponses();
        uint32_t size = 0;
        while (requests.read (workScratch.data(), (uint32_t) workScratch.size(), size))
            if (size <= workScratch.size())
                runSynchronously (size, workScratch.data());
        std::vector<uint8_t> request;
        while (popOtherRequest (request))
            runSynchronously ((uint32_t) request.size(), request.data());
    }
    stop();
}

void Lv2Worker::stop() noexcept
{
    joinThread();
    requests.reset();
    responses.reset();
    requestsQueued = 0;
    requestsDone.store (0, std::memory_order_relaxed);
    const std::lock_guard<std::mutex> lock (otherLock);
    otherRequests.clear();
    otherBytes = 0;
}

void Lv2Worker::detach() noexcept
{
    stop();
    handle = nullptr;
    iface  = nullptr;
    holdingForInstantiate = false;
    heldRequests.clear();
}

LV2_Worker_Status Lv2Worker::scheduleWork (LV2_Worker_Schedule_Handle h, uint32_t size,
                                           const void* data)
{
    auto* self = static_cast<Lv2Worker*> (h);
    if (self == nullptr || (size > 0 && data == nullptr))
        return LV2_WORKER_ERR_UNKNOWN;

    // Routed by thread identity, not by whether a run is in progress: a thread
    // that is not the one inside run() must never write to the request ring,
    // or the ring gains a second producer.
    const auto me = std::this_thread::get_id();
    // From inside the plugin's own work(): against the spec, and it would
    // deadlock on workMutex or re-enter work().
    if (self->workThread.load (std::memory_order_relaxed) == me)
        return LV2_WORKER_ERR_UNKNOWN;
    if (self->runThread.load (std::memory_order_relaxed) == me)
        return self->queueFromRun (size, data);
    if (self->hostThread.load (std::memory_order_relaxed) == me)
        return self->runSynchronously (size, data);
    return self->queueFromOtherThread (size, data);
}

LV2_Worker_Status Lv2Worker::queueFromRun (uint32_t size, const void* data) noexcept
{
    if (iface == nullptr || ! threadRunning.load (std::memory_order_acquire))
        return LV2_WORKER_ERR_UNKNOWN;
    if (! requests.write (data, size))
        return LV2_WORKER_ERR_NO_SPACE;
    ++requestsQueued;
    return LV2_WORKER_SUCCESS;
}

// Not the audio thread and not a fenced host call, so run() may be executing on
// the audio thread right now: work_response cannot be called from here, and the
// ring has its one producer already. The request goes to the worker thread,
// whose reply reaches work_response through the response ring after a run().
LV2_Worker_Status Lv2Worker::queueFromOtherThread (uint32_t size, const void* data)
{
    if (iface == nullptr || ! threadRunning.load (std::memory_order_acquire))
        return LV2_WORKER_ERR_UNKNOWN;
    {
        const std::lock_guard<std::mutex> lock (otherLock);
        if (otherBytes + size > kRingBytes)
            return LV2_WORKER_ERR_NO_SPACE;
        const auto* bytes = static_cast<const uint8_t*> (data);
        otherRequests.emplace_back (bytes, bytes + size);
        otherBytes += size;
    }
    wake.signal();
    return LV2_WORKER_SUCCESS;
}

bool Lv2Worker::popOtherRequest (std::vector<uint8_t>& out)
{
    const std::lock_guard<std::mutex> lock (otherLock);
    if (otherRequests.empty())
        return false;
    out = std::move (otherRequests.front());
    otherRequests.pop_front();
    otherBytes -= out.size();
    return true;
}

LV2_Worker_Status Lv2Worker::respondFromThread (LV2_Worker_Respond_Handle h, uint32_t size,
                                                const void* data)
{
    auto* self = static_cast<Lv2Worker*> (h);
    if (size > 0 && data == nullptr)
        return LV2_WORKER_ERR_UNKNOWN;
    return self->responses.write (data, size) ? LV2_WORKER_SUCCESS : LV2_WORKER_ERR_NO_SPACE;
}

LV2_Worker_Status Lv2Worker::respondSynchronously (LV2_Worker_Respond_Handle h, uint32_t size,
                                                   const void* data)
{
    auto* replies = static_cast<std::vector<std::vector<uint8_t>>*> (h);
    if (size > 0 && data == nullptr)
        return LV2_WORKER_ERR_UNKNOWN;
    const auto* bytes = static_cast<const uint8_t*> (data);
    replies->emplace_back (bytes, bytes + size);
    return LV2_WORKER_SUCCESS;
}

LV2_Worker_Status Lv2Worker::callWork (LV2_Worker_Respond_Function respond,
                                       LV2_Worker_Respond_Handle respondHandle,
                                       uint32_t size, const void* data)
{
    const std::lock_guard<std::mutex> lock (workMutex);
    workThread.store (std::this_thread::get_id(), std::memory_order_relaxed);
    const auto status = iface->work (handle, respond, respondHandle, size, data);
    workThread.store (std::thread::id(), std::memory_order_relaxed);
    return status;
}

LV2_Worker_Status Lv2Worker::runSynchronously (uint32_t size, const void* data)
{
    if (holdingForInstantiate)
    {
        const auto* bytes = static_cast<const uint8_t*> (data);
        heldRequests.emplace_back (bytes, bytes + size);
        return LV2_WORKER_SUCCESS;
    }
    if (iface == nullptr)
        return LV2_WORKER_ERR_UNKNOWN;

    // Only a fenced host call gets here, so the replies can go straight to
    // work_response instead of waiting for the next block.
    std::vector<std::vector<uint8_t>> replies;
    const auto status = callWork (&Lv2Worker::respondSynchronously, &replies, size, data);
    if (iface->work_response != nullptr)
        for (const auto& reply : replies)
            iface->work_response (handle, (uint32_t) reply.size(), reply.data());
    return status;
}

void Lv2Worker::deliverResponses() noexcept
{
    responses.drain (responseScratch.data(), (uint32_t) responseScratch.size(),
                     [this] (const uint8_t* data, uint32_t size)
                     {
                         if (iface->work_response != nullptr)
                             iface->work_response (handle, size, data);
                     });
}

void Lv2Worker::finishRun (bool offlineRender, const std::atomic<bool>* renderCancelled) noexcept
{
    if (iface != nullptr)
    {
        if (offlineRender)
            waitForQueuedWork (renderCancelled);
        deliverResponses();
        if (iface->end_run != nullptr)
            iface->end_run (handle);
    }
    runThread.store (std::thread::id(), std::memory_order_relaxed);
}

// A cancelled render stops waiting at once: BounceEngine's own loop polls the
// same flag, and ~BounceEngine sets it before stopThread, whose force-kill
// would otherwise land on this noexcept wait.
void Lv2Worker::waitForQueuedWork (const std::atomic<bool>* renderCancelled) noexcept
{
    auto pending = [this]
    {
        return threadRunning.load (std::memory_order_acquire)
            && requestsDone.load (std::memory_order_acquire) != requestsQueued;
    };
    auto cancelled = [renderCancelled]
    {
        return renderCancelled != nullptr && renderCancelled->load (std::memory_order_relaxed);
    };
    if (! pending())
        return;
    wake.signal();
    while (pending() && ! cancelled())
        workDone.wait (kOfflineWaitMs);
}

void Lv2Worker::threadMain()
{
    int fastPolls = 0;
    const auto capacity = (uint32_t) workScratch.size();
    std::vector<uint8_t> other;
    while (! stopRequested.load (std::memory_order_acquire))
    {
        bool worked = false;
        for (bool any = true; any && ! stopRequested.load (std::memory_order_acquire);)
        {
            any = false;
            uint32_t size = 0;
            if (requests.read (workScratch.data(), capacity, size))
            {
                if (size <= capacity)
                    callWork (&Lv2Worker::respondFromThread, this, size, workScratch.data());
                requestsDone.fetch_add (1, std::memory_order_release);
                any = true;
            }
            if (! stopRequested.load (std::memory_order_acquire) && popOtherRequest (other))
            {
                callWork (&Lv2Worker::respondFromThread, this, (uint32_t) other.size(), other.data());
                any = true;
            }
            worked = worked || any;
        }
        if (worked)
        {
            workDone.signal();
            fastPolls = kFastPollCount;
        }
        wake.wait (fastPolls > 0 ? kFastPollMs : kIdlePollMs);
        if (fastPolls > 0) --fastPolls;
    }
}
} // namespace duskstudio::lv2
