#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

#include "../engine/au/AuEditor.h"
#include "../foundation/MessageThread.h"

namespace duskstudio
{
namespace au { class AuInstance; }

class AuPluginEditorComponent final : public juce::Component,
                                      private dusk::Timer
{
public:
    AuPluginEditorComponent();
    ~AuPluginEditorComponent() override;

    bool attach (au::AuInstance& instance, juce::String& errorOut);
    bool isLoaded() const noexcept { return loaded; }

    void resized() override;
    void moved() override;
    void parentHierarchyChanged() override;
    void visibilityChanged() override;

private:
    void timerCallback() override;
    void tryEmbed();
    void pushBounds();
    std::uintptr_t peerNativeHandle() const;
    juce::Rectangle<int> editorBoundsInPeer() const;

    au::AuEditor editor;
    bool loaded = false;
    bool embedded = false;
};
} // namespace duskstudio
