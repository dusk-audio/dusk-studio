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
struct MiniMarkerPaint
{
    std::string name;
    std::uint32_t tickColour = 0, labelColour = 0;
    float tickHeight = 0.0f, tickWidth = 0.0f;
    int labelX = 0, labelY = 0, labelWidth = 0;
};

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

    // The insert button's text as the strip last painted it, health labels
    // included.
    virtual std::string insertLabel() const = 0;

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

    // A built-in unit through the lane's own load path, and the unit id the
    // slot's inline editor is currently built for (empty when none is up).
    virtual bool loadBuiltin (int slot, const std::string& unitId) = 0;
    virtual std::string builtinEditorUnit (int slot) const = 0;

    // The lane embeds a native editor from its slot row rather than from a
    // modal, so rebuilding the row is what tries the attach.
    virtual void rebuildSlots() = 0;
    virtual bool attachEditor (int slot) = 0;
    // Repaint one slot row, and the text its header button then carries.
    virtual void refreshSlot (int slot) = 0;
    virtual std::string slotLabel (int slot) const = 0;
    virtual bool captureSources (bool enabled) = 0;
    virtual std::vector<std::string> sourceRows() const = 0;
};

class GuiHost
{
public:
    virtual ~GuiHost() = default;

    virtual const std::vector<std::string>& firstLaunchErrors() const = 0;

    // The strip and aux components only exist while their stage is up.
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
    // The level text under a channel strip's send knob, as drawn.
    virtual std::string stripSendLabel (int track, int send) const = 0;

    virtual bool builtinPointer (int track, const std::string& control, float position, bool pressed) = 0;
    virtual void closeBuiltin (int track) = 0;
    virtual bool openAudioEditor (int track, int region) = 0;
    virtual bool clickAudioEditorButton (const std::string& name) = 0;
    virtual bool clickAudioEditorSample (std::int64_t sample) = 0;
    virtual std::vector<double> audioEditorView() const = 0;
    virtual std::vector<int> audioEditorPoint (const std::string& kind, std::int64_t sample) const = 0;
    virtual bool audioEditorPointer (int x, int y, bool down, int modifiers = 0) = 0;
    virtual std::vector<std::int64_t> audioEditorSelection() const = 0;
    virtual std::vector<int> audioAutomationPoint (std::int64_t sample, float value) const = 0;
    virtual bool openPiano (int track, int region) = 0;
    virtual void closePiano() = 0;
    virtual bool clickPianoCcToggle() = 0;
    virtual bool pianoCcPointer (std::int64_t tick, int value, bool down) = 0;
    virtual int pianoCcController() const = 0;
    virtual bool pianoNotePointer (std::int64_t tick, int pitch, bool down, int modifiers = 0) = 0;
    virtual std::vector<int> pianoSelection() const = 0;
    virtual bool setTimelineShown (bool shown) = 0;
    virtual std::vector<double> tapeView() const = 0;
    virtual void restoreTapeView (const std::vector<double>& view) = 0;
    virtual bool tapeWheel (float fraction, float delta, bool command, bool shift) = 0;
    virtual bool tapeRulerPointer (float fraction, bool down, bool shift = false) = 0;
    virtual std::int64_t tapeRulerSample (float fraction) const = 0;
    virtual bool clickContextMenuItem (const std::string& text) = 0;
    virtual bool clickFileMenu() = 0;
    // A menu bar title by name: "File", "View" or "Settings".
    virtual bool clickMenuBar (const std::string& name) = 0;
    // True when the open menu has that row and it can be chosen; clicks nothing.
    virtual bool contextMenuItemEnabled (const std::string& text) const = 0;
    // What Settings > Quickstart would hand to the desktop; empty when absent.
    virtual std::filesystem::path quickstartDocument() const = 0;
    virtual void refreshMasteringSource() = 0;
    virtual bool focusFileName() = 0;
    virtual bool clickFileBrowserControl (bool path) = 0;
    virtual std::vector<std::string> dpImportSummary() const = 0;
    virtual bool dropFilesOnTrack (int track, const std::vector<std::filesystem::path>& files) = 0;
    virtual std::vector<std::string> confirmationText() const = 0;
    virtual std::vector<std::string> multiImportRows() const = 0;
    virtual bool clickMultiImportTarget (int row) = 0;
    virtual bool captureMiniMarkers (bool enabled) = 0;
    virtual std::vector<MiniMarkerPaint> miniMarkerPaint() const = 0;
    virtual bool clickMiniSample (std::int64_t sample) = 0;
    virtual bool clickMiniMarker (int index) = 0;
    virtual bool clickTimeFormat() = 0;
    // The menu bar's DSP segment, and a double-click on it.
    virtual std::string dspReadout() const = 0;
    virtual bool doubleClickDspReadout() = 0;
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
    // A named control on a strip: "name" on a channel strip or an aux lane,
    // "mute" or "fader" (its return fader) on an aux lane, "eq" (the EQ
    // header's label) on a bus strip.
    virtual bool clickStripControl (StripKind kind, int index, const std::string& control,
                                    int clicks, bool right) = 0;
    virtual std::vector<double> auxReturnRange (int lane) const = 0;
    virtual std::uint32_t tapeRegionColour (int track, int region) const = 0;
    // True when the tape strip is up and lays that audio region out on its
    // track's row.
    virtual bool tapeRegionShown (int track, int region) const = 0;
    virtual std::vector<std::string> midiBindingRows() const = 0;
    virtual bool clickMidiBindingRemove (int row) = 0;
    // The top modal's body and the window it sits in, as x, y, width, height
    // and width, height, then 1 when the window behind it is dimmed.
    virtual std::vector<int> modalLayout() const = 0;
    virtual bool clickModalBackdrop() = 0;
    virtual bool modalHasKeyboardFocus() const = 0;
    // The open context menu's rows in order: separators as "-", headers by
    // their text.
    virtual std::vector<std::string> contextMenuItems() const = 0;

    virtual bool pressPeerKey (const std::string& description, char text = 0) = 0;
    virtual bool clickModalAt (float xFraction, float yFraction) = 0;
    virtual bool clickFader (int index, bool readout, bool right = false) = 0;
    virtual bool faderEditing (int index) const = 0;
    virtual double faderValue (int index) const = 0;

    virtual bool openAudioSettings() = 0;
    virtual bool audioSettingsOpen() const = 0;
    virtual void closeAudioSettings() = 0;
    virtual bool clickAudioSettingsControl (const std::string& control) = 0;
    virtual bool inputAudioSettings (const std::string& input) = 0;
    virtual bool pointerAudioSettings (const std::string& control, float position, bool pressed) = 0;
    // The launch dialog over the running window, listing `sessions` as its
    // recents. With `runChoice` false the choice it is dismissed with is only
    // recorded, for startupChoice(): pressing Quit would otherwise end the run.
    virtual bool openStartupDialog (const std::vector<std::filesystem::path>& sessions,
                                    bool runChoice) = 0;
    virtual bool startupDialogOpen() const = 0;
    virtual void closeStartupDialog() = 0;
    // "row:N", "template:N", "tab-recent", "tab-open", "tab-new", "open" or
    // "quit", clicked where the dialog last drew it; "scroll-down" wheels the
    // list. False when the control is not on screen.
    virtual bool clickStartupControl (const std::string& control) = 0;
    // Wheels the list by `wheel` notches, which may be a trackpad's fraction.
    virtual bool scrollStartupList (float wheel) = 0;
    virtual int startupSelectedRow() const = 0;
    // "recent:<path>", "new:<template>", "open-file", "quit" or "skip"; empty
    // until the dialog has been dismissed.
    virtual std::string startupChoice() const = 0;
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
    // The strip name the open EQ editor is titled with.
    virtual std::string stripModuleEditorTitle (int track, int module) const = 0;
    virtual void closeStripModuleEditors (int track) = 0;
    // The master strip's TAPE split button: its label opens Tape Machine 2's own
    // editor, its status light engages the stage.
    virtual bool clickMasterTape (bool label) = 0;
    virtual bool masterTapeEditorOpen() const = 0;
    virtual bool masterTapeEditorDrawn() const = 0;
    virtual void closeMasterTape() = 0;
    virtual bool clickInsert (int track, bool right = false) = 0;
    virtual std::vector<std::string> pickerRows (bool headers) const = 0;
    virtual bool clickPickerRow (const std::string& text) = 0;
    virtual bool clickMasteringButton (const std::string& label) = 0;
    virtual bool clickMasteringTarget() = 0;
    // The multiband comp's preset picker; false when this build lays out none.
    virtual bool clickMasteringCompPreset() = 0;
    // How many of the mastering stage's two native panels are open, or -1 when
    // this build has none. Zero while the stage is not up.
    virtual int masteringPanelsOpen() const = 0;
    virtual std::string masteringTargetText() const = 0;
    virtual std::uint32_t masteringLoudnessColour (bool peak) const = 0;
    virtual void restoreMasteringTarget (int index) = 0;
    virtual bool midiBindingsOpen() const = 0;
    virtual bool virtualKeyboardOpen() const = 0;
    virtual bool tunerOpen() const = 0;
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
    // The text a titled slider's value box shows, as drawn: "<missing>" when no
    // control carries the title, "<no value box>" when it has none.
    virtual std::string valueBoxText (const std::string& title) = 0;
    virtual bool clickTitledControl (const std::string& title, bool right) = 0;
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
    virtual bool clickAudioRegion (int track, int region, bool right = false) = 0;
    virtual bool clickTakeBadge (int track, int region) = 0;
    // A marker pill in the tape ruler: clicked, or dragged to a ruler fraction.
    virtual bool clickTapeMarker (int index, bool right) = 0;
    virtual bool dragTapeMarker (int index, float toFraction) = 0;
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
    virtual bool autosaveRunning() const = 0;
    // The latch a quit's Save sets while it holds the audio callback off.
    virtual bool engineDetached() const = 0;
    // The titlebar X. False, with nothing asked, when there are no unsaved
    // changes: that quit would end the run.
    virtual bool requestQuit() = 0;
    virtual bool mixdownRunning() const = 0;
    virtual std::string statusMessage() const = 0;
    virtual void requestSessionSwitch (const std::filesystem::path& sessionJson) = 0;
    // Opens a session the way File > Open does: a newer autosave beside it
    // raises the recovery prompt instead of loading.
    virtual bool openSession (const std::filesystem::path& sessionJson) = 0;
    // Answers the recovery prompt; false when none is up.
    enum class Recovery { Recover, LoadSaved, Cancel };
    virtual bool answerRecovery (Recovery) = 0;

    // Every way the window now differs from how it launched, one readable line
    // each. Empty means clean. The suite runner reads this between scenarios,
    // so a case that leaves the window changed is named by the run rather than
    // breaking whichever case happens to follow it.
    virtual std::vector<std::string> launchStateDiff() const = 0;

    // Put the window back the way it launched: modals down, editors closed,
    // transport stopped, stage / scale / timeline / console back to their
    // launch values. What a scenario owns - its regions, its loaded plug-ins -
    // is left alone; launchStateDiff() names those instead.
    virtual void resetForScenario() = 0;
};
} // namespace duskstudio::scenario
