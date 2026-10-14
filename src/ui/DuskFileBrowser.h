#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "FileBrowserChoice.h"
#include <filesystem>
#include <functional>

namespace duskstudio::filebrowser
{
struct Options
{
    juce::String title;                  // shown in the modal header
    juce::File   initialFileOrDirectory; // path to seed (file selected when Save); empty = home
    juce::String filePatternsAllowed;    // "*.wav;*.aiff" - empty = any
    Mode         mode = Mode::Open;
    bool         selectDirectories     = false; // false = files only
    bool         saveAnswerIsFolder    = false; // Save mode: the name is a folder to create or reuse
};

// In-window Dusk-native file browser. Hosts juce::FileBrowserComponent
// inside an EmbeddedModal-hosted panel - no juce::FileChooser standalone
// window, no Wayland positioning / stacking issues, parented to the
// main app window so it inherits the DAW's modal grammar.
//
// `host` is any Component inside the main window (typically `this` from
// a caller's method); the modal is shown on the host's top-level.
// `onResult` fires once with the chosen file, or an empty file on Cancel / Esc /
// click-outside dismiss, and when another browser opens in its place. Open with
// no existing file picked (files mode) is a Cancel too. A file Save named after a
// folder opens that folder, and one with an empty name stays open.
void open (juce::Component& host,
            Options opts,
            std::function<void (juce::File)> onResult);

// Multi-selection variant. Same as open() but the browser allows the
// user to pick several files; result fires with the full set (empty
// Array on cancel). Used by audio / MIDI import flows.
void openMulti (juce::Component& host,
                  Options opts,
                  std::function<void (juce::Array<juce::File>)> onResult);

// True while a browser is the topmost modal.
bool browserOnTop();

// Closes the shared browser while the message loop still exists. Call once, as
// the app shuts down; browsers closed after it get no timers.
void closeForShutdown();

// The folder the topmost browser is listing, and whether it is still listing
// it; empty and false when the top modal is not a browser.
std::filesystem::path shownFolderForScenario();
bool shownFolderScanningForScenario();

// Browsers closed mid-scan that are still waiting for their scan to stop.
int retiredScansForScenario();

// Browsers not yet destroyed, including one a closed modal has still to delete.
int livePanelsForScenario();

// Holds every check of a folder a browser is about to move to, for up to three
// seconds, as macOS does while it asks the user about a protected folder.
void holdFolderChecksForScenario (bool held);
} // namespace duskstudio::filebrowser
