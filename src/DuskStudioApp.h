#pragma once

#include <juce_gui_basics/juce_gui_basics.h>

namespace duskstudio
{
class DuskStudioApp final : public juce::JUCEApplication
{
public:
    DuskStudioApp();
    ~DuskStudioApp() override;

    const juce::String getApplicationName() override       { return JUCE_APPLICATION_NAME_STRING; }
    const juce::String getApplicationVersion() override    { return JUCE_APPLICATION_VERSION_STRING; }
    bool moreThanOneInstanceAllowed() override             { return true; }

    void initialise (const juce::String& commandLine) override;
    void shutdown() override;
    void systemRequestedQuit() override;
    void anotherInstanceStarted (const juce::String& commandLine) override;

private:
    class MainWindow;
    std::unique_ptr<MainWindow> mainWindow;
};
} // namespace duskstudio
