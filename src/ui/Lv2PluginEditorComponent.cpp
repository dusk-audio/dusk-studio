#include "Lv2PluginEditorComponent.h"

#include "EmbeddedModal.h"   // kPluginEditorTag
#include "NativeEditorEmbedScale.h"
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
    editor.close();
}

bool Lv2PluginEditorComponent::attach (lv2::Lv2Instance& shared, juce::String& errorOut)
{
    std::string err;
    if (! editor.open (shared, err))
    { errorOut = "editor: " + juce::String (err); return false; }

    editor.onResize = [this] (int w, int h)
    {
        if (w > 0 && h > 0)
            setSize (embedscale::fromPhysical (*this, w),
                     embedscale::fromPhysical (*this, h));
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
    startTimerHz (60);   // ui:idleInterface + X event drain
    return true;
}

unsigned long Lv2PluginEditorComponent::peerX11() const
{
    if (auto* peer = getPeer())
        return (unsigned long) (juce::pointer_sized_uint) peer->getNativeHandle();
    return 0;
}

void Lv2PluginEditorComponent::tryEmbed()
{
    // Embed ONLY when actually on-screen: the UI instantiates directly into our
    // host window, and toolkit wrappers can abort realising into a non-viewable
    // parent (same rule as the CLAP editor).
    // `embedding` breaks the re-entry cycle: suil instantiation runs the UI's
    // ui:resize synchronously -> onResize -> setSize -> resized() -> tryEmbed again,
    // which would build a SECOND UI instance and orphan the first (the black-
    // rectangle bug).
    if (! loaded || embedded || embedding || ! isShowing()) return;
    const auto parent = peerX11();
    if (parent == 0) return;

    const auto area = embedscale::toPhysical (
        *this, getTopLevelComponent()->getLocalArea (this, getLocalBounds()));
    std::string err;
    embedding = true;
    const bool ok = editor.embed (parent, area.getX(), area.getY(),
                                  std::max (1, area.getWidth()), std::max (1, area.getHeight()), err);
    embedding = false;
    if (ok)
    {
        embedded = true;
        // Adopt the UI's own size once known so the modal/lane can fit to it.
        if (editor.preferredWidth() > 0 && editor.preferredHeight() > 0)
            setSize (embedscale::fromPhysical (*this, editor.preferredWidth()),
                     embedscale::fromPhysical (*this, editor.preferredHeight()));
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
    if (! embedded) return;
    // Borrowed bodies get setBounds'd by EmbeddedModal BEFORE being re-added
    // to a parent - getTopLevelComponent() is then `this` and the area
    // degenerates to (0,0), slamming the native window to the origin. Skip
    // while unparented; parentHierarchyChanged re-syncs once re-added.
    if (getParentComponent() == nullptr || getPeer() == nullptr) return;
    const auto area = embedscale::toPhysical (
        *this, getTopLevelComponent()->getLocalArea (this, getLocalBounds()));
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
    editor.setLeakOnClose (true);
}

void Lv2PluginEditorComponent::verifyGeometry()
{
    // The message flow can miss a move (compositor interference, an event
    // arriving while unparented) - poll the REAL geometry ~3 Hz, snap back on
    // drift, and log the numbers so field reports say what actually happened.
    if (! isShowing() || getParentComponent() == nullptr || getPeer() == nullptr) return;
    if (++geometryCheckTick < 20) return;
    geometryCheckTick = 0;

    const auto area = embedscale::toPhysical (
        *this, getTopLevelComponent()->getLocalArea (this, getLocalBounds()));
    int ax = 0, ay = 0, aw = 0, ah = 0;
    if (! embedCheckLogged)
    {
        embedCheckLogged = true;
        int relX = 0, relY = 0;
        if (editor.getRootRelativePosition (peerX11(), relX, relY))
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

void Lv2PluginEditorComponent::timerCallback()
{
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
        verifyGeometry();
    }
    else
    {
        tryEmbed();
    }
    editor.pump();
}
} // namespace duskstudio
