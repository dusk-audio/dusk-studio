#include "MultisampleLoader.h"
#include "../sfz/SfzLibraryLocations.h"

#include <algorithm>
#include <chrono>
#include <exception>
#include <utility>

#if defined(_WIN32)
 #include <windows.h>
#elif defined(__APPLE__)
 #include <pthread.h>
#elif defined(__linux__)
 #include <sys/resource.h>
#endif

namespace duskstudio
{
namespace
{
constexpr const char* kCancelled = "Soundfont load cancelled";
constexpr const char* kSuperseded = "Soundfont load superseded by a newer request";
}

MultisampleLoader::MultisampleLoader (PrimingFactory factory)
    : primingFactory (factory ? std::move (factory) : PrimingFactory (primeRequest)),
      worker ([this] { run(); })
{
}

MultisampleLoader::~MultisampleLoader()
{
    stopping.store (true, std::memory_order_release);
    wake.notify_all();
    worker.join();
}

void MultisampleLoader::submit (Request request)
{
    {
        std::lock_guard<std::mutex> lock (mutex);
        if (request.trackIndex < 0 || request.trackIndex >= kTrackCount)
        {
            Result rejected;
            rejected.request = std::move (request);
            rejected.error = "Invalid soundfont track";
            completed.push_back (std::move (rejected));
            return;
        }
        ++trackTickets[(std::size_t) request.trackIndex];
        auto previous = std::find_if (requests.begin(), requests.end(),
            [&request] (const Request& old) { return old.trackIndex == request.trackIndex; });
        if (previous != requests.end())
        {
            Result superseded;
            superseded.request = std::move (*previous);
            superseded.error = kSuperseded;
            requests.erase (previous);
            completed.push_back (std::move (superseded));
        }
        requests.push_back (std::move (request));
    }
    wake.notify_one();
}

void MultisampleLoader::cancelAll()
{
    std::vector<NativeMultisampleSlot::PrimedLoad> retired;
    {
        std::lock_guard<std::mutex> lock (mutex);
        ++cancelEpoch;
        for (auto& result : completed)
        {
            if (result.primed) retired.push_back (std::move (result.primed));
            result.error = kCancelled;
        }
        while (! requests.empty())
        {
            Result cancelled;
            cancelled.request = std::move (requests.front());
            requests.pop_front();
            cancelled.error = kCancelled;
            completed.push_back (std::move (cancelled));
        }
    }
    wake.notify_all();
}

bool MultisampleLoader::hasPendingWork() const
{
    std::lock_guard<std::mutex> lock (mutex);
    return inFlight || ! requests.empty() || ! completed.empty();
}

std::vector<MultisampleLoader::Result> MultisampleLoader::takeCompleted()
{
    std::vector<Result> result;
    {
        std::lock_guard<std::mutex> lock (mutex);
        result.swap (completed);
    }
    return result;
}

MultisampleLoader::Result MultisampleLoader::primeRequest (Request request)
{
    Result result;
    result.request = std::move (request);
    auto& input = result.request;
    const auto locations = sfz::readLibraryLocations();
    if (input.locator)
    {
        const auto resolved = sfz::resolveLibraryInstrument (locations, *input.locator);
        if (resolved) input.path = *resolved;
        else result.error = locations.error.empty()
            ? "This SFZ pack version is unavailable. Reconnect its library drive or open SFZ Manager to install it."
            : locations.error;
    }
    else
    {
        input.locator = sfz::libraryLocatorForFile (locations, input.path);
    }
    if (result.error.empty())
    {
        result.primed = NativeMultisampleSlot::prime (input.path, input.sampleRate,
            input.maxBlock, result.error, input.state.empty() ? nullptr : &input.state,
            ! input.restoring || input.locator.has_value());
        if (result.primed && input.presetIndex >= 0)
        {
            decltype(result.primed.instance->getLastLoadError()) error;
            if (! result.primed.instance->loadSf2Preset (input.presetIndex, error))
            {
                result.error = error.toStdString();
                result.primed = {};
            }
        }
    }
    return result;
}

void MultisampleLoader::run()
{
   #if defined(_WIN32)
    SetThreadPriority (GetCurrentThread(), THREAD_PRIORITY_LOWEST);
   #elif defined(__APPLE__)
    pthread_set_qos_class_self_np (QOS_CLASS_UTILITY, 0);
   #elif defined(__linux__)
    setpriority (PRIO_PROCESS, 0, 10);
   #endif
    while (! stopping.load (std::memory_order_acquire))
    {
        Request request;
        std::uint64_t epoch = 0, ticket = 0;
        {
            std::unique_lock<std::mutex> lock (mutex);
            wake.wait_for (lock, std::chrono::milliseconds (25), [this]
                { return stopping.load (std::memory_order_acquire)
                    || (! paused.load (std::memory_order_acquire) && ! requests.empty()); });
            if (stopping.load (std::memory_order_acquire)) break;
            if (paused.load (std::memory_order_acquire) || requests.empty()) continue;
            request = std::move (requests.front());
            requests.pop_front();
            epoch = cancelEpoch;
            ticket = trackTickets[(std::size_t) request.trackIndex];
            inFlight = true;
        }
        Result result;
        try
        {
            bool cancelled;
            {
                std::unique_lock<std::mutex> lock (mutex);
                while (paused.load (std::memory_order_acquire)
                       && ! stopping.load (std::memory_order_acquire)
                       && cancelEpoch == epoch
                       && trackTickets[(std::size_t) request.trackIndex] == ticket)
                    wake.wait_for (lock, std::chrono::milliseconds (25));
                cancelled = stopping.load (std::memory_order_acquire) || cancelEpoch != epoch
                    || trackTickets[(std::size_t) request.trackIndex] != ticket;
            }
            if (cancelled) result.error = kCancelled;
            else result = primingFactory (request);
        }
        catch (const std::exception& error) { result.error = error.what(); }
        catch (...) { result.error = "Unable to prepare the soundfont"; }
        // Preserve the notification on exceptions, and the default factory's
        // resolved managed identity on success.
        if (! result.request.path.empty()) request.path = std::move (result.request.path);
        if (result.request.locator) request.locator = std::move (result.request.locator);
        result.request = std::move (request);
        std::vector<NativeMultisampleSlot::PrimedLoad> retired;
        {
            std::lock_guard<std::mutex> lock (mutex);
            if (cancelEpoch != epoch || trackTickets[(std::size_t) result.request.trackIndex] != ticket)
            {
                if (result.primed) retired.push_back (std::move (result.primed));
                result.error = cancelEpoch != epoch ? kCancelled : kSuperseded;
            }
            // Every callback gets a result, while heavy prepared occupants are
            // bounded to one per track until the message thread drains them.
            for (auto& previous : completed)
                if (previous.request.trackIndex == result.request.trackIndex && previous.primed)
                {
                    retired.push_back (std::move (previous.primed));
                    previous.error = kSuperseded;
                }
            inFlight = false;
            completed.push_back (std::move (result));
        }
    }
}
} // namespace duskstudio
