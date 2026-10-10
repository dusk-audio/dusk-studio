#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include "../session/Session.h"

namespace duskstudio
{
// In-window modal body that lets the user pick which track an import
// should land on. Built to be hosted inside an EmbeddedModal owned by
// MainComponent. Stays alive only for the duration of the picker
// interaction; tearing down the modal destructs this body.
//
// Smart sort: tracks whose mode matches the file's channel layout
// bubble to the top (empty ones first, occupied next); mismatched-mode
// tracks render greyed below them, empty ones first, and tag their row
// with a hint that picking them switches the track's mode with the import.
class ImportTargetPicker final : public juce::Component
{
public:
    struct FileSummary
    {
        juce::File   file;
        double       sampleRate    = 0.0;   // source SR (audio only)
        int          numChannels   = 1;     // 1 or 2 for audio
        std::int64_t  lengthSamples = 0;     // audio only
        int          numMidiNotes  = 0;     // MIDI only
        std::int64_t  lengthTicks   = 0;     // MIDI only
        bool         isMidi        = false;

        // The mode a track has to be in to take the file; any other is switched.
        Track::Mode trackMode() const noexcept
        {
            if (isMidi) return Track::Mode::Midi;
            return numChannels == 2 ? Track::Mode::Stereo : Track::Mode::Mono;
        }
    };

    // onCommit fires with the resolved (0-based) target track index and, once
    // the user has agreed to it, the mode the import switches the track to.
    // The picker leaves the track as it is, so the switch can join the import's
    // undo step. onCancel fires on Cancel / Esc / click-outside. Both close
    // the host modal.
    ImportTargetPicker (Session& session,
                         FileSummary summary,
                         std::int64_t timelineStartSamples,
                         double      sessionSampleRate,
                         float       sessionBpm,
                         int         beatsPerBar,
                         int         timeDisplayMode,
                         int         preferredTrackIndex,
                         std::function<void (int trackIndex, std::optional<Track::Mode> switchTo)> onCommit,
                         std::function<void()> onCancel);
    ~ImportTargetPicker() override;

    void paint (juce::Graphics&) override;
    void resized() override;

    // One line per row, top first: the track's number, what the row says it
    // holds, then "RECOMMENDED", the mode-switch hint, "selected" and "in view"
    // where they apply, "-" where they do not, tab-separated.
    std::vector<std::string> rowsForScenario() const;

    // Rebuilds the rows from the session as it is now, keeping the selected
    // track: an undo under the picker can change what any track holds.
    void refresh();

private:
    struct Row;
    void buildRows (int trackToSelect);
    void keepSelectedRowInView();
    void selectRow (int index);
    void commitSelection();

    Session& session;
    FileSummary summary;
    std::int64_t timelineStart;
    double  sessionSampleRate;
    float   sessionBpm;
    int     beatsPerBar;
    int     timeDisplayMode;
    int     preferredTrack;

    std::function<void (int, std::optional<Track::Mode>)> onCommit;
    std::function<void()>      onCancel;

    juce::Label headerTitle;
    juce::Label headerSubtitle;
    juce::Label headerPlaceAt;

    juce::Viewport listViewport;
    juce::Component listContainer;
    std::vector<std::unique_ptr<Row>> rows;
    int selectedRowIdx = -1;

    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton importButton { "Import" };
};
} // namespace duskstudio
