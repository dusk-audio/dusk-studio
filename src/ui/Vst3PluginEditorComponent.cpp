#include "Vst3PluginEditorComponent.h"

#include "EmbeddedModal.h"   // kPluginEditorTag
#if ! defined(__APPLE__)
#include "NativeEditorEmbedScale.h"
#endif
#if DUSKSTUDIO_HAS_NATIVE_AU
#include "../engine/au/AuInstance.h"
#endif
#if DUSKSTUDIO_HAS_NATIVE_VST3
#include "../engine/vst3/Vst3Instance.h"
#endif

#include <algorithm>

namespace duskstudio
{
#if DUSKSTUDIO_HAS_NATIVE_VST3
Vst3PluginEditorComponent::Vst3PluginEditorComponent()
{
    setOpaque (false);
    // EmbeddedModal hides tagged editors while a modal is up - the native X11
    // window otherwise paints ABOVE the modal regardless of JUCE z-order,
    // burying dialogs under the plugin UI.
    getProperties().set (kPluginEditorTag, true);
}

Vst3PluginEditorComponent::~Vst3PluginEditorComponent()
{
    stopTimer();
    // Every reset path lands here, and both releasing the view and detaching the
    // container it is attached to call into the module that was unloaded with
    // the instance.
    if (ownerIsStale()) editor.abandonPluginAndContainer();
    else                editor.quiesce();
    editor.close();
}

bool Vst3PluginEditorComponent::attach (vst3::Vst3Instance& shared, juce::String& errorOut)
{
    std::string err;
    if (! editor.open (shared, err))
    { errorOut = "editor: " + juce::String (err); return false; }

    editor.onResize = [this] (int w, int h)
    {
        if (w > 0 && h > 0)
            setSize (componentExtentFromEditor (w),
                     componentExtentFromEditor (h));
    };

    if (editor.preferredWidth() > 0 && editor.preferredHeight() > 0)
        setSize (componentExtentFromEditor (editor.preferredWidth()),
                 componentExtentFromEditor (editor.preferredHeight()));
    else
        setSize (480, 320);

    loaded = true;
    lastPumpMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);
    return true;
}

std::uintptr_t Vst3PluginEditorComponent::peerNativeHandle() const
{
    if (auto* peer = getPeer())
        return reinterpret_cast<std::uintptr_t> (peer->getNativeHandle());
    return 0;
}

juce::Rectangle<int> Vst3PluginEditorComponent::editorBoundsInPeer() const
{
    const auto logical = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
#if defined(__APPLE__)
    return logical;
#else
    return embedscale::toPhysical (*this, logical);
#endif
}

int Vst3PluginEditorComponent::componentExtentFromEditor (int extent) const
{
#if defined(__APPLE__)
    return std::max (1, extent);
#else
    return embedscale::fromPhysical (*this, extent);
#endif
}

void Vst3PluginEditorComponent::tryEmbed()
{
    // Layout / hierarchy events reach here in the same message-loop turn as an
    // undo-triggered unload, so this is a plugin-code entry point like the pump.
    if (ownerIsStale()) { abandonInstance(); return; }

    // Embed ONLY when actually on-screen (toolkit-backed editors can abort
    // realising into a non-viewable parent). `embedding` breaks the re-entry
    // cycle: attached() can fire resizeView synchronously -> onResize -> setSize
    // -> resized() -> tryEmbed again (the LV2 black-rectangle bug).
    if (! loaded || embedded || embedding || ! isShowing()) return;
    const auto parent = peerNativeHandle();
    if (parent == 0) return;

#if defined(__linux__)
    if (auto* peer = getPeer())
        editor.setContentScale ((float) peer->getPlatformScaleFactor());
#endif

    const auto area = editorBoundsInPeer();
    std::string err;
    embedding = true;
    const bool ok = editor.embed (parent, area.getX(), area.getY(),
                                  std::max (1, area.getWidth()), std::max (1, area.getHeight()), err);
    embedding = false;
    if (ok)
    {
        embedded = true;
        // Adopt the view's own size once known so the modal/lane can fit to it.
        if (editor.preferredWidth() > 0 && editor.preferredHeight() > 0)
            setSize (componentExtentFromEditor (editor.preferredWidth()),
                     componentExtentFromEditor (editor.preferredHeight()));
        // Re-sync unconditionally: a synchronous resizeView during embed can
        // move this component (modal recentre) while `embedded` was still
        // false, so the moved()/resized() pushes were skipped and the native
        // window would keep the pre-move coords passed to embed().
        pushBounds();
        editor.reveal();
    }
    else
    {
        std::fprintf (stderr, "[vst3 editor] embed failed: %s\n", err.c_str());
        // A failed embed tears the Vst3Editor down; stop retrying every frame.
        loaded = false;
        stopTimer();
    }
}

void Vst3PluginEditorComponent::pushBounds()
{
    if (ownerIsStale()) return;   // setBounds drives IPlugView::onSize
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

void Vst3PluginEditorComponent::resized()                { if (embedded) pushBounds(); else tryEmbed(); }
void Vst3PluginEditorComponent::moved()                  { pushBounds(); }
void Vst3PluginEditorComponent::parentHierarchyChanged() { if (embedded) pushBounds(); else tryEmbed(); }

void Vst3PluginEditorComponent::visibilityChanged()
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

void Vst3PluginEditorComponent::abandonInstance()
{
    stopTimer();
    // A stale owner means the instance is ALREADY disposed, so releasing the
    // container is plugin contact too: the view is still attached to it, and the
    // window-detach hooks are where plugin views tear their listeners down.
    // Giving it up instead makes the close a no-op on Cocoa; on X11 close()
    // still destroys the host window and display.
    abandonNativeEditorInstance (editor, ownerIsStale());
    ownerSlot = nullptr;
    abandoned = true;
    loaded = embedded = embedding = false;
}

#if defined(__linux__)
void Vst3PluginEditorComponent::verifyGeometry()
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
                "vst3", relX, relY, area.getX(), area.getY());
    }
    if (! editor.getActualGeometry (ax, ay, aw, ah))
    {
        if (! geometryLostLogged)
        {
            geometryLostLogged = true;
            std::fprintf (stderr, "[vst3 editor] host window lost (XGetGeometry failed)\n");
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
                "[vst3 editor] geometry drift: intended (%d,%d %dx%d) actual (%d,%d %dx%d) - re-syncing\n",
                area.getX(), area.getY(), area.getWidth(), area.getHeight(), ax, ay, aw, ah);
        }
        pushBounds();
    }
}
#endif

void Vst3PluginEditorComponent::timerCallback()
{
    if (ownerIsStale()) { abandonInstance(); return; }

    // Ancestor visibility changes (tab switches) don't fire visibilityChanged -
    // poll like the CLAP/LV2 editors so the native window can't float over
    // another view. The un-embedded poll covers the mirror case: a component
    // created while its lane was hidden (session restore lands before the AUX
    // tab is first shown) gets no callback when an ANCESTOR becomes visible,
    // so the first embed must also be polled. tryEmbed no-ops until showing.
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
    const double now = juce::Time::getMillisecondCounterHiRes();
    editor.pump (now - lastPumpMs);
    lastPumpMs = now;
}
#endif

#if DUSKSTUDIO_HAS_NATIVE_AU
AuPluginEditorComponent::AuPluginEditorComponent()
{
    setOpaque (false);
    getProperties().set (kPluginEditorTag, true);
}

AuPluginEditorComponent::~AuPluginEditorComponent()
{
    stopTimer();
    if (ownerIsStale()) editor.abandonPluginAndContainer();
    else                editor.quiesce();
    editor.close();
}

bool AuPluginEditorComponent::attach (au::AuInstance& instance, juce::String& errorOut)
{
    std::string error;
    if (! editor.open (instance, error))
    {
        errorOut = "editor: " + juce::String (error);
        return false;
    }
    editor.onResize = [this] (int width, int height)
    {
        if (width > 0 && height > 0) setSize (width, height);
    };
    setSize (editor.preferredWidth() > 0 ? editor.preferredWidth() : 480,
             editor.preferredHeight() > 0 ? editor.preferredHeight() : 320);
    loaded = true;
    startTimerHz (30);
    return true;
}

std::uintptr_t AuPluginEditorComponent::peerNativeHandle() const
{
    if (auto* peer = getPeer())
        return reinterpret_cast<std::uintptr_t> (peer->getNativeHandle());
    return 0;
}

juce::Rectangle<int> AuPluginEditorComponent::editorBoundsInPeer() const
{
    return getTopLevelComponent()->getLocalArea (this, getLocalBounds());
}

void AuPluginEditorComponent::tryEmbed()
{
    if (ownerIsStale()) { abandonInstance(); return; }

    // `embedding` breaks re-entry: addSubview can let the plugin's view run
    // AppKit work that ticks this component's timer, and AuEditor only marks
    // itself embedded at the END - a nested call would build a second container.
    if (! loaded || embedded || embedding || ! isShowing()) return;
    const auto parent = peerNativeHandle();
    if (parent == 0) return;
    const auto area = editorBoundsInPeer();
    std::string error;
    embedding = true;
    const bool ok = editor.embed (parent, area.getX(), area.getY(),
                                  std::max (1, area.getWidth()),
                                  std::max (1, area.getHeight()), error);
    embedding = false;
    if (! ok)
    {
        std::fprintf (stderr, "[au editor] embed failed: %s\n", error.c_str());
        loaded = false;
        stopTimer();
        return;
    }
    embedded = true;
    pushBounds();
    editor.reveal();
}

void AuPluginEditorComponent::pushBounds()
{
    if (ownerIsStale()) return;   // setFrame reaches the plugin's own view
    if (! embedded || getParentComponent() == nullptr || getPeer() == nullptr) return;
    const auto area = editorBoundsInPeer();
    editor.setBounds (area.getX(), area.getY(),
                      std::max (1, area.getWidth()), std::max (1, area.getHeight()));
}

void AuPluginEditorComponent::resized() { if (embedded) pushBounds(); else tryEmbed(); }
void AuPluginEditorComponent::moved() { pushBounds(); }
void AuPluginEditorComponent::parentHierarchyChanged()
{
    if (embedded) pushBounds(); else tryEmbed();
}

void AuPluginEditorComponent::visibilityChanged()
{
    // Unhiding the container makes AppKit draw the plugin's Cocoa subview
    // synchronously, unlike the VST3/X11 case where reveal touches only a
    // host-owned foreign window.
    if (ownerIsStale()) { abandonInstance(); return; }
    if (! loaded) return;
    if (isShowing())
    {
        if (! embedded) tryEmbed(); else editor.reveal();
    }
    else if (embedded)
        editor.hide();
}

void AuPluginEditorComponent::abandonInstance()
{
    stopTimer();
    // Same split as the VST3 component: a stale owner means the unit is already
    // disposed, so the container detach would run the plugin view's listener
    // teardown against it, leaving close() nothing to do afterwards.
    abandonNativeEditorInstance (editor, ownerIsStale());
    ownerSlot = nullptr;
    abandoned = true;
    loaded = embedded = embedding = false;
}

void AuPluginEditorComponent::timerCallback()
{
    if (ownerIsStale()) { abandonInstance(); return; }

    if (embedded)
    {
        if (isShowing()) editor.reveal(); else editor.hide();
    }
    else
        tryEmbed();
    editor.pump();
}
#endif
} // namespace duskstudio
