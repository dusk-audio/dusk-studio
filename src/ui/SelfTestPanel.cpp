#include "SelfTestPanel.h"
#include "../engine/AudioEngine.h"
#include "../engine/AudioPipelineSelfTest.h"
#include "../session/Session.h"

namespace duskstudio
{
SelfTestPanel::SelfTestPanel (AudioEngine& e,
                                device::DeviceManager& dm,
                                Session& s)
    : engine (e), deviceManager (dm), session (s)
{
    logView.setMultiLine (true);
    logView.setReadOnly (true);
    logView.setScrollbarsShown (true);
    logView.setCaretVisible (false);
    logView.setReturnKeyStartsNewLine (false);
    logView.setFont (juce::Font (juce::Font::getDefaultMonospacedFontName(), 12.0f, 0));
    logView.setPopupMenuEnabled (false);   // XWayland popup flash (see DuskComboBox)
    logView.setColour (juce::TextEditor::backgroundColourId, juce::Colour (0xff101012));
    logView.setColour (juce::TextEditor::textColourId, juce::Colour (0xffd0d0d0));
    logView.setText ("Press [Run] to execute the audio pipeline self-test. "
                     "The test mutates session state during the run and restores "
                     "it on completion. The audio engine is detached from the "
                     "device manager during synthetic tests, so any in-progress "
                     "audio will pause briefly.\n");
    addAndMakeVisible (logView);

    runButton.onClick   = [this] { runTest(); };
    copyButton.onClick  = [this] { copyToClipboard(); };
    closeButton.onClick = [this]
    {
        // Hosted in an EmbeddedModal, not a DialogWindow - ask the host to
        // close it (the old DialogWindow lookup always returned nullptr, so
        // this button did nothing and only Esc / click-outside dismissed).
        if (onCloseRequested) onCloseRequested();
    };
    addAndMakeVisible (runButton);
    addAndMakeVisible (copyButton);
    addAndMakeVisible (closeButton);
}

void SelfTestPanel::paint (juce::Graphics& g)
{
    g.fillAll (juce::Colour (0xff202024));
}

void SelfTestPanel::resized()
{
    auto area = getLocalBounds().reduced (8);
    auto buttons = area.removeFromBottom (32);
    runButton.setBounds   (buttons.removeFromLeft (80));
    buttons.removeFromLeft (8);
    copyButton.setBounds  (buttons.removeFromLeft (80));
    closeButton.setBounds (buttons.removeFromRight (80));
    area.removeFromBottom (8);
    logView.setBounds (area);
}

void SelfTestPanel::runTest()
{
    runButton.setEnabled (false);
    logView.setText ("Running... (this can take a few seconds while backends are cycled)\n");
    logView.repaint();

    // The test does some heavy I/O (device opens) on the message thread.
    // We yield via MessageManager::callAsync so the "Running..." text paints
    // before we block on the synchronous test run.
    //
    // SafePointer guards the small race between callAsync scheduling the
    // lambda and the message loop actually running it: if the user closes
    // this panel in that window (or the parent dialog gets force-deleted on
    // app shutdown), the lambda would otherwise dereference freed memory.
    // Once the lambda is ACTIVELY running, the message thread is blocked
    // for the duration of the test, so the panel can't be destroyed
    // mid-execution; only the queued-but-not-started case is dangerous.
    juce::Component::SafePointer<SelfTestPanel> safeThis (this);
    juce::MessageManager::callAsync ([safeThis]
    {
        if (safeThis == nullptr) return;
        auto& self = *safeThis;
        AudioPipelineSelfTest test (self.engine, self.deviceManager, self.session);
        const auto result = test.runAll();
        self.logView.setText (result);
        self.runButton.setEnabled (true);
    });
}

void SelfTestPanel::copyToClipboard()
{
    juce::SystemClipboard::copyTextToClipboard (logView.getText());
}
} // namespace duskstudio
