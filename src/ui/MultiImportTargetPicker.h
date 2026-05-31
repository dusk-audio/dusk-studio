#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>
#include <memory>
#include <vector>
#include "ImportTargetPicker.h"
#include "../session/Session.h"

namespace duskstudio
{
// Single-modal multi-file import picker. One row per file, each with a
// track dropdown. "Auto-assign" fills the rows sequentially (file 1 -> track
// 1, file 2 -> track 2, ...) for the common stems-in-order case; the user can
// still override any row by hand. Track picks are unique: rebuildPicker()
// removes a track already chosen by another row from the remaining dropdowns,
// and auto-assign hands out distinct tracks, so two files can't land on the
// same track.
//
// Lives inside an EmbeddedModal owned by MainComponent. Reuses
// ImportTargetPicker::FileSummary so the file-peek code stays in one
// place (MainComponent).
class MultiImportTargetPicker final : public juce::Component
{
public:
    struct Assignment
    {
        juce::File file;
        int        trackIndex = -1;
        bool       isMidi     = false;
    };

    MultiImportTargetPicker (Session& session,
                              std::vector<ImportTargetPicker::FileSummary> summaries,
                              juce::int64 timelineStartSamples,
                              std::function<void (std::vector<Assignment>)> onCommit,
                              std::function<void()> onCancel);
    ~MultiImportTargetPicker() override;

    void paint (juce::Graphics&) override;
    void resized() override;

private:
    struct Row;

    void rebuildImportEnabled();

    // Toggle. First click assigns each file to a track sequentially (file i ->
    // track i); files beyond the track count stay unassigned. Second click
    // clears every row back to "Choose track...". Refreshes the dropdowns +
    // Import-enabled state + the button label either way.
    void toggleAutoAssign();
    bool autoAssignActive = false;

    // Re-populate every row's track-picker combo so a track picked by
    // one row is hidden from the other rows' dropdowns. Called whenever
    // any row's selection changes. Each combo retains its own current
    // pick - only other rows' picks are filtered out.
    void rebuildAvailableTracks();

    std::vector<Assignment> collectAssignments() const;

    Session& session;
    std::vector<ImportTargetPicker::FileSummary> summaries;
    juce::int64 timelineStart;

    std::function<void (std::vector<Assignment>)> onCommit;
    std::function<void()>                          onCancel;

    juce::Label headerTitle;
    juce::Label headerSubtitle;

    juce::Viewport listViewport;
    juce::Component listContainer;
    std::vector<std::unique_ptr<Row>> rows;

    juce::TextButton cancelButton { "Cancel" };
    juce::TextButton autoAssignButton { "Auto-assign" };
    juce::TextButton importButton { "Import" };
};
} // namespace duskstudio
