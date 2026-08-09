#pragma once

#include <cstdint>
#include <memory>

namespace duskstudio
{
// Identity of the native plugin instance an editor component was built against.
// Undo replay (RegionEditActions), session restore (AudioEngine) and
// one-host-per-insert eviction (ChannelStrip) all destroy an instance without
// going through the UI, and that teardown unloads the module the editor's
// handles point into. The slot's load generation rides along with the pointer
// because a reload can place the successor at the address the previous instance
// was freed from.
//
// The editor component holds the stamp and re-checks it on every path that
// reaches plugin code: the pump tick, the embed, and the bounds push. Layout
// and hierarchy events fire in the same message-loop turn as the unload that
// retires an instance, so a guard on the pump alone leaves the other two open.
struct NativeEditorOwner
{
    template <typename Slot>
    void stamp (Slot& slot) noexcept
    {
        instance   = static_cast<const void*> (slot.getInstance());
        generation = slot.generation();
    }

    template <typename Slot>
    bool isStale (Slot& slot) const noexcept
    {
        return instance != static_cast<const void*> (slot.getInstance())
            || generation != slot.generation();
    }

    const void*   instance   = nullptr;
    std::uint64_t generation = 0;
};

// How a cached native editor is torn down when its owner drops it.
enum class NativeEditorTeardown
{
    // The app runs on and the instance stays loaded: full teardown including the
    // plugin's own gui->destroy, so a later re-open creates a fresh GUI.
    Destroy,
    // The caller destroys the instance immediately afterwards (session restore).
    // This MUST run while the instance is still live: first quiesce the native
    // container, because hiding a Cocoa container notifies the plugin view, then
    // drop plugin-side handles and close host-owned resources. A stale-owner reap
    // is only a fail-safe; it may abandon retained Cocoa references but must not
    // hide, detach or release them after the plugin/module has gone.
    AbandonInstance,
    // The process is exiting: leak the GUI (it hangs in its own destructor)
    // together with the container + display it is still drawing into.
    LeakForExit,
};

// Enforce the AbandonInstance ordering above. The normal path is called before
// slot disposal and may safely hide the native container; stale cleanup must
// never use that as a late opportunity to touch the embedded Cocoa hierarchy.
template <typename PlatformEditor>
void abandonNativeEditorInstance (PlatformEditor& editor, bool ownerIsStale)
{
    if (ownerIsStale)
    {
        editor.abandonPluginAndContainer();
    }
    else
    {
        editor.quiesce();
        editor.abandonPlugin();
    }
    editor.close();
}

// Drop `editor` once its instance is no longer the slot's live one, or once it
// has already abandoned itself on one of its own guards - abandoning unbinds the
// slot, so staleness alone stops reporting and the component would sit there as
// a dead rectangle. Order is load-bearing: the editor abandons its plugin-side
// handles first - on Cocoa a stale owner gives up the host container without
// messaging it - then `detach` does the owner's own bookkeeping, dismissing a
// modal that borrows the component and removing it as a child while it is alive.
template <typename EditorComponent, typename Detach>
void syncNativeEditorOwner (std::unique_ptr<EditorComponent>& editor, Detach&& detach)
{
    if (editor == nullptr) return;
    if (! editor->ownerIsStale() && ! editor->wasAbandoned()) return;
    editor->abandonInstance();
    detach (*editor);
    editor.reset();
}
} // namespace duskstudio
