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

    // Hide the host container while the unit is still valid. AbandonInstance
    // callers must do this before the slot disposes the Audio Unit, because
    // setHidden: notifies the embedded Cocoa view.
    void quiesce() noexcept;

    // The unit is going away but is STILL ALIVE: drop the plugin's view without
    // releasing it, because its -dealloc runs listener teardown against the
    // unit. close() still releases the container, which only messages the
    // detached subtree while that unit is valid.
    void abandonPlugin() noexcept;

    // Same, for an ALREADY-disposed unit: only clear retained references. The
    // plugin's view is still a subview, so no Cocoa message - including a late
    // setHidden: - is safe here. close() afterwards is a genuine no-op; the
    // container and plugin hierarchy remain leaked for the process lifetime.
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
