#include "ClapPluginEditorComponent.h"
#include "EmbeddedModal.h"   // kPluginEditorTag
#include "NativeEditorEmbedScale.h"

namespace duskstudio
{
ClapPluginEditorComponent::ClapPluginEditorComponent()
{
    setOpaque (false);
    // EmbeddedModal hides tagged editors while a modal is up — see the same tag
    // in Lv2PluginEditorComponent.
    getProperties().set (kPluginEditorTag, true);
}

ClapPluginEditorComponent::~ClapPluginEditorComponent()
{
    stopTimer();
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

    // The plugin asked to resize → resize this component (which re-bounds the host
    // window). The GUI closed → tear the editor down.
    editor.onResize = [this] (int w, int h)
    {
        if (w > 0 && h > 0)
            setSize (embedscale::fromPhysical (*this, w),
                     embedscale::fromPhysical (*this, h));
    };
    // GUI was_destroyed: tear down our state fully. Leaving `loaded` set would let
    // tryEmbed()/the timer keep poking an already-destroyed editor.
    editor.onClosed = [this] { embedded = false; loaded = false; stopTimer(); };

    const int w = editor.preferredWidth()  > 0
                    ? embedscale::fromPhysical (*this, editor.preferredWidth())  : 480;
    const int h = editor.preferredHeight() > 0
                    ? embedscale::fromPhysical (*this, editor.preferredHeight()) : 320;
    setSize (w, h);

    loaded = true;
    lastPumpMs = juce::Time::getMillisecondCounter();
    startTimerHz (60);   // pump the plugin's GUI fds/timers
    return true;
}

unsigned long ClapPluginEditorComponent::peerX11() const
{
    if (auto* peer = getPeer())
        return (unsigned long) (juce::pointer_sized_uint) peer->getNativeHandle();
    return 0;
}

void ClapPluginEditorComponent::tryEmbed()
{
    // Embed ONLY when actually on-screen. Some plugins (u-he Satin) abort() if asked
    // to set_parent/show into a parent that isn't viewable yet — so no pre-warm: build
    // + map when shown, exactly like the JUCE editor path. The kept-alive remap on a
    // later tab switch keeps re-opens instant.
    if (! loaded || embedded || ! isShowing()) return;
    const auto parent = peerX11();
    if (parent == 0) return;

    const auto area = embedscale::toPhysical (
        *this, getTopLevelComponent()->getLocalArea (this, getLocalBounds()));
    std::string err;
    if (editor.embed (parent, area.getX(), area.getY(),
                      juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()), err))
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
        // the GUI is gone. Stop treating this component as live — otherwise the next
        // resized()/visibilityChanged would retry embed against a destroyed editor
        // every frame.
        loaded = false;
        stopTimer();
    }
}

void ClapPluginEditorComponent::pushBounds()
{
    if (! embedded) return;
    // Borrowed bodies get setBounds'd by EmbeddedModal BEFORE being re-added
    // to a parent — getTopLevelComponent() is then `this` and the area
    // degenerates to (0,0), slamming the native window to the origin. Skip
    // while unparented; parentHierarchyChanged re-syncs once re-added.
    if (getParentComponent() == nullptr || getPeer() == nullptr) return;
    const auto area = embedscale::toPhysical (
        *this, getTopLevelComponent()->getLocalArea (this, getLocalBounds()));
    editor.setBounds (area.getX(), area.getY(),
                      juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()));
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
    editor.setLeakOnClose (true);
}

void ClapPluginEditorComponent::timerCallback()
{
    // Keep the native X11 window's mapped state in sync with our real on-screen
    // visibility. visibilityChanged() does NOT fire for ancestor (aux-tab / stage)
    // changes, so without this poll the window stays mapped — floating over whatever
    // view replaced the aux lane. reveal()/hide() are idempotent (guarded by `mapped`).
    if (embedded)
    {
        if (isShowing()) editor.reveal();
        else             editor.hide();
    }

    const auto now = juce::Time::getMillisecondCounter();
    const auto elapsed = (double) (now - lastPumpMs);
    lastPumpMs = now;
    editor.pump (elapsed);
}
} // namespace duskstudio
