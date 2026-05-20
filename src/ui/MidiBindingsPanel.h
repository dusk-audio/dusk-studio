#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <functional>

namespace duskstudio
{
class Session;
class AudioEngine;

// In-window panel listing every active MIDI binding with a per-row
// Remove button + a 'Clear all' action. Read-only-edit (no inline
// reassign of target / channel / CC) by design - re-learn via right-
// click on the target is the right edit path. Acts as the audit + bulk-
// cleanup surface for bindings created across the strips / transport /
// plugin slots.
//
// Hosted inside an EmbeddedModal owned by the calling AudioSettings
// panel. Selection mutations go through Session::midiBindings.mutate
// so the audio thread sees the new set on the next block.
class MidiBindingsPanel final : public juce::Component
{
public:
    MidiBindingsPanel (Session& session,
                        AudioEngine& engine,
                        std::function<void()> onDone);
    ~MidiBindingsPanel() override = default;

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kPanelW = 620;
    static constexpr int kPanelH = 480;

private:
    void rebuildRows();
    void removeBindingAt (int displayIndex);
    void clearAll();

    Session& session;
    AudioEngine& engine;
    std::function<void()> onDoneCallback;

    juce::Label headerLabel;
    juce::Label emptyHint;

    // Per-row Component: target label + source label + Remove button.
    // Rebuilt on every binding change via rebuildRows so indices stay
    // stable across removes.
    struct Row final : public juce::Component
    {
        juce::Label targetLabel;
        juce::Label sourceLabel;
        juce::TextButton removeButton { "Remove" };

        Row();
        void resized() override;
    };
    juce::Viewport rowsViewport;
    juce::Component rowsContainer;
    std::vector<std::unique_ptr<Row>> rows;

    juce::TextButton exportButton   { "Export..." };
    juce::TextButton importButton   { "Import..." };
    juce::TextButton clearAllButton { "Clear all" };
    juce::TextButton doneButton     { "Done" };
    // Async file choosers kept alive across the OS dialog's lifetime.
    // Two slots because the user CAN trigger both flows back-to-back
    // (open Export, then click Import before the Export dialog closes);
    // a single slot would let the second click reassign the unique_ptr
    // and destroy the first FileChooser mid-callback, which JUCE forbids.
    std::unique_ptr<juce::FileChooser> exportChooser;
    std::unique_ptr<juce::FileChooser> importChooser;

    void exportPreset();
    void importPreset();
};
} // namespace duskstudio
