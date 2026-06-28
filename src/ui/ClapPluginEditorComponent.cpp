#include "ClapPluginEditorComponent.h"

namespace duskstudio
{
ClapPluginEditorComponent::ClapPluginEditorComponent()
{
    setOpaque (false);
}

ClapPluginEditorComponent::~ClapPluginEditorComponent()
{
    stopTimer();
    editor.close();
    instance.deactivate();
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

    if (! editor.open (instance.getPlugin(), instance.getHost(), err))
    { errorOut = "editor: " + juce::String (err); return false; }

    // The plugin asked to resize → resize this component (which re-bounds the host
    // window). The GUI closed → tear the editor down.
    editor.onResize = [this] (int w, int h)
    {
        if (w > 0 && h > 0) setSize (w, h);
    };
    editor.onClosed = [this] { embedded = false; };

    const int w = editor.preferredWidth()  > 0 ? editor.preferredWidth()  : 480;
    const int h = editor.preferredHeight() > 0 ? editor.preferredHeight() : 320;
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
    if (! loaded || embedded || ! isShowing()) return;
    const auto parent = peerX11();
    if (parent == 0) return;

    const auto area = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
    std::string err;
    if (editor.embed (parent, area.getX(), area.getY(),
                      juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()), err))
    {
        embedded = true;
        editor.reveal();   // visible now (this is the on-screen, shown case)
    }
    else
    {
        std::fprintf (stderr, "[clap editor] embed failed: %s\n", err.c_str());
    }
}

void ClapPluginEditorComponent::pushBounds()
{
    if (! embedded) return;
    const auto area = getTopLevelComponent()->getLocalArea (this, getLocalBounds());
    editor.setBounds (area.getX(), area.getY(),
                      juce::jmax (1, area.getWidth()), juce::jmax (1, area.getHeight()));
}

void ClapPluginEditorComponent::resized()              { if (embedded) pushBounds(); else tryEmbed(); }
void ClapPluginEditorComponent::moved()                { pushBounds(); }
void ClapPluginEditorComponent::parentHierarchyChanged() { tryEmbed(); }

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

void ClapPluginEditorComponent::timerCallback()
{
    const auto now = juce::Time::getMillisecondCounter();
    const auto elapsed = (double) (now - lastPumpMs);
    lastPumpMs = now;
    editor.pump (elapsed);
}
} // namespace duskstudio
