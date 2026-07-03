#include "Vst3PluginEditorComponent.h"

#include "EmbeddedModal.h"   // kPluginEditorTag
#include "../engine/vst3/Vst3Instance.h"

namespace duskstudio
{
Vst3PluginEditorComponent::Vst3PluginEditorComponent()
{
    setOpaque (false);
    // EmbeddedModal hides tagged editors while a modal is up — the native X11
    // window otherwise paints ABOVE the modal regardless of JUCE z-order,
    // burying dialogs under the plugin UI.
    getProperties().set (kPluginEditorTag, true);
}

Vst3PluginEditorComponent::~Vst3PluginEditorComponent()
{
    stopTimer();
    editor.close();
}

bool Vst3PluginEditorComponent::attach (vst3::Vst3Instance& shared, juce::String& errorOut)
{
    std::string err;
    if (! editor.open (shared, err))
    { errorOut = "editor: " + juce::String (err); return false; }

    editor.onResize = [this] (int w, int h)
    {
        if (w > 0 && h > 0) setSize (w, h);
    };

    if (editor.preferredWidth() > 0 && editor.preferredHeight() > 0)
        setSize (editor.preferredWidth(), editor.preferredHeight());
    else
        setSize (480, 320);

    loaded = true;
    lastPumpMs = juce::Time::getMillisecondCounterHiRes();
    startTimerHz (60);   // IRunLoop fds/timers + X event drain
    return true;
}

unsigned long Vst3PluginEditorComponent::peerX11() const
{
    if (auto* peer = getPeer())
        return (unsigned long) (juce::pointer_sized_uint) peer->getNativeHandle();
    return 0;
}

void Vst3PluginEditorComponent::tryEmbed()
{
    // Embed ONLY when actually on-screen (toolkit-backed editors can abort
    // realising into a non-viewable parent). `embedding` breaks the re-entry
    // cycle: attached() can fire resizeView synchronously → onResize → setSize
    // → resized() → tryEmbed again (the LV2 black-rectangle bug).
    if (! loaded || embedded || embedding || ! isShowing()) return;
    const auto parent = peerX11();
    if (parent == 0) return;

    if (auto* peer = getPeer())
        editor.setContentScale ((float) peer->getPlatformScaleFactor());

    const auto area = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
    std::string err;
    embedding = true;
    const bool ok = editor.embed (parent, area.getX(), area.getY(),
                                  juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()), err);
    embedding = false;
    if (ok)
    {
        embedded = true;
        // Adopt the view's own size once known so the modal/lane can fit to it.
        if (editor.preferredWidth() > 0 && editor.preferredHeight() > 0)
            setSize (editor.preferredWidth(), editor.preferredHeight());
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
    if (! embedded) return;
    const auto area = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
    editor.setBounds (area.getX(), area.getY(),
                      juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()));
}

void Vst3PluginEditorComponent::resized()                { if (embedded) pushBounds(); else tryEmbed(); }
void Vst3PluginEditorComponent::moved()                  { pushBounds(); }
void Vst3PluginEditorComponent::parentHierarchyChanged() { tryEmbed(); }

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

void Vst3PluginEditorComponent::timerCallback()
{
    // Ancestor visibility changes (tab switches) don't fire visibilityChanged —
    // poll like the CLAP/LV2 editors so the native window can't float over
    // another view. The un-embedded poll covers the mirror case: a component
    // created while its lane was hidden (session restore lands before the AUX
    // tab is first shown) gets no callback when an ANCESTOR becomes visible,
    // so the first embed must also be polled. tryEmbed no-ops until showing.
    if (embedded)
    {
        if (isShowing()) editor.reveal();
        else             editor.hide();
    }
    else
    {
        tryEmbed();
    }
    const double now = juce::Time::getMillisecondCounterHiRes();
    editor.pump (now - lastPumpMs);
    lastPumpMs = now;
}
} // namespace duskstudio
