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
// track dropdown the user must pick. No auto-assignment - the user is
// always explicit. Same-track collisions are allowed: the dispatch path
// stacks both files at the shared timelineStart in selection order.
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
    juce::TextButton importButton { "Import" };
};
} // namespace duskstudio
