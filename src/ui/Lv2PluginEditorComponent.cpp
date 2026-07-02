#include "Lv2PluginEditorComponent.h"

#include "../engine/lv2/Lv2Instance.h"

namespace duskstudio
{
Lv2PluginEditorComponent::Lv2PluginEditorComponent()
{
    setOpaque (false);
    // EmbeddedModal hides tagged editors while a modal is up — the native X11
    // window otherwise paints ABOVE the modal regardless of JUCE z-order,
    // burying dialogs under the plugin UI.
    getProperties().set ("dusk_pluginEditor", true);
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
        if (w > 0 && h > 0) setSize (w, h);
    };
    // The UI reported closed (idle() non-zero): stop treating it as live AND tear
    // the native UI down — leaving it open would keep a dead-by-its-own-request
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
    // ui:resize synchronously → onResize → setSize → resized() → tryEmbed again,
    // which would build a SECOND UI instance and orphan the first (the black-
    // rectangle bug).
    if (! loaded || embedded || embedding || ! isShowing()) return;
    const auto parent = peerX11();
    if (parent == 0) return;

    const auto area = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
    std::string err;
    embedding = true;
    const bool ok = editor.embed (parent, area.getX(), area.getY(),
                                  juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()), err);
    embedding = false;
    if (ok)
    {
        embedded = true;
        // Adopt the UI's own size once known so the modal/lane can fit to it.
        if (editor.preferredWidth() > 0 && editor.preferredHeight() > 0)
            setSize (editor.preferredWidth(), editor.preferredHeight());
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
    const auto area = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
    editor.setBounds (area.getX(), area.getY(),
                      juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()));
}

void Lv2PluginEditorComponent::resized()                { if (embedded) pushBounds(); else tryEmbed(); }
void Lv2PluginEditorComponent::moved()                  { pushBounds(); }
void Lv2PluginEditorComponent::parentHierarchyChanged() { tryEmbed(); }

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

void Lv2PluginEditorComponent::timerCallback()
{
    // Ancestor visibility changes (tab switches) don't fire visibilityChanged —
    // poll like the CLAP editor so the native window can't float over another view.
    if (embedded)
    {
        if (isShowing()) editor.reveal();
        else             editor.hide();
    }
    editor.pump();
}
} // namespace duskstudio
