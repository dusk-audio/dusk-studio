#pragma once

#include <cstdint>
#include <functional>
#include <memory>
#include <string>

namespace duskstudio::au
{
class AuInstance;

// AUv2 Cocoa view factory embedded into the same owned NSView-container shape
// used by the H5a CLAP/VST3/LV2 editors. All Cocoa work is message-thread-only.
class AuEditor
{
public:
    AuEditor();
    ~AuEditor();
    AuEditor (const AuEditor&) = delete;
    AuEditor& operator= (const AuEditor&) = delete;

    bool open (AuInstance& instance, std::string& errorOut);
    bool embed (std::uintptr_t parentHandle, int x, int y, int width, int height,
                std::string& errorOut);
    void setBounds (int x, int y, int width, int height);
    void reveal();
    void hide();

    // The unit is going away but is STILL ALIVE: drop the plugin's view without
    // releasing it, because its -dealloc runs listener teardown against the
    // unit. close() still releases the container, which only messages the
    // detached subtree while that unit is valid.
    void abandonPlugin() noexcept;

    // Same, for an ALREADY-disposed unit: nothing may reach plugin code again,
    // which costs the container as well - the plugin's view is still a subview,
    // so detaching or releasing it runs AUListenerDispose /
    // AudioUnitRemovePropertyListener from the window-detach hooks against the
    // disposed unit. It is hidden and then given up unreleased instead: that
    // trades one bounded viewDidHide for the unbounded drawRect/displayLayer a
    // parented dead view takes on every window redraw, and the visible guard
    // means the usual reap (lane already hidden) sends no message at all. What
    // it does not fix is the leaked view's own timers and listeners. close()
    // afterwards is a genuine no-op, and each stale reap strands one container
    // plus the plugin's entire view hierarchy for the life of the process, so
    // repeated undo/redo or session loads with an editor open accumulate them.
    void abandonPluginAndContainer() noexcept;

    void close();
    void pump();

    int preferredWidth() const noexcept;
    int preferredHeight() const noexcept;
    bool isOpen() const noexcept;
    bool isEmbedded() const noexcept;

    std::function<void (int, int)> onResize;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::au
