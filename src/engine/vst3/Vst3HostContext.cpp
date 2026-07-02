#include "Vst3HostContext.h"

#include <public.sdk/source/vst/hosting/hostclasses.h>
#include <pluginterfaces/gui/iplugview.h>
#include <pluginterfaces/vst/ivsteditcontroller.h>

#include <poll.h>

#include <algorithm>
#include <vector>

namespace duskstudio::vst3
{
namespace
{
using namespace Steinberg;

// One COM object exposing every host facet. Lifetime is the owning
// Vst3HostContext (plugins addRef/release freely against the SDK's FUnknown
// refcount from HostApplication, which never reaches zero while we hold ours).
class HostObject final : public Vst::HostApplication,
                         public Vst::IComponentHandler,
                         public IPlugFrame,
                         public Linux::IRunLoop
{
public:
    explicit HostObject (Vst3HostContext::Callbacks*& cb) : callbacks (cb) {}

    // ── FUnknown: extend HostApplication's QI with the extra facets ──
    tresult PLUGIN_API queryInterface (const TUID iid, void** obj) override
    {
        QUERY_INTERFACE (iid, obj, Vst::IComponentHandler::iid, Vst::IComponentHandler)
        QUERY_INTERFACE (iid, obj, IPlugFrame::iid, IPlugFrame)
        QUERY_INTERFACE (iid, obj, Linux::IRunLoop::iid, Linux::IRunLoop)
        return Vst::HostApplication::queryInterface (iid, obj);
    }
    uint32 PLUGIN_API addRef() override  { return Vst::HostApplication::addRef(); }
    uint32 PLUGIN_API release() override { return Vst::HostApplication::release(); }

    // ── IHostApplication ──
    tresult PLUGIN_API getName (Vst::String128 name) override
    {
        static const char16_t kName[] = u"Dusk Studio";
        std::copy (std::begin (kName), std::end (kName), name);
        return kResultTrue;
    }

    // ── IComponentHandler (controller → host parameter edits) ──
    tresult PLUGIN_API beginEdit (Vst::ParamID id) override
    {
        if (callbacks != nullptr) callbacks->onBeginEdit ((uint32_t) id);
        return kResultOk;
    }
    tresult PLUGIN_API performEdit (Vst::ParamID id, Vst::ParamValue v) override
    {
        if (callbacks != nullptr) callbacks->onPerformEdit ((uint32_t) id, (double) v);
        return kResultOk;
    }
    tresult PLUGIN_API endEdit (Vst::ParamID id) override
    {
        if (callbacks != nullptr) callbacks->onEndEdit ((uint32_t) id);
        return kResultOk;
    }
    tresult PLUGIN_API restartComponent (int32 flags) override
    {
        if (callbacks != nullptr) callbacks->onRestartComponent ((int32_t) flags);
        return kResultOk;
    }

    // ── IPlugFrame (editor asks the host to resize its window) ──
    tresult PLUGIN_API resizeView (IPlugView* /*view*/, ViewRect* newSize) override
    {
        if (newSize == nullptr) return kInvalidArgument;
        const bool ok = callbacks != nullptr
                     && callbacks->onResizeView (newSize->getWidth(), newSize->getHeight());
        return ok ? kResultOk : kResultFalse;
    }

    // ── Linux::IRunLoop (fd + timer registry, pumped from the message thread) ──
    tresult PLUGIN_API registerEventHandler (Linux::IEventHandler* handler,
                                             Linux::FileDescriptor fd) override
    {
        if (handler == nullptr || fd < 0) return kInvalidArgument;
        handler->addRef();
        fdHandlers.push_back ({ handler, fd });
        return kResultOk;
    }
    tresult PLUGIN_API unregisterEventHandler (Linux::IEventHandler* handler) override
    {
        bool found = false;
        for (auto it = fdHandlers.begin(); it != fdHandlers.end();)
        {
            if (it->handler == handler)
            {
                it->handler->release();
                it = fdHandlers.erase (it);
                found = true;
            }
            else
                ++it;
        }
        return found ? kResultOk : kResultFalse;
    }
    tresult PLUGIN_API registerTimer (Linux::ITimerHandler* handler,
                                      Linux::TimerInterval milliseconds) override
    {
        if (handler == nullptr || milliseconds == 0) return kInvalidArgument;
        handler->addRef();
        timers.push_back ({ handler, (double) milliseconds, 0.0 });
        return kResultOk;
    }
    tresult PLUGIN_API unregisterTimer (Linux::ITimerHandler* handler) override
    {
        bool found = false;
        for (auto it = timers.begin(); it != timers.end();)
        {
            if (it->handler == handler)
            {
                it->handler->release();
                it = timers.erase (it);
                found = true;
            }
            else
                ++it;
        }
        return found ? kResultOk : kResultFalse;
    }

    void pump (double elapsedMs)
    {
        // Snapshot both registries before dispatching: a handler may
        // register/unregister from inside its own callback.
        if (! fdHandlers.empty())
        {
            std::vector<pollfd> pfds;
            std::vector<Linux::IEventHandler*> handlers;
            pfds.reserve (fdHandlers.size());
            for (const auto& e : fdHandlers)
            {
                pfds.push_back ({ e.fd, POLLIN | POLLOUT | POLLERR, 0 });
                handlers.push_back (e.handler);
            }
            if (::poll (pfds.data(), (nfds_t) pfds.size(), 0) > 0)
                for (size_t i = 0; i < pfds.size(); ++i)
                    if (pfds[i].revents != 0 && stillRegistered (handlers[i]))
                        handlers[i]->onFDIsSet (pfds[i].fd);
        }

        auto snapshot = timers;
        for (auto& t : snapshot)
        {
            auto* live = findTimer (t.handler);
            if (live == nullptr) continue;   // unregistered by an earlier callback
            live->accumMs += elapsedMs;
            if (live->accumMs >= live->intervalMs)
            {
                live->accumMs = 0.0;   // cadence, not catch-up bursts
                live->handler->onTimer();
            }
        }
    }

    void teardown()
    {
        for (auto& e : fdHandlers) e.handler->release();
        for (auto& t : timers)     t.handler->release();
        fdHandlers.clear();
        timers.clear();
    }

    int numEventHandlers() const { return (int) fdHandlers.size(); }
    int numTimerHandlers() const { return (int) timers.size(); }

private:
    struct FdEntry    { Linux::IEventHandler* handler; Linux::FileDescriptor fd; };
    struct TimerEntry { Linux::ITimerHandler* handler; double intervalMs; double accumMs; };

    bool stillRegistered (Linux::IEventHandler* h) const
    {
        return std::any_of (fdHandlers.begin(), fdHandlers.end(),
                            [h] (const FdEntry& e) { return e.handler == h; });
    }
    TimerEntry* findTimer (Linux::ITimerHandler* h)
    {
        for (auto& t : timers) if (t.handler == h) return &t;
        return nullptr;
    }

    Vst3HostContext::Callbacks*& callbacks;
    std::vector<FdEntry>    fdHandlers;
    std::vector<TimerEntry> timers;
};
} // namespace

struct Vst3HostContext::Impl
{
    Callbacks* callbacks = nullptr;
    Steinberg::IPtr<HostObject> host;
};

Vst3HostContext::Vst3HostContext() : impl (std::make_unique<Impl>())
{
    impl->host = Steinberg::owned (new HostObject (impl->callbacks));
}

Vst3HostContext::~Vst3HostContext()
{
    impl->host->teardown();
}

void Vst3HostContext::setCallbacks (Callbacks* cb) noexcept { impl->callbacks = cb; }

void* Vst3HostContext::hostApplication() const noexcept
{
    return static_cast<Steinberg::Vst::IHostApplication*> (impl->host.get());
}
void* Vst3HostContext::componentHandler() const noexcept
{
    return static_cast<Steinberg::Vst::IComponentHandler*> (impl->host.get());
}
void* Vst3HostContext::plugFrame() const noexcept
{
    return static_cast<Steinberg::IPlugFrame*> (impl->host.get());
}
void* Vst3HostContext::runLoop() const noexcept
{
    return static_cast<Steinberg::Linux::IRunLoop*> (impl->host.get());
}

void Vst3HostContext::pump (double elapsedMs)     { impl->host->pump (elapsedMs); }
int  Vst3HostContext::numEventHandlers() const noexcept { return impl->host->numEventHandlers(); }
int  Vst3HostContext::numTimerHandlers() const noexcept { return impl->host->numTimerHandlers(); }
} // namespace duskstudio::vst3
