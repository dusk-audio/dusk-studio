#pragma once

#include <cstdint>
#include <filesystem>
#include <array>
#include <functional>
#include <string>
#include <vector>

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
    virtual bool clickArm() = 0;
    virtual bool armLit() const = 0;
    virtual bool midiActivityVisible() const = 0;
    virtual bool midiActivityLit() const = 0;
    virtual bool inputSettingsOpen() const = 0;
    virtual bool openInputSettings (int mode) = 0;
    virtual void loadBuiltin (const std::string& id) = 0;
    virtual void clickMonitor() = 0;
    virtual void clickAutomationMode() = 0;
    virtual void restoreTrackMode (int mode) = 0;
    virtual bool instrumentControlsMatch (int input, bool monitor) const = 0;
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
    virtual bool captureSources (bool enabled) = 0;
    virtual std::vector<std::string> sourceRows() const = 0;
};

class GuiHost
{
public:
    virtual ~GuiHost() = default;

    virtual const std::vector<std::string>& firstLaunchErrors() const = 0;

    enum class Stage { Recording, Mixing, Aux, Mastering };
    virtual void switchToStage (Stage) = 0;
    virtual bool clickStage (Stage) = 0;
    virtual bool stageViewMatches (Stage) const = 0;
    virtual bool stripStageControlsMatch (int index, bool mixing) const = 0;
    virtual bool pressKey (const std::string& description, char text = 0) = 0;
    virtual std::function<void()> preserveKeyboardFocus() = 0;
    virtual int consolePageCount() const = 0;
    virtual bool consolePageMatches (int index) const = 0;
    virtual bool timelineViewMatches (bool expanded) const = 0;
    virtual bool stripCompact (int index) const = 0;

    virtual bool clickTimeFormat() = 0;
    virtual bool clickRecord() = 0;
    virtual bool doubleClickTempo() = 0;
    virtual bool rightClickPunch() = 0;
    virtual bool focusModalTextInput() = 0;
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
    virtual bool clickContextMenuItem (const std::string& text) = 0;
    virtual bool clickFader (int index, bool readout, bool right = false) = 0;
    virtual bool faderEditing (int index) const = 0;
    virtual double faderValue (int index) const = 0;

    virtual bool openAudioSettings() = 0;
    virtual bool audioSettingsOpen() const = 0;
    virtual void closeAudioSettings() = 0;
    virtual bool clickAudioSettingsControl (const std::string& control) = 0;
    virtual bool inputAudioSettings (const std::string& input) = 0;
    virtual bool pointerAudioSettings (const std::string& control, float position, bool pressed) = 0;
    virtual double uiScale() const = 0;
    virtual void restoreUiScale (float scale) = 0;
    virtual int tapeExpansionState() const = 0;
    virtual int timelineChaseState() const = 0;
    virtual bool openRegionEditor (int track, int region, bool midi) = 0;
    virtual int regionEditorChase() const = 0;
    virtual void closeRegionEditors() = 0;
    virtual std::array<double, 4> pianoViewport() const = 0;
    virtual std::array<int, 4> pianoOptions() const = 0;
    virtual bool scrollPiano (float delta, bool command, bool shift) = 0;
    virtual bool clickPianoFit() = 0;
    virtual bool focusPiano() = 0;
    virtual bool setStripCompact (int track, bool compact) = 0;
    virtual bool clickStripModule (int track, int module, bool label, bool right) = 0;
    virtual bool stripModuleEditorOpen (int track, int module) const = 0;
    virtual void closeStripModuleEditors (int track) = 0;
    virtual bool clickInsert (int track, bool right = false) = 0;
    virtual std::vector<std::string> pickerRows (bool headers) const = 0;
    virtual bool clickPickerRow (const std::string& text) = 0;
    virtual bool focusFileName() = 0;
    virtual bool clickMasteringButton (const std::string& label) = 0;
    virtual void refreshMasteringSource() = 0;
    virtual bool clickMasteringTarget() = 0;
    virtual std::string masteringTargetText() const = 0;
    virtual std::uint32_t masteringLoudnessColour (bool peak) const = 0;
    virtual void restoreMasteringTarget (int index) = 0;
    virtual bool midiBindingsOpen() const = 0;
    virtual bool virtualKeyboardOpen() const = 0;
    virtual bool inputVirtualKeyboard (const std::string& key) = 0;
    virtual void closeVirtualKeyboard() = 0;

    virtual bool meterClip (int index) = 0;
    virtual bool openMidiIo (int index) = 0;
    virtual bool clickMidiSelector (int index, int kind) = 0;
    virtual std::string midiSelectorText (int index, int kind) const = 0;

    virtual bool groupChipView (int index, std::string& text, int& master, bool& filled) = 0;

    virtual bool canEmbedPluginEditors() const = 0;
    virtual bool modalStackEmpty() const = 0;
    virtual std::string modalText() const = 0;
    virtual bool clickModalButton (const std::string& label) = 0;
    virtual void openAbout() = 0;
    virtual bool shortcutsOpen() const = 0;
    virtual void startMixdown() = 0;
    virtual bool fullScreen() const = 0;
    virtual int activeAuxLane() const = 0;
    virtual bool clickAuxSelector (int index) = 0;
    virtual bool auxLaneLayoutMatches (int index) const = 0;
    virtual bool accessibleControl (const std::string& title, std::string& value, std::string& help) = 0;
    virtual bool setAccessibleValue (const std::string& title, const std::string& value) = 0;
    virtual bool loadMasteringFile (const std::filesystem::path& path) = 0;
    virtual bool clickMasteringWaveform (float fraction) = 0;
    virtual void openPianoRoll (int track, int region) = 0;
    virtual bool doubleClickMidiRegion (int track, int region) = 0;
    virtual int pianoRollRegion() const = 0;
    virtual bool pianoRollOpen() const = 0;
    virtual bool clickPianoGrid (std::int64_t tick, int pitch) = 0;
    virtual bool dragPianoVelocity (std::int64_t tick, float fraction) = 0;
    virtual bool resizePianoVelocity (int pixels) = 0;
    virtual bool wheelPianoVelocity (float delta) = 0;
    virtual int pianoVelocityHeight() const = 0;
    virtual bool togglePianoCc() = 0;
    virtual bool dragPianoCc (std::int64_t tick, float fraction) = 0;
    virtual bool resizePianoCc (int pixels) = 0;
    virtual int pianoCcHeight() const = 0;
    virtual void closePianoRoll() = 0;
    virtual bool pressPianoRollKey (const std::string& description) = 0;
    virtual bool doubleClickAudioRegion (int track, int region) = 0;
    virtual bool clickAudioRegion (int track, int region) = 0;
    virtual bool audioEditorOpen() const = 0;
    virtual int audioEditorRegion() const = 0;
    virtual bool clickAudioEditorWaveform() = 0;
    virtual void closeAudioEditor() = 0;
    virtual bool pressAudioEditorKey (const std::string& description) = 0;
    virtual bool clickOutsideAudioEditor() = 0;
    // Dismiss the newest modal - the alert a deliberately failing open raised.
    virtual void closeTopModal() = 0;

    // One tick of the autosave heartbeat, as its timer runs it.
    virtual void autosaveTick() = 0;
    virtual bool mixdownRunning() const = 0;
    virtual std::string statusMessage() const = 0;
    virtual void requestSessionSwitch (const std::filesystem::path& sessionJson) = 0;
    // Opens a session the way File > Open does: a newer autosave beside it
    // raises the recovery prompt instead of loading.
    virtual bool openSession (const std::filesystem::path& sessionJson) = 0;
    // Answers the recovery prompt; false when none is up.
    enum class Recovery { Recover, LoadSaved, Cancel };
    virtual bool answerRecovery (Recovery) = 0;
};
} // namespace duskstudio::scenario
