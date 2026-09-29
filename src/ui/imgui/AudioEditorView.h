#pragma once

#include "DuskPanelWindow.h"

#include <cstdint>
#include <functional>
#include <memory>
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
};

// The audio region editor: toolbar, bar ruler, the track's regions around the focused
// one as stacked per-channel waveforms, the edit cursor and playhead, and the status
// bar of region readouts.
//
// The editor addresses its region by index into the track's region list, which other
// surfaces edit while it is open, so every read goes through a bounds check and a
// stale index paints "region unavailable" rather than reaching past the list.
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

    virtual bool chaseEnabled() const = 0;

    // { pixelsPerSample, scrollSamples, editCursorSample }. Pixels are design pixels.
    virtual std::vector<double> viewForScenario() const = 0;

    // Where the waveform shows a timeline sample, in the window's own pixels, three
    // quarters of the way down the lanes. False before the first frame has laid out.
    virtual bool samplePointForScenario (std::int64_t timelineSample, ImVec2& point) const = 0;

    // { regionIndex, rangeActive, rangeStart, rangeEnd }.
    virtual std::vector<std::int64_t> selectionForScenario() const = 0;
};

std::unique_ptr<AudioEditorView> makeAudioEditorView (Session& session, AudioEngine& engine,
                                                      int trackIndex, int regionIndex,
                                                      AudioEditorHost host);
} // namespace imgui
} // namespace duskstudio
