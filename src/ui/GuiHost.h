#pragma once

#include <cstdint>
#include <filesystem>
#include <array>
#include <functional>
#include <optional>
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
    // A built-in unit's editor, which opens over the window rather than in the
    // insert's plug-in modal.
    virtual bool hasOpenBuiltinEditor() const = 0;

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
    // Every channel, bus and master GR meter beside its fader is on screen with its
    // threshold handle.
    virtual bool grMetersShown() const = 0;
    // The level text under a channel strip's send knob, as drawn.
    virtual std::string stripSendLabel (int track, int send) const = 0;

    virtual bool builtinPointer (int track, const std::string& control, float position, bool pressed) = 0;
    virtual void closeBuiltin (int track) = 0;
    // Escape at the window, sent through the display server so the message loop
    // has to reach the window system to read it. False where the platform has no
    // such route.
    virtual bool pressEscapeThroughDisplayServer() = 0;
    // A press on the dim behind a track's built-in editor, in the window's corner,
    // which the centred editor never covers. False when no editor is up.
    virtual bool clickBuiltinEditorDim (int track) = 0;
    virtual bool openAudioEditor (int track, int region) = 0;
    virtual bool clickAudioEditorButton (const std::string& name) = 0;
    // Both clicks in one call, so the pair is a double-click however slowly the editor draws.
    virtual bool doubleClickAudioEditorButton (const std::string& name) = 0;
    virtual bool clickAudioEditorSample (std::int64_t sample) = 0;
    virtual std::vector<double> audioEditorView() const = 0;
    // { marks, sampleRate }: the bar numbers or time stamps the editor's ruler drew in
    // its last frame, and the rate the editor reads time at. Empty with no editor up.
    virtual std::vector<double> audioEditorRuler() const = 0;
    virtual std::vector<int> audioEditorPoint (const std::string& kind, std::int64_t sample) const = 0;
    virtual bool audioEditorPointer (int x, int y, bool down, int modifiers = 0) = 0;
    virtual std::vector<std::int64_t> audioEditorSelection() const = 0;
    virtual std::vector<int> audioAutomationPoint (std::int64_t sample, float value) const = 0;
    // The audio editor on a track's take lanes, the way the timeline's take count opens it.
    virtual bool openAudioEditorOnTakes (int track) = 0;
    // Take ids in lane order, newest first; empty when the editor shows no lanes.
    virtual std::vector<std::uint64_t> audioEditorTakeLanes() const = 0;
    // In the frame audioEditorPointer takes: "lane" is a take's waveform at a timeline
    // sample; "name", "audition", "delete", "confirm" and "cancel" its header controls.
    // Empty while the lane or the control is off screen.
    virtual std::vector<int> audioEditorTakePoint (const std::string& kind, std::uint64_t take,
                                                   std::int64_t sample) const = 0;
    // Scrolls the lanes to the take's, the newest when take is 0; lands on the next frame.
    virtual bool revealAudioEditorTake (std::uint64_t take) = 0;
    // { renamingTake, confirmingDeleteTake, draggedTake, dragStart, dragEnd }.
    virtual std::vector<std::int64_t> audioEditorTakeState() const = 0;
    // The lane caption's explanation of a refused take edit; empty when none shows.
    virtual std::string audioEditorTakeNotice() const = 0;
    // Characters typed into the editor's open text field.
    virtual bool typeInAudioEditor (const std::string& text) = 0;
    // The whole take-lane caption; empty when the editor shows no lanes.
    virtual std::string audioEditorTakeCaption() const = 0;
    // Tells the editor's child it gained or lost the keyboard, as the platform does
    // when the focus moves. A Windows child never holds it, so every key reaches the
    // shell's window instead.
    virtual bool audioEditorKeyboardFocus (bool focused) = 0;
    // How often a field or menu opening in the editor has asked for the keyboard.
    virtual int audioEditorKeyboardRequests() const = 0;
    virtual bool openPiano (int track, int region) = 0;
    virtual void closePiano() = 0;
    virtual bool clickPianoCcToggle() = 0;
    virtual bool pianoCcPointer (std::int64_t tick, int value, bool down) = 0;
    virtual int pianoCcController() const = 0;
    virtual bool pianoNotePointer (std::int64_t tick, int pitch, bool down, int modifiers = 0) = 0;
    virtual std::vector<int> pianoSelection() const = 0;
    // The open piano roll's edit cursor, in ticks from the region's start; -1 when none is open.
    virtual std::int64_t pianoEditCursor() const = 0;
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
    // The directory the running executable sits in; empty if the platform would not say.
    virtual std::filesystem::path executableDirectory() const = 0;
    virtual void refreshMasteringSource() = 0;
    virtual bool focusFileName() = 0;
    // The folder the topmost file browser lists; empty when none is on top.
    virtual std::filesystem::path fileBrowserFolder() const = 0;
    // True while the topmost file browser is still listing its folder.
    virtual bool fileBrowserScanning() const = 0;
    // File browsers closed while listing a folder whose scan has not stopped yet.
    virtual int retiredFileBrowserScans() const = 0;
    // File browsers not yet destroyed; a closed one lingers until the next message-loop tick.
    virtual int fileBrowserPanels() const = 0;
    // Holds, or lets go of, the file browsers' checks of a folder they are about to show.
    virtual void holdFileBrowserFolderChecks (bool held) = 0;
    virtual bool clickFileBrowserControl (bool path) = 0;
    virtual bool clickFileBrowserUp() = 0;
    virtual std::vector<std::string> dpImportSummary() const = 0;
    virtual bool dropFilesOnTrack (int track, const std::vector<std::filesystem::path>& files) = 0;
    // Files held over the point dropFilesOnTrack drops on, then taken away:
    // the drop line's x and the pointer's, in the tape strip. The line is -1
    // when none shows; empty when the row is not on screen.
    virtual std::vector<int> tapeDropHover (int track, const std::vector<std::filesystem::path>& files) = 0;
    // The same files brought over that point and held there until tapeDropLeave:
    // the drop line's x and the pointer's, as tapeDropHover gives them.
    virtual std::vector<int> tapeDropHold (int track, const std::vector<std::filesystem::path>& files) = 0;
    // The drop line's x while files are held; -1 when none shows.
    virtual int tapeDropLine() const = 0;
    virtual void tapeDropLeave() = 0;
    // How many times the tape strip has painted; -1 without one.
    virtual int tapePaints() const = 0;
    // The sample under the point dropFilesOnTrack drops on.
    virtual std::int64_t tapeDropPointSample (int track) const = 0;
    virtual int tapeXForSample (std::int64_t sample) const = 0;
    // Stands in for the Alt key a drop reads: true lands at the pointer, false
    // at the playhead, nullopt reads the key again.
    virtual void forceDropAtMouse (std::optional<bool> atMouse) = 0;
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
    // When TAP last registered a tap, in the milliseconds of the app's own tap
    // clock; 0 before any tap, -1 without a transport bar.
    virtual std::int64_t lastTapMs() const = 0;

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
    // "print" (PRINT / FREEZE, shown outside MIXING) on a channel strip,
    // "mute", "fader" (its return fader) or "insert" (the slot's header
    // button) on an aux lane, "eq" (the EQ header's label) on a bus strip.
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
    // True when the top modal's body is what the window shows at the body's centre.
    virtual bool modalBodyOnTop() const = 0;
    // The open context menu's rows in order: separators as "-", headers by
    // their text.
    virtual std::vector<std::string> contextMenuItems() const = 0;

    // pressKey and pressPeerKey tap a key: the press, then its release.
    virtual bool pressPeerKey (const std::string& description, char text = 0) = 0;
    // A key held down at the window: the first call presses it, and each further call
    // before releasePeerKey is an auto-repeat, a press with no release between, as the
    // platform delivers one.
    virtual bool holdPeerKey (const std::string& description, char text = 0) = 0;
    virtual void releasePeerKey() = 0;
    // Leaves no component holding the keyboard, as Windows leaves the window once a
    // native child takes it, so keys at the window reach the window itself. Returns
    // what gives the keyboard back.
    virtual std::function<void()> unfocusWindow() = 0;
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
    // The main window's width and height, then the smallest width and height it
    // may be sized to, all in interface units before the UI scale. Empty
    // without a window.
    virtual std::vector<int> mainWindowSize() const = 0;
    // Sizes the main window, in the units mainWindowSize gives; the runner puts the
    // launch size back after the case.
    virtual bool resizeMainWindow (int width, int height) = 0;
    virtual int tapeExpansionState() const = 0;
    virtual int timelineChaseState() const = 0;
    virtual bool openRegionEditor (int track, int region, bool midi) = 0;
    virtual int regionEditorChase() const = 0;
    virtual void closeRegionEditors() = 0;
    virtual std::array<double, 4> pianoViewport() const = 0;
    virtual std::array<int, 4> pianoOptions() const = 0;
    virtual bool scrollPiano (float delta, bool command, bool shift, bool smooth) = 0;
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
    // Works one control of the open tape editor, by its parameter symbol,
    // through the callbacks a drag in the editor calls.
    virtual bool masterTapeEdit (const std::string& paramSymbol, float value) = 0;
    virtual void closeMasterTape() = 0;
    virtual bool clickInsert (int track, bool right = false) = 0;
    virtual std::vector<std::string> pickerRows (bool headers) const = 0;
    virtual bool clickPickerRow (const std::string& text) = 0;
    // Opens the insert picker on `rows` stand-in effects, long enough to scroll
    // however few plug-ins this machine has scanned.
    virtual bool openScrollingPicker (int rows) = 0;
    // The insert picker list's scroll offset and the most it can scroll; zeros
    // when no picker is up.
    virtual std::array<int, 2> pickerScroll() const = 0;
    // A raw wheel delta over the picker list, precise when smooth.
    virtual bool wheelPicker (float delta, bool smooth) = 0;
    virtual bool clickMasteringButton (const std::string& label) = 0;
    virtual bool clickMasteringTarget() = 0;
    // The multiband comp's preset picker; false when this build lays out none.
    virtual bool clickMasteringCompPreset() = 0;
    // How many of the mastering stage's two native panels are open, or -1 when
    // this build has none. Zero while the stage is not up.
    virtual int masteringPanelsOpen() const = 0;
    // Pixels around the multiband comp's editor that nothing painted, or -1
    // while the stage is not up.
    virtual int masteringCompPanelUnpaintedPixels() = 0;
    virtual std::string masteringTargetText() const = 0;
    // The Mastering page's source line, and the file its waveform was last
    // pointed at (empty once cleared). Both empty before the page is built.
    virtual std::string masteringSourceText() const = 0;
    virtual std::filesystem::path masteringWaveformFile() const = 0;
    virtual std::uint32_t masteringLoudnessColour (bool peak) const = 0;
    // What the integrated / true-peak cell shows; set on the same view tick
    // that colours it.
    virtual std::string masteringLoudnessText (bool peak) const = 0;
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
    virtual int modalCount() const = 0;
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
    // One pointer edge on a titled control, so a scenario can hold it down.
    virtual bool pressTitledControl (const std::string& title, bool down) = 0;
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
    // Modifiers as pianoNotePointer takes them: 1 Shift, 2 Cmd/Ctrl.
    virtual bool clickAudioRegion (int track, int region, bool right = false, int modifiers = 0) = 0;
    virtual bool clickMidiRegion (int track, int region, bool right = false) = 0;
    // Presses a region's middle and drags it `pixels` to the right.
    virtual bool dragTapeRegion (int track, int region, bool midi, int pixels) = 0;
    // The take-count badge in a track's label cell, as drawn ("3 takes"), and
    // one or more clicks on it. Empty text and false when that track shows no badge.
    virtual std::string tapeTakeBadgeText (int track) const = 0;
    virtual bool clickTapeTakeBadge (int track, int clicks = 1) = 0;
    // A marker pill in the tape ruler: clicked, or dragged to a ruler fraction.
    virtual bool clickTapeMarker (int index, bool right) = 0;
    virtual bool dragTapeMarker (int index, float toFraction) = 0;
    // A track's name in the tape strip's label column, clicked once or
    // double-clicked, with modifiers as pianoNotePointer takes them (1 Shift,
    // 2 Cmd/Ctrl, 4 right button). False when that row is not on screen.
    virtual bool clickTapeTrackName (int track, int clicks, int modifiers = 0) = 0;
    // The track whose row the open name editor sits on; -1 when none is open.
    virtual int tapeNameEditorTrack() const = 0;
    virtual int tapeSelectedTrack() const = 0;
    // Every track the tape strip lights as selected, ascending.
    virtual std::vector<int> tapeSelectedTracks() const = 0;
    // Where a track's row sits in the window, as a y; -1 when the row is not shown.
    virtual int tapeTrackRowY (int track) const = 0;
    // Presses a track's name and drags it to the gap before the gap-th shown
    // row (the row count is the gap after the last), releasing there unless
    // release is false. False when either point is not on screen.
    virtual bool dragTapeTrackName (int track, int gap, bool release = true) = 0;
    // Lets go of a name drag in the gap-th gap. With that gap off screen it
    // lets go where the drag went down, and returns false.
    virtual bool releaseTapeTrackName (int gap) = 0;
    // A click on a track's name whose pointer drifts `pixels` down while held.
    virtual bool nudgeTapeTrackName (int track, int pixels) = 0;
    // The window y of the line a name drag would drop on; -1 when none shows.
    virtual int tapeMoveLineY() const = 0;
    // The strip carrying the console's focus ring, -1 when none does.
    virtual int consoleFocusedStrip() const = 0;
    // What a channel strip's input selector shows; empty when it is not built.
    virtual std::string stripInputText (int track) const = 0;
    // The console strips whose insert is not the one the engine runs in their
    // slot, ascending.
    virtual std::vector<int> consoleStripsOffTheirSlot() const = 0;
    virtual bool audioEditorOpen() const = 0;
    virtual int audioEditorRegion() const = 0;
    // The track the open editor edits, -1 when none is open.
    virtual int audioEditorTrack() const = 0;
    virtual bool clickAudioEditorWaveform() = 0;
    virtual void closeAudioEditor() = 0;
    virtual bool pressAudioEditorKey (const std::string& description) = 0;
    virtual bool clickOutsideAudioEditor() = 0;
    // Dismiss the newest modal - the alert a deliberately failing open raised.
    virtual void closeTopModal() = 0;
    // An alert over the main window, the way the engine's own reports raise one.
    virtual void raiseAlert (const std::string& title, const std::string& message) = 0;

    // One tick of the autosave heartbeat, as its timer runs it.
    virtual void autosaveTick() = 0;
    virtual bool autosaveRunning() const = 0;
    // The latch a quit's Save sets while it holds the audio callback off.
    virtual bool engineDetached() const = 0;
    // Whether the session was opened from or saved to its folder, which is
    // what lets Save write in place rather than ask where.
    virtual bool sessionOnDisk() const = 0;
    // Closing the notepad after typing text into it, without the native editor.
    // False when the notes could not be saved.
    virtual bool closeNotepadAfterTyping (const std::string& text) = 0;
    // A fresh never-saved session in the folder launch would pick under parent.
    virtual void startUnsavedSessionIn (const std::filesystem::path& parent) = 0;
    // The titlebar X, which commits a take still recording before it decides.
    // False, with nothing asked, when there are no unsaved changes: that quit
    // would end the run.
    virtual bool requestQuit() = 0;
    virtual bool mixdownRunning() const = 0;
    // Whether any modal's render is still running: bounce, mixdown, master export or freeze.
    virtual bool renderRunning() const = 0;
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

// Whether every native panel has drawn the pointer, key and text input the host
// queued on it. A panel takes that input a frame at a time, so a case that reads
// the result straight after sending it is reading a race with the renderer.
bool panelInputSeen();
} // namespace duskstudio::scenario
