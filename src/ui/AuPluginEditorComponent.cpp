#include "AuPluginEditorComponent.h"

#include "EmbeddedModal.h"
#include "../engine/au/AuInstance.h"

#include <algorithm>

namespace duskstudio
{
AuPluginEditorComponent::AuPluginEditorComponent()
{
    setOpaque (false);
    getProperties().set (kPluginEditorTag, true);
}

AuPluginEditorComponent::~AuPluginEditorComponent()
{
    stopTimer();
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
    if (! loaded || embedded || ! isShowing()) return;
    const auto parent = peerNativeHandle();
    if (parent == 0) return;
    const auto area = editorBoundsInPeer();
    std::string error;
    if (! editor.embed (parent, area.getX(), area.getY(),
                        std::max (1, area.getWidth()), std::max (1, area.getHeight()), error))
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
    if (! loaded) return;
    if (isShowing())
    {
        if (! embedded) tryEmbed(); else editor.reveal();
    }
    else if (embedded)
        editor.hide();
}

void AuPluginEditorComponent::timerCallback()
{
    if (embedded)
    {
        if (isShowing()) editor.reveal(); else editor.hide();
    }
    else
        tryEmbed();
    editor.pump();
}
} // namespace duskstudio
