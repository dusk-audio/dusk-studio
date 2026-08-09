#include "Lv2PluginEditorComponent.h"

#include "EmbeddedModal.h"   // kPluginEditorTag
#if ! defined(__APPLE__)
#include "NativeEditorEmbedScale.h"
#endif
#include "../engine/lv2/Lv2Instance.h"
#include <algorithm>

namespace duskstudio
{
Lv2PluginEditorComponent::Lv2PluginEditorComponent()
{
    setOpaque (false);
    // EmbeddedModal hides tagged editors while a modal is up - the native X11
    // window otherwise paints ABOVE the modal regardless of JUCE z-order,
    // burying dialogs under the plugin UI.
    getProperties().set (kPluginEditorTag, true);
}

Lv2PluginEditorComponent::~Lv2PluginEditorComponent()
{
    stopTimer();
    // Every reset path lands here, and both suil teardown and releasing the
    // container its plugin view still sits in can call into an unloaded module.
    if (ownerIsStale()) editor.abandonPluginAndContainer();
    else                editor.quiesce();
    editor.close();
}

bool Lv2PluginEditorComponent::attach (lv2::Lv2Instance& shared, juce::String& errorOut)
{
    std::string err;
    if (! editor.open (shared, err))
    { errorOut = "editor: " + juce::String (err); return false; }
    attachedInstance = &shared;
    embeddedEpoch = shared.instanceEpoch();

    editor.onResize = [this] (int w, int h)
    {
        if (w > 0 && h > 0)
            setSize (componentExtentFromEditor (w),
                     componentExtentFromEditor (h));
    };
    // The UI reported closed (idle() non-zero): stop treating it as live AND tear
    // the native UI down - leaving it open would keep a dead-by-its-own-request
    // window mapped. close() is safe from inside pump()'s onClosed dispatch (pump
    // returns immediately after) and idempotent for the destructor's later call.
    editor.onClosed = [this]
    {
        embedded = false;
        loaded = false;
        stopTimer();
        editor.close();
    };

    // Real size arrives at embed (LV2 UIs size themselves at instantiate); start
    // with a sane placeholder so layout has something to centre.
    setSize (480, 320);

    loaded = true;
    startTimerHz (60);
    return true;
}

std::uintptr_t Lv2PluginEditorComponent::peerNativeHandle() const
{
    if (auto* peer = getPeer())
        return reinterpret_cast<std::uintptr_t> (peer->getNativeHandle());
    return 0;
}

juce::Rectangle<int> Lv2PluginEditorComponent::editorBoundsInPeer() const
{
    const auto logical = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
#if defined(__APPLE__)
    return logical;
#else
    return embedscale::toPhysical (*this, logical);
#endif
}

int Lv2PluginEditorComponent::componentExtentFromEditor (int extent) const
{
#if defined(__APPLE__)
    return std::max (1, extent);
#else
    return embedscale::fromPhysical (*this, extent);
#endif
}

void Lv2PluginEditorComponent::tryEmbed()
{
    // Layout / hierarchy events reach here in the same message-loop turn as an
    // undo-triggered unload, so this is a plugin-code entry point like the pump.
    if (ownerIsStale()) { abandonInstance(); return; }

    // Embed ONLY when actually on-screen: the UI instantiates directly into our
    // host window, and toolkit wrappers can abort realising into a non-viewable
    // parent (same rule as the CLAP editor).
    // `embedding` breaks the re-entry cycle: suil instantiation runs the UI's
    // ui:resize synchronously -> onResize -> setSize -> resized() -> tryEmbed again,
    // which would build a SECOND UI instance and orphan the first (the black-
    // rectangle bug).
    if (! loaded || ! editor.isOpen() || embedded || embedding || ! isShowing()) return;
    const auto parent = peerNativeHandle();
    if (parent == 0) return;

    const auto area = editorBoundsInPeer();
    std::string err;
    embedding = true;
    const bool ok = editor.embed (parent, area.getX(), area.getY(),
                                  std::max (1, area.getWidth()), std::max (1, area.getHeight()), err);
    embedding = false;
    if (ok)
    {
        embedded = true;
        embeddedEpoch = attachedInstance != nullptr ? attachedInstance->instanceEpoch() : 0;
        // Adopt the UI's own size once known so the modal/lane can fit to it.
        if (editor.preferredWidth() > 0 && editor.preferredHeight() > 0)
            setSize (componentExtentFromEditor (editor.preferredWidth()),
                     componentExtentFromEditor (editor.preferredHeight()));
        // Re-sync unconditionally: a synchronous ui:resize during embed can
        // move this component (modal recentre) while `embedded` was still
        // false, so the moved()/resized() pushes were skipped and the native
        // window would keep the pre-move coords passed to embed().
        pushBounds();
        editor.reveal();
    }
    else
    {
        std::fprintf (stderr, "[lv2 editor] embed failed: %s\n", err.c_str());
        // A failed embed tears the Lv2Editor down; stop retrying every frame.
        loaded = false;
        stopTimer();
    }
}

void Lv2PluginEditorComponent::pushBounds()
{
    if (ownerIsStale()) return;   // setBounds resizes the UI's own widget window
    if (! embedded) return;
    // Borrowed bodies get setBounds'd by EmbeddedModal BEFORE being re-added
    // to a parent - getTopLevelComponent() is then `this` and the area
    // degenerates to (0,0), slamming the native window to the origin. Skip
    // while unparented; parentHierarchyChanged re-syncs once re-added.
    if (getParentComponent() == nullptr || getPeer() == nullptr) return;
    const auto area = editorBoundsInPeer();
    editor.setBounds (area.getX(), area.getY(),
                      std::max (1, area.getWidth()), std::max (1, area.getHeight()));
}

void Lv2PluginEditorComponent::resized()                { if (embedded) pushBounds(); else tryEmbed(); }
void Lv2PluginEditorComponent::moved()                  { pushBounds(); }
void Lv2PluginEditorComponent::parentHierarchyChanged() { if (embedded) pushBounds(); else tryEmbed(); }

void Lv2PluginEditorComponent::visibilityChanged()
{
    if (! loaded) return;
    if (isShowing())
    {
        if (! embedded) tryEmbed();
        else            editor.reveal();
    }
    else if (embedded)
    {
        editor.hide();
    }
}

void Lv2PluginEditorComponent::leakForShutdown()
{
    stopTimer();
    // The engine leaks its slots first, which bumps the generation - unbind so
    // that cannot read as stale and divert the destructor into a destroy.
    ownerSlot = nullptr;
    editor.setLeakOnClose (true);
}

void Lv2PluginEditorComponent::abandonInstance()
{
    stopTimer();
    // A stale owner means the instance is ALREADY disposed. On Cocoa the suil
    // callbacks and attached view hierarchy must therefore be leaked untouched;
    // on X11 close() can still release the host window and display.
    abandonNativeEditorInstance (editor, ownerIsStale());
    ownerSlot = nullptr;
    abandoned = true;
    attachedInstance = nullptr;
    loaded = embedded = embedding = false;
}

#if defined(__linux__)
void Lv2PluginEditorComponent::verifyGeometry()
{
    // The message flow can miss a move (compositor interference, an event
    // arriving while unparented) - poll the REAL geometry ~3 Hz, snap back on
    // drift, and log the numbers so field reports say what actually happened.
    if (! isShowing() || getParentComponent() == nullptr || getPeer() == nullptr) return;
    if (++geometryCheckTick < 20) return;
    geometryCheckTick = 0;

    const auto area = editorBoundsInPeer();
    int ax = 0, ay = 0, aw = 0, ah = 0;
    if (! embedCheckLogged)
    {
        embedCheckLogged = true;
        int relX = 0, relY = 0;
        if (editor.getRootRelativePosition (peerNativeHandle(), relX, relY))
            std::fprintf (stderr,
                "[%s editor] embed check: host rel to peer (%d,%d), intended (%d,%d)\n",
                "lv2", relX, relY, area.getX(), area.getY());
    }
    if (! editor.getActualGeometry (ax, ay, aw, ah))
    {
        if (! geometryLostLogged)
        {
            geometryLostLogged = true;
            std::fprintf (stderr, "[lv2 editor] host window lost (XGetGeometry failed)\n");
        }
        return;
    }
    if (ax != area.getX() || ay != area.getY()
        || aw != std::max (1, area.getWidth()) || ah != std::max (1, area.getHeight()))
    {
        if (driftLogsLeft > 0)
        {
            --driftLogsLeft;
            std::fprintf (stderr,
                "[lv2 editor] geometry drift: intended (%d,%d %dx%d) actual (%d,%d %dx%d) - re-syncing\n",
                area.getX(), area.getY(), area.getWidth(), area.getHeight(), ax, ay, aw, ah);
        }
        pushBounds();
    }
}
#endif

void Lv2PluginEditorComponent::timerCallback()
{
    // Ahead of the epoch read below, which dereferences the instance.
    if (ownerIsStale()) { abandonInstance(); return; }

    // reactivate() (device rate/block change) frees and rebuilds the
    // LilvInstance this UI captured via instance-access at embed time. Tear
    // the UI down BEFORE the pump below can drive its idle interface against
    // the freed handle, then re-embed against the fresh instance. Reactivate
    // and this timer both run on the message thread, so the check-then-pump
    // sequence cannot interleave with the swap.
    const auto currentEpoch = attachedInstance != nullptr
                                ? attachedInstance->instanceEpoch() : embeddedEpoch;
    if (currentEpoch != embeddedEpoch)
    {
        editor.close();
        embedded = false;

        // Consume the epoch only once the editor reopens against the new
        // instance. While the instance is inactive (mid-reactivate, or a
        // failed activate awaiting recovery) the stale epoch keeps this
        // branch retrying every tick instead of leaving the editor closed
        // for good; close() above is idempotent, and attach()'s onResize /
        // onClosed callbacks live on the Lv2Editor across close/open.
        if (attachedInstance->isActive())
        {
            std::string err;
            if (! editor.open (*attachedInstance, err))
            {
                std::fprintf (stderr, "[lv2 editor] reopen failed: %s\n", err.c_str());
                loaded = false;
                stopTimer();
                return;
            }
            embeddedEpoch = currentEpoch;
        }
    }

    // Ancestor visibility changes (tab switches) don't fire visibilityChanged -
    // poll like the CLAP editor so the native window can't float over another view.
    // The un-embedded poll covers the mirror case: a component created while its
    // lane was hidden (session restore lands before the AUX tab is first shown)
    // gets no callback when an ANCESTOR becomes visible, so the first embed must
    // also be polled. tryEmbed no-ops until showing.
    if (embedded)
    {
        if (isShowing()) editor.reveal();
        else             editor.hide();
#if defined(__linux__)
        verifyGeometry();
#endif
    }
    else
    {
        tryEmbed();
    }
    editor.pump();
}
} // namespace duskstudio
