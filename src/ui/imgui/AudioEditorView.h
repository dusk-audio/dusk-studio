#pragma once

#include "DuskPanelWindow.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio
{
class AudioEngine;
class Session;

namespace imgui
{
// What the audio editor asks of the shell it floats over.
struct AudioEditorHost
{
    // Reopen on another region of the same track. Called from inside a frame, so the
    // shell defers the close-and-reopen until the frame has returned.
    std::function<void (int track, int region)> navigateToRegion;
    // Undo, or redo when redo is true, as the shell's own keys run it: a track
    // move's undo is refused, with the reason, on the same grounds. The engine's
    // undo manager when unset.
    std::function<void (bool redo)> undo;
    // Save, Save As or Quit, as the shell's own keys run them. Called from inside a
    // frame, and Save As and the quit prompt each raise a modal that closes the
    // editor, so the shell defers it until the frame has returned. Unset, the editor
    // leaves these keys alone.
    enum class ShellCommand { save, saveAs, quit };
    std::function<void (ShellCommand)> shellCommand;
};

// The audio region editor: toolbar, bar ruler, the track's regions around the focused
// one as stacked per-channel waveforms, the edit cursor and playhead, the track's
// takes as lanes under the regions, and the status bar of region readouts.
//
// The editor addresses its region by index into the track's region list, which other
// surfaces edit while it is open, so every read goes through a bounds check and a
// stale index paints "region unavailable" rather than reaching past the list. A track
// with takes but no region to focus (index -1) shows its lanes over an empty region
// view, so a take can be promoted into it.
class AudioEditorView : public DuskPanelView
{
public:
    // The body the shell can give the editor, in design pixels: the main window less
    // the modal inset and the plate frame. preferredSize() reports it back.
    virtual void setAvailableSize (float width, float height) = 0;

    virtual int trackIndex() const = 0;
    virtual int regionIndex() const = 0;

    // Moves the edit focus to another region of the same track without re-fitting
    // the view.
    virtual void focusRegion (int regionIndex) = 0;

    // The editor's track now sits in another slot, a track move having moved it with
    // its regions and takes. Any drag or open field is dropped.
    virtual void followTrack (int trackIndex) = 0;

    // A recording on the editor's track is about to commit. A drag that edits
    // regions puts them back as it found them, as Escape does, so the take's undo
    // step holds them that way; the drag then waits out the release and records
    // nothing.
    virtual void yieldDragToRecordCommit() = 0;

    virtual bool chaseEnabled() const = 0;

    // A key the shell received while the editor is up, as JUCE's KeyPress describes it
    // ("ctrl + E", "delete"). The editor's child only has the keyboard once it takes
    // focus, which a Windows child never does, so the shell offers keys here first,
    // after DuskPanelWindow::offerShellKey has taken those typed into an open field
    // or menu. `repeat` marks a key held down, which JUCE delivers as another press;
    // the editor takes it only for a key that repeats at its child too. False when the
    // editor has no use for the key.
    virtual bool handleShellKey (const std::string& description, bool repeat) = 0;

    // { pixelsPerSample, scrollSamples, editCursorSample }. Pixels are design pixels.
    virtual std::vector<double> viewForScenario() const = 0;

    // { marks, sampleRate }: the bar numbers or time stamps the ruler drew in its last
    // frame, and the rate the editor reads time at.
    virtual std::vector<double> rulerForScenario() const = 0;

    // Where the waveform shows a timeline sample, in the window's own pixels, three
    // quarters of the way down the lanes. False before the first frame has laid out.
    virtual bool samplePointForScenario (std::int64_t timelineSample, ImVec2& point) const = 0;

    // Where a drag of `kind` starts for a timeline sample, in design pixels from the
    // body's top-left - the frame "at:<x>,<y>" addresses. "start" and "end" are the
    // trim handles, "gain" the gain line at that sample, "wave" the waveform point
    // samplePointForScenario gives, "stripe" the take stripe along the waveform's top
    // at that sample, above the fade discs, "divider" the take lanes' caption, and
    // "fadeIn" and "fadeOut" the focused region's fade discs wherever they sit.
    virtual bool gesturePointForScenario (const std::string& kind, std::int64_t timelineSample,
                                          ImVec2& point) const = 0;

    // Where the automation lane shows a normalised value at a timeline sample, in the
    // same body-relative design pixels as gesturePointForScenario.
    virtual bool automationPointForScenario (std::int64_t timelineSample, float value, ImVec2& point) const = 0;

    // { regionIndex, rangeActive, rangeStart, rangeEnd, additionalCount }.
    virtual std::vector<std::int64_t> selectionForScenario() const = 0;

    // Fits the whole track in view and scrolls the take lanes to the take's lane, the
    // newest take's when `take` is 0. Takes effect on the next frame.
    virtual void revealTakes (std::uint64_t take) = 0;

    // Take ids in lane order, newest first; empty when the editor shows no lanes.
    virtual std::vector<std::uint64_t> takeLanesForScenario() const = 0;

    // Where a take lane shows something, in the same body-relative design pixels as
    // gesturePointForScenario: "lane" is the lane's waveform at a timeline sample, and
    // "name", "audition", "delete", "confirm" and "cancel" the controls in its header.
    // False while the lane or the control is not on screen.
    virtual bool takePointForScenario (const std::string& kind, std::uint64_t take,
                                       std::int64_t timelineSample, ImVec2& point) const = 0;

    // { renamingTake, confirmingDeleteTake, draggedTake, dragStart, dragEnd }, 0 for
    // none; renamingTake once its name field has the keyboard, and the drag ends are
    // timeline samples, snapped as the drag snaps them.
    virtual std::vector<std::int64_t> takeStateForScenario() const = 0;

    // What the lane caption says about the last refused take edit; empty when nothing.
    virtual std::string takeNoticeForScenario() const = 0;

    // The whole lane caption beside the take count; empty when the editor shows no lanes.
    virtual std::string takeCaptionForScenario() const = 0;
};

std::unique_ptr<AudioEditorView> makeAudioEditorView (Session& session, AudioEngine& engine,
                                                      int trackIndex, int regionIndex,
                                                      AudioEditorHost host);
} // namespace imgui
} // namespace duskstudio
