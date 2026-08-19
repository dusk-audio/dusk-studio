#include "ClapPluginEditorComponent.h"
#include "EmbeddedModal.h"   // kPluginEditorTag
#if ! defined(__APPLE__)
#include "NativeEditorEmbedScale.h"
#endif

#include <algorithm>

namespace duskstudio
{
ClapPluginEditorComponent::ClapPluginEditorComponent()
{
    setOpaque (false);
    // EmbeddedModal hides tagged editors while a modal is up - see the same tag
    // in Lv2PluginEditorComponent.
    getProperties().set (kPluginEditorTag, true);
}

ClapPluginEditorComponent::~ClapPluginEditorComponent()
{
    stopTimer();
    // Every reset path lands here, and both gui->destroy and releasing the
    // container the plugin's view still sits in call into the bundle that was
    // unloaded with the instance.
    if (ownerIsStale()) editor.abandonPluginAndContainer();
    else                editor.quiesce();
    editor.close();
    if (ownsInstance) instance.deactivate();   // attach() mode: the slot owns it
}

bool ClapPluginEditorComponent::load (const juce::File& clapPath, juce::String& errorOut)
{
    std::string err;
    if (! bundle.load (clapPath.getFullPathName().toStdString(), err))
    { errorOut = "bundle: " + juce::String (err); return false; }
    if (bundle.plugins().empty())
    { errorOut = "no plugins in bundle"; return false; }

    if (! instance.create (bundle, bundle.plugins().front().id, err))
    { errorOut = "create: " + juce::String (err); return false; }
    if (! instance.activate (48000.0, 1024, err))   // editor doesn't need the real rate
    { errorOut = "activate: " + juce::String (err); return false; }

    ownsInstance = true;
    return openEditorOn (instance, errorOut);
}

bool ClapPluginEditorComponent::attach (clap::ClapInstance& shared, juce::String& errorOut)
{
    ownsInstance = false;
    return openEditorOn (shared, errorOut);
}

bool ClapPluginEditorComponent::openEditorOn (clap::ClapInstance& inst, juce::String& errorOut)
{
    std::string err;
    if (! editor.open (inst.getPlugin(), inst.getHost(), err))
    { errorOut = "editor: " + juce::String (err); return false; }

    // The plugin asked to resize -> resize this component (which re-bounds the host
    // window). The GUI closed -> tear the editor down.
    editor.onResize = [this] (int w, int h)
    {
        if (w > 0 && h > 0)
            setSize (componentExtentFromEditor (w),
                     componentExtentFromEditor (h));
    };
    // GUI was_destroyed: tear down our state fully. Leaving `loaded` set would let
    // tryEmbed()/the timer keep poking an already-destroyed editor.
    editor.onClosed = [this] { embedded = false; loaded = false; stopTimer(); };

    const int w = editor.preferredWidth()  > 0
                    ? componentExtentFromEditor (editor.preferredWidth())  : 480;
    const int h = editor.preferredHeight() > 0
                    ? componentExtentFromEditor (editor.preferredHeight()) : 320;
    setSize (w, h);

    loaded = true;
    lastPumpMs = juce::Time::getMillisecondCounter();
    startTimerHz (60);   // pump the plugin's GUI fds/timers
    return true;
}

void* ClapPluginEditorComponent::peerNativeHandle() const
{
    if (auto* peer = getPeer())
        return peer->getNativeHandle();
    return nullptr;
}

juce::Rectangle<int> ClapPluginEditorComponent::editorBoundsInPeer() const
{
    const auto logical = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
#if defined(__APPLE__)
    return logical;
#else
    return embedscale::toPhysical (*this, logical);
#endif
}

int ClapPluginEditorComponent::componentExtentFromEditor (int extent) const
{
#if defined(__APPLE__)
    return std::max (1, extent);
#else
    return embedscale::fromPhysical (*this, extent);
#endif
}

void ClapPluginEditorComponent::tryEmbed()
{
    // Layout / hierarchy events reach here in the same message-loop turn as an
    // undo-triggered unload, so this is a plugin-code entry point like the pump.
    if (ownerIsStale()) { abandonInstance(); return; }

    // Embed ONLY when actually on-screen. Some plugins (u-he Satin) abort() if asked
    // to set_parent/show into a parent that isn't viewable yet - so no pre-warm: build
    // + map when shown, exactly like the JUCE editor path. The kept-alive remap on a
    // later tab switch keeps re-opens instant.
    // `embedding` guards re-entry. Unlike LV2, a CLAP request_resize can't reach us
    // synchronously - it only records atomics, and onResize dispatches later from
    // drainPendingCallbacks() inside pump(). The vector here is the timer poll below:
    // a plugin that spins a nested message dispatch inside gui->set_parent/show lets
    // a tick re-enter tryEmbed while the first embed is still in flight, which would
    // build a SECOND GUI and orphan the first (the LV2 black-rectangle bug).
    if (! loaded || embedded || embedding || ! isShowing()) return;
    auto* parent = peerNativeHandle();
    if (parent == nullptr) return;

#if defined(_WIN32)
    // The container HWND is sized in physical pixels via embedscale, so the GUI
    // must be told the same combined factor (platform DPI x app UI zoom) before
    // it parents itself and reports a size.
    editor.setContentScale (embedscale::factor (*this));
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
        // Re-sync unconditionally: a synchronous gui resize during embed can
        // move this component (modal recentre) while `embedded` was still
        // false, so the moved()/resized() pushes were skipped and the native
        // window would keep the pre-move coords passed to embed().
        pushBounds();
        editor.reveal();
    }
    else
    {
        std::fprintf (stderr, "[clap editor] embed failed: %s\n", err.c_str());
        // A failed embed tears the ClapEditor down (set_parent/show call close()), so
        // the GUI is gone. Stop treating this component as live - otherwise the next
        // resized()/visibilityChanged would retry embed against a destroyed editor
        // every frame.
        loaded = false;
        stopTimer();
    }
}

void ClapPluginEditorComponent::pushBounds()
{
    if (ownerIsStale()) return;   // setBounds drives gui->set_size on the plugin
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

void ClapPluginEditorComponent::resized()              { if (embedded) pushBounds(); else tryEmbed(); }
void ClapPluginEditorComponent::moved()                { pushBounds(); }
void ClapPluginEditorComponent::parentHierarchyChanged() { if (embedded) pushBounds(); else tryEmbed(); }

void ClapPluginEditorComponent::visibilityChanged()
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

void ClapPluginEditorComponent::leakForShutdown()
{
    stopTimer();
    // The engine leaks its slots first, which bumps the generation - unbind so
    // that cannot read as stale and divert the destructor into a destroy.
    ownerSlot = nullptr;
    editor.setLeakOnClose (true);
}

void ClapPluginEditorComponent::abandonInstance()
{
    stopTimer();
    // A stale owner means the plugin is ALREADY destroyed, so the container is
    // plugin contact too: close() releases it with the plugin's view still
    // inside, and nothing else retains that view. Giving it up instead makes the
    // close a no-op on Cocoa; on X11 close() still destroys the host window and
    // display.
    abandonNativeEditorInstance (editor, ownerIsStale());
    ownerSlot = nullptr;
    abandoned = true;
    loaded = embedded = embedding = false;
}

#if defined(__linux__)
void ClapPluginEditorComponent::verifyGeometry()
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
                "clap", relX, relY, area.getX(), area.getY());
    }
    if (! editor.getActualGeometry (ax, ay, aw, ah))
    {
        if (! geometryLostLogged)
        {
            geometryLostLogged = true;
            std::fprintf (stderr, "[clap editor] host window lost (XGetGeometry failed)\n");
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
                "[clap editor] geometry drift: intended (%d,%d %dx%d) actual (%d,%d %dx%d) - re-syncing\n",
                area.getX(), area.getY(), area.getWidth(), area.getHeight(), ax, ay, aw, ah);
        }
        pushBounds();
    }
}
#endif

void ClapPluginEditorComponent::timerCallback()
{
    if (ownerIsStale()) { abandonInstance(); return; }

    // Keep the native child container's visible state in sync with our real on-screen
    // visibility. visibilityChanged() does NOT fire for ancestor (aux-tab / stage)
    // changes, so without this poll the container stays visible over whatever view
    // replaced the aux lane. reveal()/hide() are idempotent.
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

    const auto now = juce::Time::getMillisecondCounter();
    const auto elapsed = (double) (now - lastPumpMs);
    lastPumpMs = now;
    editor.pump (elapsed);
}
} // namespace duskstudio
