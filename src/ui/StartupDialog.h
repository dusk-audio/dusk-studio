#pragma once

#include <juce_core/juce_core.h>
#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <vector>

namespace duskstudio
{
// Shown once on app launch. Ardour-style layout: a sidebar (Dusk wordmark
// + Recent / Open / New nav) on the left, a table of recent sessions in
// the middle, and a Quit / Open footer at the bottom.
//
// All actions are reported via callbacks; the dialog itself doesn't touch
// the Session/AudioEngine. MainComponent wires the callbacks to its
// session-management helpers.
class StartupDialog final : public juce::Component,
                              private juce::TableListBoxModel,
                              private juce::Timer
{
public:
    explicit StartupDialog (juce::Array<juce::File> recentSessions);

    void resized() override;
    void paint (juce::Graphics&) override;
    bool keyPressed (const juce::KeyPress& key) override;
    void mouseUp (const juce::MouseEvent& e) override;

    // Shows the "update available" banner above the sessions heading.
    // Called (message thread) when the async tag check finds a release
    // newer than this build; never called when up to date. The banner is
    // click- and keyboard-activatable and opens the downloads page.
    void setUpdateAvailable (const juce::String& tagName);

    // Each action callback also triggers the dialog to dismiss itself.
    std::function<void (juce::File)> onOpenRecent;   // arg = recent session dir
    std::function<void()>            onNewSession;    // dir picker handled by host
    std::function<void()>            onOpenFile;      // file picker handled by host
    std::function<void()>            onSkip;          // dismiss without action
    std::function<void()>            onQuit;          // user wants to quit the app

    // Fired when the dialog wants to be torn down (after any of the action
    // callbacks above). Set by hosts that embed the dialog as a child
    // component instead of a juce::DialogWindow.
    std::function<void()>            onDismiss;

private:
    void closeDialog (int returnCode);
    void openSelectedRow();
    void openDownloadsPage();
    void timerCallback() override;   // flashes the update badge

    // juce::TableListBoxModel
    int  getNumRows() override;
    void paintRowBackground (juce::Graphics& g, int rowNumber, int width, int height,
                                bool rowIsSelected) override;
    void paintCell (juce::Graphics& g, int rowNumber, int columnId,
                      int width, int height, bool rowIsSelected) override;
    void cellDoubleClicked (int rowNumber, int columnId,
                              const juce::MouseEvent&) override;
    void selectedRowsChanged (int) override;

    // One row per recent session. Sample rate + bit depth are inferred
    // by peeking at the first audio file in <dir>/audio/ when the
    // dialog is constructed; the result is cached so paintCell stays
    // cheap. Empty strings render as an em-dash placeholder.
    struct RecentRow
    {
        juce::File    dir;
        juce::String  name;
        juce::String  sampleRate;
        juce::String  bitDepth;
        juce::String  lastModified;
    };
    std::vector<RecentRow> rows;

    juce::ImageComponent brandIcon;
    juce::Label   brandWordmark;
    juce::Label   tableHeading;
    juce::Label   emptyLabel;
    juce::Label   updateLabel;       // hidden until setUpdateAvailable
    bool          updateFlashOn = true;
    int           updateBlinkCount = 0;

    juce::TableListBox table;

    juce::TextButton recentTab  { "RECENT" };
    juce::TextButton openTab    { "OPEN" };
    juce::TextButton newTab     { "NEW" };
    juce::TextButton quitButton { "Quit" };
    juce::TextButton openButton { "Open" };

    enum ColumnIds
    {
        kColName       = 1,
        kColSampleRate = 2,
        kColBitDepth   = 3,
        kColModified   = 4,
    };
};
} // namespace duskstudio
