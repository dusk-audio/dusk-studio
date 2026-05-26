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

    static constexpr int kMinChannelWidth = 154;   // bumped so VCA comp's "513 ms" + "0.2 ms" etc textboxes don't clip at min strip width
    static constexpr int kMinBusWidth     = 172;   // wide enough for single-row 4-knob COMP labels ("4.0:1", "AUTO", "10.0") without truncation
    static constexpr int kMinMasterWidth  = 290;   // master EQ HF row hosts 4 cells — ~70 px per column readable for "HF BOOST FREQ" labels

    static constexpr int kRefChannelWidth = 188;   // +24 so labels like "VCA 513 ms" + "0.0 dB" never clip in comfortable-default layout
    static constexpr int kRefBusWidth     = 192;   // accommodates single-row 4-knob COMP with comfortable per-cell width for value labels
    static constexpr int kRefMasterWidth  = 340;   // 4-cell EQ HF row at ~80 px per column — comfortable for "HF BOOST FREQ" full caption

    // Auto-engage TIMELINE (compact mode) so the EQ/COMP sections collapse
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

    // Number of channel strips that fit at kMinChannelWidth given the
    // CURRENT component width. Buses + master are always reserved on the
    // right; this returns how many channel slots are left for the
    // strip column. Capped at kNumTracks. Used by MainComponent to lay
    // out the dynamic bank-button row.
    int  channelsThatFit() const;

    // Same as channelsThatFit() but for a hypothetical component width
    // — lets the parent compute fit/numBanks for the width it is
    // ABOUT to set on the console (avoiding the one-frame stale-width
    // race when window snap-resizes shrink the console).
    static int channelsThatFitForWidth (int componentWidth) noexcept;

    // Number of banks needed to surface every track at the current
    // fit count. Returns 1 when all 16 fit (no banking). bankStride()
    // gives the number of strips in each non-final bank; the last bank
    // may be sparse.
    int  numBanks()    const noexcept;
    int  bankStride()  const noexcept;

    static int numBanksForWidth (int componentWidth) noexcept;

    // Inclusive 1-based range labels for a given bank index, e.g.
    // {1, 13} for the first bank when bankStride()==13. Used by the
    // dynamic bank-button row in MainComponent.
    std::pair<int, int> rangeForBank (int bankIndex) const noexcept;
    static std::pair<int, int> rangeForBankAtWidth (int bankIndex,
                                                      int componentWidth) noexcept;

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
    // Forwarded by MainComponent when the TIMELINE view toggles. Each track
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
    // TIMELINE can be requested by the user (TAPE button) OR by the layout
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
