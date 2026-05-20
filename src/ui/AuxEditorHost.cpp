#include "AuxEditorHost.h"
#include "PlatformWindowing.h"
#include "../engine/AudioEngine.h"

namespace duskstudio
{
class AuxEditorHost::EditorPanel final : public juce::Component
{
public:
    explicit EditorPanel (juce::Component& editorIn) : editorSafe (&editorIn)
    {
        addAndMakeVisible (editorIn);
    }

    void resized() override
    {
        auto* editor = editorSafe.getComponent();
        if (editor == nullptr) return;

        if (auto* ape = dynamic_cast<juce::AudioProcessorEditor*> (editor))
        {
            if (ape->isResizable())
            {
                editor->setBounds (getLocalBounds());
            }
            else
            {
                const int ew = editor->getWidth();
                const int eh = editor->getHeight();
                const int x = juce::jmax (0, (getWidth()  - ew) / 2);
                const int y = juce::jmax (0, (getHeight() - eh) / 2);
                editor->setTopLeftPosition (x, y);
            }
        }
        else
        {
            // Non-AudioProcessorEditor branch: centre at native size instead
            // of stretching (matches the fixed-layout AudioProcessorEditor
            // branch above). Falls back to fill if the editor reports zero
            // size so it stays visible.
            const int ew = editor->getWidth();
            const int eh = editor->getHeight();
            if (ew > 0 && eh > 0)
                editor->setBounds (getLocalBounds().withSizeKeepingCentre (ew, eh));
            else
                editor->setBounds (getLocalBounds());
        }
    }

    juce::Component* getEditor() const noexcept { return editorSafe.getComponent(); }

private:
    juce::Component::SafePointer<juce::Component> editorSafe;
};

AuxEditorHost::AuxEditorHost (const juce::String& title,
                                juce::Component& editor,
                                juce::AudioProcessor* processorForResizability,
                                AudioEngine* engineForTransport)
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
                              /*requiredButtons*/ 0,
                              /*addToDesktop*/ true),
      trackedEditor (&editor),
      processor (processorForResizability),
      enginePtr (engineForTransport)
{
    setUsingNativeTitleBar (false);
    setTitleBarHeight (0);
    setDropShadowEnabled (false);
    setResizable (false, false);
    setWantsKeyboardFocus (false);

    editorPanel = std::make_unique<EditorPanel> (editor);
    editorPanel->setSize (juce::jmax (200, editor.getWidth()),
                          juce::jmax (200, editor.getHeight()));
    setContentNonOwned (editorPanel.get(), /*resizeToFitContent*/ true);

    // Without an explicit setVisible(true) the peer is mapped but the
    // window arrives iconified on Mutter/XWayland, forcing the user to
    // click the slot to surface the UI. PluginEditorWindow has the same
    // line for the channel-strip path.
    setVisible (true);

    // Bring above the main window so the user sees the editor immediately.
    juce::Component::SafePointer<AuxEditorHost> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (auto* self = safeThis.getComponent())
            if (auto* peer = self->getPeer())
                duskstudio::platform::bringWindowToFront (*peer);
    });

    // LV2 + some VST3 editors finalize their preferred size on a later
    // idle pump; the size known at construction is stale. Same staged
    // re-fit cadence PluginEditorWindow uses for the channel-strip path.
    for (int delayMs : { 100, 350, 800 })
    {
        juce::Component::SafePointer<AuxEditorHost> refit (this);
        juce::Timer::callAfterDelay (delayMs, [refit]
        {
            if (auto* self = refit.getComponent())
                self->applyEditorLayout();
        });
    }

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

void AuxEditorHost::setLaneScreenBounds (juce::Rectangle<int> screenRect)
{
    if (screenRect.isEmpty()) return;
    if (screenRect == lastBounds) return;
    lastBounds = screenRect;
    setBounds (screenRect);
    applyEditorLayout();
}

void AuxEditorHost::setHostHidden (bool hidden)
{
    if (hidden == ! isVisible()) return;
    setVisible (! hidden);
}

void AuxEditorHost::resized()
{
    juce::DocumentWindow::resized();
    applyEditorLayout();
}

void AuxEditorHost::applyEditorLayout()
{
    if (editorPanel == nullptr) return;
    // LV2 + some VST3 editors finalise their preferred size on a later
    // idle pump than construction. If the editor has grown since we last
    // sized the panel, the inner editor will clip when resized() centres
    // it. Resize the panel to fit the editor's current native size first,
    // then grow the host window to match (this is a borderless
    // DocumentWindow so the content area equals the panel size), then
    // run the layout so resizable editors stretch and non-resizable
    // editors recentre cleanly.
    if (auto* editor = editorPanel->getEditor())
    {
        const int ew = editor->getWidth();
        const int eh = editor->getHeight();
        if (ew > editorPanel->getWidth() || eh > editorPanel->getHeight())
        {
            const int newW = juce::jmax (editorPanel->getWidth(),  ew);
            const int newH = juce::jmax (editorPanel->getHeight(), eh);
            editorPanel->setSize (newW, newH);
            // Grow the host's window to match. setContentNonOwned's
            // resizeToFitContent flag only sizes the window on initial
            // attach, so mutating the panel later requires an explicit
            // setSize call here. The host is borderless / no title bar,
            // so content size == window size.
            if (getWidth() < newW || getHeight() < newH)
                setSize (juce::jmax (getWidth(),  newW),
                          juce::jmax (getHeight(), newH));
        }
    }
    editorPanel->resized();
}
} // namespace duskstudio
