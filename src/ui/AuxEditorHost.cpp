#include "AuxEditorHost.h"
#include "PlatformWindowing.h"
#include "../engine/AudioEngine.h"

namespace duskstudio
{
AuxEditorHost::AuxEditorHost (const juce::String& title,
                                juce::Component& editor,
                                juce::AudioProcessor* processorForResizability,
                                AudioEngine* engineForTransport,
                                std::function<void()> onClose)
    : juce::DocumentWindow (([&]
                              {
                                  // X11-route the toplevel peer so VST3 /
                                  // LV2 X11 sub-windows have an X11 host
                                  // to reparent into. Same comma-expression
                                  // pattern PluginEditorWindow uses.
                                  duskstudio::platform::preferX11ForNextNativeWindow();
                                  return title;
                              })(),
                              juce::Colour (0xff202024),
                              juce::DocumentWindow::closeButton,
                              /*addToDesktop*/ true),
      trackedEditor (&editor),
      processor (processorForResizability),
      enginePtr (engineForTransport),
      onCloseCallback (std::move (onClose))
{
    setUsingNativeTitleBar (true);

    const int ew = juce::jmax (200, editor.getWidth());
    const int eh = juce::jmax (200, editor.getHeight());
    editor.setSize (ew, eh);

    // Size the WINDOW first (so its X11 peer is allocated at the right
    // geometry), then map it. Deliberately do NOT call setContentNonOwned
    // yet - on Linux, JUCE's VST3PluginWindow owns an XEmbedComponent
    // whose host X11 window is parented to root at construction. The
    // reparent into our peer happens in componentVisibilityChanged ->
    // attachPluginWindow. If we attach the editor BEFORE our DocumentWindow's
    // peer is fully realized, the reparent fires against a not-yet-mapped
    // window and the plugin's X11 child window stays at root - the
    // visible result is a blank/white editor interior. Defer the
    // content-set to after setVisible has propagated through one
    // message loop tick.
    setSize (ew, eh);
    setResizable (false, false);
    centreAroundComponent (nullptr, ew, eh);
    setVisible (true);

    juce::Component::SafePointer<AuxEditorHost> attachThis (this);
    juce::Component::SafePointer<juce::Component> attachEditor (&editor);
    juce::MessageManager::callAsync ([attachThis, attachEditor]
    {
        auto* self = attachThis.getComponent();
        auto* ed   = attachEditor.getComponent();
        if (self == nullptr || ed == nullptr) return;
        self->setContentNonOwned (ed, /*resizeToFitContent*/ true);
    });

    // Staged re-fits. LV2 plugin UIs finalize their preferred geometry
    // after the first few X11 idle pumps - the bounds reported at
    // createEditorIfNeeded() time are stale. Re-pull the editor's
    // current size and inflate the window to match at 100 / 350 / 800 ms.
    for (int delayMs : { 100, 350, 800 })
    {
        juce::Component::SafePointer<AuxEditorHost> refit (this);
        juce::Component::SafePointer<juce::Component> rEditor (&editor);
        juce::Timer::callAfterDelay (delayMs, [refit, rEditor]
        {
            auto* self = refit.getComponent();
            auto* ed   = rEditor.getComponent();
            if (self == nullptr || ed == nullptr) return;
            const int ew = ed->getWidth();
            const int eh = ed->getHeight();
            if (ew <= 0 || eh <= 0) return;
            auto* content = self->getContentComponent();
            if (content == nullptr) return;
            if (content->getWidth() == ew && content->getHeight() == eh) return;
            self->setContentComponentSize (ew, eh);
        });
    }

    // Promote above the main window so the user sees the editor
    // immediately after load. No-op on non-Linux.
    juce::Component::SafePointer<AuxEditorHost> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (auto* self = safeThis.getComponent())
            if (auto* peer = self->getPeer())
                duskstudio::platform::bringWindowToFront (*peer);
    });

    duskstudio::platform::clearPreferX11ForNativeWindow();
}

AuxEditorHost::~AuxEditorHost()
{
    setContentNonOwned (nullptr, false);
}

bool AuxEditorHost::keyPressed (const juce::KeyPress& k)
{
    if (enginePtr == nullptr) return false;
    if (! k.getModifiers().isAnyModifierKeyDown()
        && k == juce::KeyPress::spaceKey)
    {
        auto& transport = enginePtr->getTransport();
        if (transport.isStopped()) enginePtr->play();
        else                       enginePtr->stop();
        return true;
    }
    return false;
}

void AuxEditorHost::closeButtonPressed()
{
    trackedEditor = nullptr;
    setContentNonOwned (nullptr, false);
    duskstudio::platform::prepareForTopLevelDestruction (*this);
    if (onCloseCallback) onCloseCallback();
}

void AuxEditorHost::setHostHidden (bool hidden)
{
    if (hidden == ! isVisible()) return;
    setVisible (! hidden);
}
} // namespace duskstudio
