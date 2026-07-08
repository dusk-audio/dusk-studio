#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
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
// tracks render greyed at the bottom and tag their row with a hint
// that picking them will flip the track mode atomically before the
// import runs.
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
    };

    // onCommit fires with the resolved (0-based) target track index AFTER
    // the picker has flipped the track mode if needed. onCancel fires on
    // Cancel / Esc / click-outside. Both close the host modal.
    ImportTargetPicker (Session& session,
                         FileSummary summary,
                         std::int64_t timelineStartSamples,
                         double      sessionSampleRate,
                         float       sessionBpm,
                         int         beatsPerBar,
                         int         timeDisplayMode,
                         int         preferredTrackIndex,
                         std::function<void (int trackIndex)> onCommit,
                         std::function<void()> onCancel);
    ~ImportTargetPicker() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row;
    void selectRow (int index);
    void commitSelection();

    Session& session;
    FileSummary summary;
    std::int64_t timelineStart;
    double  sessionSampleRate;
    float   sessionBpm;
    int     beatsPerBar;
    int     timeDisplayMode;

    std::function<void (int)>  onCommit;
    std::function<void()>      onCancel;

    juce::Label headerTitle;
    juce::Label headerSubtitle;
    juce::Label headerPlaceAt;

    juce::Viewport listViewport;
    juce::Component listContainer;
    std::vector<std::unique_ptr<Row>> rows;
    int selectedRowIdx = -1;
    int recommendedRowIdx = -1;

    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton importButton { "Import" };
};
} // namespace duskstudio
