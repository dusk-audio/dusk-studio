#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio::filebrowser
{
// Mode hint - same shape as juce::FileBrowserComponent's flag bitmask
// but enum'd so callers don't deal with OR'd constants. Save mode adds
// a name field at the bottom + warns about overwriting existing files.
enum class Mode { Open, Save };

struct Options
{
    juce::String title;                  // shown in the modal header
    juce::File   initialFileOrDirectory; // path to seed (file selected when Save)
    juce::String filePatternsAllowed;    // "*.wav;*.aiff" - empty = any
    Mode         mode = Mode::Open;
    bool         warnAboutOverwriting = true;  // Save mode only
    bool         selectDirectories     = false; // false = files only
};

// In-window Dusk-native file browser. Hosts juce::FileBrowserComponent
// inside an EmbeddedModal-hosted panel - no juce::FileChooser standalone
// window, no Wayland positioning / stacking issues, parented to the
// main app window so it inherits the DAW's modal grammar.
//
// `host` is any Component inside the main window (typically `this` from
// a caller's method); the modal is shown on the host's top-level.
// `onResult` fires once with the chosen file, or a default-constructed
// juce::File on Cancel / Esc / click-outside dismiss.
void open (juce::Component& host,
            Options opts,
            std::function<void (juce::File)> onResult);

// Multi-selection variant. Same as open() but the browser allows the
// user to pick several files; result fires with the full set (empty
// Array on cancel). Used by audio / MIDI import flows.
void openMulti (juce::Component& host,
                  Options opts,
                  std::function<void (juce::Array<juce::File>)> onResult);
} // namespace duskstudio::filebrowser
