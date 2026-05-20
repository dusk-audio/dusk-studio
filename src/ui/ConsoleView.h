#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include <functional>
#include <memory>
#include "BusComponent.h"
#include "ChannelStripComponent.h"
#include "MasterStripComponent.h"
#include "../session/Session.h"
#include "../engine/AudioEngine.h"

namespace duskstudio
{
class ConsoleView final : public juce::Component
{
public:
    // Takes both Session (data model) and AudioEngine (live DSP). The
    // engine is needed so each ChannelStripComponent can be handed a
    // reference to its PluginSlot - the UI calls slot.loadFromFile etc.
    // on the message thread.
    ConsoleView (Session& session, AudioEngine& engine);

    void paint (juce::Graphics&) override;
    void resized() override;

    static constexpr int kMinChannelWidth = 120;
    static constexpr int kMinBusWidth     = 130;   // 3 × 40 px knob blocks + 10 px padding
    static constexpr int kMinMasterWidth  = 210;   // 5 × 40 px knob blocks + 10 px padding

    static constexpr int kRefChannelWidth = 150;
    static constexpr int kRefBusWidth     = 150;   // comfortable padding around the 3-knob row
    static constexpr int kRefMasterWidth  = 230;   // 5 EQ knobs + 5 comp knobs at 40 px each

    // Auto-engage SUMMARY (compact mode) so the EQ/COMP sections collapse
    // into popup-launchers and the fader keeps its full vertical span when
    // the window gets cramped. Two independent triggers:
    //   - Height: EQ + COMP eat ~230 px of fixed vertical space inside the
    //     strip; below this strip height the fader gets squeezed too far.
    //   - Width:  only fires when the layout has already pushed strips
    //     well below the kMinChannelWidth floor (the secondary scaling
    //     pass). At kMin the knobs are still readable, so we don't want to
    //     auto-compact just because we hit the floor.
    static constexpr int kAutoCompactStripHeight = 820;
    static constexpr int kAutoCompactChannelWidth = 110;

    static constexpr int kStripGap     = 4;
    static constexpr int kSectionGap   = 12;

    static int minimumContentWidth();

    // Width threshold above which we drop banking and show all 16 strips.
    // Public so MainComponent (which now owns the BANK A/B row) can decide
    // whether to render the bank-row above the transport.
    int fixedWidthFor16Tracks() const;

    void setBank (int bankIndex);
    int  getBank() const noexcept { return currentBank; }

    // Force-close every per-strip plugin editor window before app
    // shutdown. Called from MainComponent::requestQuit's Save / Don't
    // Save handlers BEFORE systemRequestedQuit() so the editor windows
    // (real top-level juce::DocumentWindows on Linux) die in a quiet
    // window rather than racing Mutter's own teardown of our main
    // window. Safe to call when no editors are open.
    void dropAllPluginEditors();

private:
    Session& sessionRef;

    std::array<std::unique_ptr<ChannelStripComponent>, Session::kNumTracks> strips;

public:
    // Forwarded by MainComponent when the SUMMARY view toggles. Each track
    // strip collapses its EQ + COMP into popup-launch buttons so the fader,
    // bus assigns, and meters stay visible while the tape strip is up.
    // The user's intent is OR'd with auto-compact (engaged when the window
    // is too narrow) — see applyCompactState.
    void setStripsCompactMode (bool compact);

    // Forwarded by MainComponent when the stage selector changes. In Mixing
    // each strip swaps its input/IN/ARM/PRINT block for a row of 4 AUX send
    // knobs (the tracking controls only matter while recording).
    void setStripsMixingMode (bool mixing);

    // Wire each strip's onTrackFocusRequested callback. MainComponent uses
    // this to forward strip clicks to the TapeStrip's track selection so
    // keyboard shortcuts (A / S / X) target the strip the user touched
    // even when no region has been selected.
    void setOnStripFocusRequested (std::function<void (int)> cb);

private:
    // SUMMARY can be requested by the user (TAPE button) OR by the layout
    // engine when the window shrinks past kAutoCompactChannelWidth. The
    // applied state on each strip is the OR of both.
    bool userWantsCompact = false;
    bool autoCompact      = false;
    void applyCompactState();

    std::array<std::unique_ptr<BusComponent>,       Session::kNumBuses> busStrips;
    std::unique_ptr<MasterStripComponent> masterStrip;

    int currentBank = 0;
    bool showingAllTracks = false;

    void updateBankVisibility();
};
} // namespace duskstudio
