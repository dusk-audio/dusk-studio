#pragma once

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio
{
// Shown once on app launch. Lets the user pick from recent sessions, open a
// session.json, create a new session in a chosen directory, or skip and
// continue with the default empty session.
//
// All actions are reported via callbacks; the dialog itself doesn't touch
// the Session/AudioEngine. MainComponent wires the callbacks to its
// session-management helpers.
class StartupDialog final : public juce::Component
{
public:
    explicit StartupDialog (juce::Array<juce::File> recentSessions);

    void resized() override;
    void paint (juce::Graphics&) override;

    // Each callback also triggers the dialog to dismiss itself.
    std::function<void (juce::File)> onOpenRecent;   // arg = recent session dir
    std::function<void()>            onNewSession;    // dir picker handled by host
    std::function<void()>            onOpenFile;      // file picker handled by host
    std::function<void()>            onSkip;

    // Fired when the dialog wants to be torn down (after any of the action
    // callbacks above). Set by hosts that embed the dialog as a child
    // component instead of a juce::DialogWindow — the embedded path has
    // no exitModalState equivalent, so the host must do its own cleanup.
    std::function<void()>            onDismiss;

private:
    void closeDialog (int returnCode);
    void rebuildRecentButtons();

    juce::Array<juce::File> recents;

    juce::Label titleLabel;
    juce::Label recentsHeading;
    juce::Label emptyRecentsLabel;
    juce::OwnedArray<juce::TextButton> recentButtons;

    juce::TextButton newSessionButton { "New session..." };
    juce::TextButton openFileButton   { "Open..." };
    juce::TextButton skipButton       { "Continue blank" };
};
} // namespace duskstudio
