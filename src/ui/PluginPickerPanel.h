#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <juce_audio_processors/juce_audio_processors.h>
#include <functional>

namespace duskstudio
{
// Dusk-native plugin picker. Replaces juce::PopupMenu for the insert /
// AUX slot pickers — pure in-window Component, no native popup peer,
// no JUCE PopupMenu dismiss heuristics. Shown via EmbeddedModal so it
// inherits the same Esc / click-outside dismiss the rest of Dusk uses.
//
// Two-column layout:
//   • Title row.
//   • Scrollable plugin list grouped by manufacturer.
//   • Bottom action row: Hardware Insert / Load Soundfont / Browse /
//     Scan / Cancel.
//
// Callbacks let the host wire each action without the panel knowing
// about PluginManager, FileChooser, or session state.
class PluginPickerPanel final : public juce::Component
{
public:
    enum class Kind { Effects, Instruments };

    struct Callbacks
    {
        // Fired on row click. Description is owned by the panel; copy if
        // you need it past the callback's stack frame.
        std::function<void (const juce::PluginDescription&)> onPickPlugin;
        std::function<void()> onScan;
        std::function<void()> onBrowseFile;
        std::function<void()> onHardwareInsert;   // null → omit button
        std::function<void()> onLoadSoundfont;    // null → omit button
        std::function<void()> onCancel;
    };

    PluginPickerPanel (juce::Array<juce::PluginDescription> descriptions,
                       Kind kind,
                       Callbacks cb);
    ~PluginPickerPanel() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;

private:
    class ListBody;

    Kind kind_;
    Callbacks callbacks_;

    juce::Label titleLabel;
    juce::TextEditor filterEditor;
    juce::TextButton cancelBtn      { "Close" };
    juce::TextButton scanBtn        { "Scan plugins" };
    juce::TextButton browseBtn      { "Browse file..." };
    juce::TextButton hwInsertBtn    { "Hardware Insert" };
    juce::TextButton soundfontBtn   { "Load Soundfont" };

    std::unique_ptr<ListBody> listBody;

    static constexpr int kPad      = 10;
    static constexpr int kTitleH   = 28;
    static constexpr int kFilterH  = 28;
    static constexpr int kActionsH = 32;
};
} // namespace duskstudio
