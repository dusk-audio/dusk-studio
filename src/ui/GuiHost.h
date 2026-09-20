#pragma once

#include <filesystem>
#include <string>

// What a GUI scenario is allowed to touch. Everything a case would otherwise
// reach through the component tree, expressed in std types so the cases stay
// free of the GUI framework; MainComponent implements it in GuiScenarios.cpp,
// which is the only place that knows how any of this is realised.
namespace duskstudio::scenario
{
// One channel strip's insert.
class StripHandle
{
public:
    virtual ~StripHandle() = default;

    // Engine-side loads, so the audio thread is fenced the way the picker's are.
    virtual bool loadNativeClap (const std::filesystem::path& file,
                                 const std::string& pluginId,
                                 std::string& errorOut) = 0;
    virtual bool loadNativeLv2  (const std::filesystem::path& file,
                                 const std::string& pluginId,
                                 std::string& errorOut) = 0;
    virtual bool loadNativeVst3 (const std::filesystem::path& file,
                                 const std::string& pluginId,
                                 std::string& errorOut) = 0;
    virtual void unloadNativePlugins() = 0;

    // Repaint the insert button so the strip shows what the engine now holds.
    virtual void refreshInsertButton() = 0;

    // True once an editor is actually up. A plug-in whose editor cannot be
    // attached leaves the modal stack carrying the alert that says so.
    virtual bool openEditor() = 0;
    virtual void closeEditor() = 0;
    virtual bool hasOpenEditor() const = 0;

    // Linux CLAP only: the plug-in took the container and put no window in it.
    virtual bool pluginWindowMissing() const = 0;

    // The value behind the open editor, by control name. False when no editor
    // is up, or the plug-in has no such control.
    virtual bool readEditorControl (const std::string& name, double& valueOut) const = 0;

    // Click MUTE / SOLO as the mouse would; the click lands on a later tick.
    virtual void clickMute() = 0;
    virtual void clickSolo() = 0;
};

// One aux lane's plug-in slots.
class AuxLaneHandle
{
public:
    virtual ~AuxLaneHandle() = default;

    virtual bool loadNativeClap (int slot, const std::filesystem::path& file,
                                 const std::string& pluginId) = 0;
    virtual void unloadSlot (int slot) = 0;

    // The lane embeds a native editor from its slot row rather than from a
    // modal, so rebuilding the row is what tries the attach.
    virtual void rebuildSlots() = 0;
    virtual bool attachEditor (int slot) = 0;
};

class GuiHost
{
public:
    virtual ~GuiHost() = default;

    // The strip and aux components only exist while their stage is up.
    enum class Stage { Recording, Mixing, Aux, Mastering };
    virtual void switchToStage (Stage) = 0;
    virtual bool clickStage (Stage) = 0;

    virtual bool clickTimeFormat() = 0;
    virtual std::string clockText() const = 0;

    // Null when the index is out of range, or the stage that realises the
    // component is not the one currently shown.
    virtual StripHandle*   strip   (int index) = 0;
    virtual AuxLaneHandle* auxLane (int index) = 0;

    // A strip's automation mode as the strip itself shows it: the mode label,
    // and whether its fader takes input (READ locks it). False when that strip
    // is not built.
    enum class StripKind { Channel, Bus, Master, Aux };
    virtual bool automationView (StripKind kind, int index,
                                 std::string& label, bool& faderEnabled) = 0;

    virtual bool pressPeerKey (const std::string& description, char text = 0) = 0;
    virtual bool clickModalAt (float xFraction, float yFraction) = 0;
    virtual bool clickFader (int index, bool readout, bool right = false) = 0;
    virtual bool faderEditing (int index) const = 0;
    virtual double faderValue (int index) const = 0;

    virtual bool openAudioSettings() = 0;
    virtual bool audioSettingsOpen() const = 0;
    virtual void closeAudioSettings() = 0;
    virtual bool clickAudioSettingsControl (const std::string& control) = 0;
    virtual bool midiBindingsOpen() const = 0;

    virtual bool meterClip (int index) = 0;
    virtual bool openMidiIo (int index) = 0;
    virtual bool clickMidiSelector (int index, int kind) = 0;
    virtual std::string midiSelectorText (int index, int kind) const = 0;

    virtual bool groupChipView (int index, std::string& text, int& master, bool& filled) = 0;

    virtual bool canEmbedPluginEditors() const = 0;
    virtual bool modalStackEmpty() const = 0;
    // Dismiss the newest modal - the alert a deliberately failing open raised.
    virtual void closeTopModal() = 0;

    // One tick of the autosave heartbeat, as its timer runs it.
    virtual void autosaveTick() = 0;
    virtual void startMixdown() = 0;
    virtual bool mixdownRunning() const = 0;
    virtual std::string statusMessage() const = 0;
    virtual void requestSessionSwitch (const std::filesystem::path& sessionJson) = 0;
    virtual bool clickModalButton (const std::string& label) = 0;
    // Opens a session the way File > Open does: a newer autosave beside it
    // raises the recovery prompt instead of loading.
    virtual bool openSession (const std::filesystem::path& sessionJson) = 0;
    // Answers the recovery prompt; false when none is up.
    enum class Recovery { Recover, LoadSaved, Cancel };
    virtual bool answerRecovery (Recovery) = 0;
};
} // namespace duskstudio::scenario
