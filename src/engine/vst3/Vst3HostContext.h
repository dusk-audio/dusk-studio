#pragma once

#include <cstdint>
#include <memory>

namespace duskstudio::vst3
{
// The host-side COM object a VST3 plugin talks back to: IHostApplication (name +
// IMessage/IAttributeList factory via the SDK's HostApplication), IComponentHandler
// (editor→host parameter edits + restartComponent), IPlugFrame (editor resize
// requests), and the Linux IRunLoop (fd/timer registry plugin UIs — and some DSP —
// require on Linux). VST3's analog of ClapHost.
//
// Threading: everything here is message-thread. VST3 routes controller-side calls
// (component handler, plug frame, run-loop handlers) through the UI thread; pump()
// drives the run loop from the editor timer, mirroring ClapHost::pumpGui.
//
// SDK types stay out of this header; the instance/editor layer casts the opaque
// accessors back to the Steinberg interfaces.
class Vst3HostContext
{
public:
    // Restart/edit notifications, delivered synchronously on the message thread.
    struct Callbacks
    {
        virtual ~Callbacks() = default;
        virtual void onBeginEdit (uint32_t /*paramId*/) {}
        virtual void onPerformEdit (uint32_t /*paramId*/, double /*normalised*/) {}
        virtual void onEndEdit (uint32_t /*paramId*/) {}
        virtual void onRestartComponent (int32_t /*RestartFlags*/) {}
        // The editor asked to resize. Return true when the host honoured it.
        virtual bool onResizeView (int /*w*/, int /*h*/) { return false; }
    };

    Vst3HostContext();
    ~Vst3HostContext();
    Vst3HostContext (const Vst3HostContext&)            = delete;
    Vst3HostContext& operator= (const Vst3HostContext&) = delete;

    void setCallbacks (Callbacks* cb) noexcept;

    // Opaque Steinberg interface pointers (all facets of one COM object whose
    // lifetime is this context — plugins may addRef/release freely):
    void* hostApplication() const noexcept;   // Steinberg::Vst::IHostApplication*
    void* componentHandler() const noexcept;  // Steinberg::Vst::IComponentHandler*
    void* plugFrame() const noexcept;         // Steinberg::IPlugFrame*
    void* runLoop() const noexcept;           // Steinberg::Linux::IRunLoop*

    // Message thread, ~60 Hz: poll the registered fds (dispatching onFDIsSet) and
    // advance the timers by elapsedMs (dispatching onTimerFired on cadence).
    void pump (double elapsedMs);

    int numEventHandlers() const noexcept;
    int numTimerHandlers() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::vst3
