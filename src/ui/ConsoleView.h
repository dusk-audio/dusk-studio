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
class ConsoleView final : public juce::Component,
                          private juce::Timer
{
public:
    // Engine ref so each ChannelStripComponent gets a PluginSlot
    // reference — UI calls slot.loadFromFile etc. on message thread.
    ConsoleView (Session& session, AudioEngine& engine);

    void paint (juce::Graphics&) override;
    void paintOverChildren (juce::Graphics&) override;
    void resized() override;

    // Keyboard strip focus: Left/Right move the focus ring across the 24
    // strips (auto-flipping the visible bank at a boundary); the focused
    // strip becomes the A/S/X target via the same path a click uses.
    void moveFocus (int delta);
    int  getFocusedStrip() const noexcept { return focusedStrip; }

    // Min: VCA comp's "513 ms" / "0.2 ms" textboxes don't clip.
    static constexpr int kMinChannelWidth = 154;
    // Min: single-row 4-knob COMP labels ("4.0:1", "AUTO", "10.0") fit.
    static constexpr int kMinBusWidth     = 172;
    // Min: with the Pultec HF split across two 2-cell rows, the widest EQ row
    // is the LF 3-cell row; "HF− ATTEN FREQ" now gets a half-strip cell so the
    // floor drops well below the old 4-cell-row constraint.
    static constexpr int kMinMasterWidth  = 210;

    // Ref widths add ~25 px so "VCA 513 ms" / "HF BOOST FREQ" labels
    // never clip in the comfortable-default layout.
    static constexpr int kRefChannelWidth = 188;
    static constexpr int kRefBusWidth     = 192;
    static constexpr int kRefMasterWidth  = 260;

    // Auto-compact triggers (TIMELINE mode independent of user toggle):
    //   Height: EQ+COMP eat ~230 px fixed; below this the fader is
    //           squeezed too far.
    //   Width:  only fires when layout already pushed strips well below
    //           the floor. At kMin the knobs are still readable.
    static constexpr int kAutoCompactStripHeight = 820;
    static constexpr int kAutoCompactChannelWidth = 110;

    static constexpr int kStripGap     = 4;
    static constexpr int kSectionGap   = 12;

    static int minimumContentWidth();

    // Above this width we drop banking and show all 24 strips.
    int fixedWidthFor16Tracks() const;

    // Strips that fit at kMinChannelWidth given current width. Buses +
    // master always reserved. Capped at kNumTracks.
    int  channelsThatFit() const;

    // Lets the parent compute fit/numBanks for a width it's about to
    // set on the console, avoiding the one-frame stale-width race
    // during snap-resizes.
    static int channelsThatFitForWidth (int componentWidth) noexcept;

    // 1 when all fit (no banking). bankStride gives strips per non-
    // final bank; last bank may be sparse.
    int  numBanks()    const noexcept;
    int  bankStride()  const noexcept;

    static int numBanksForWidth (int componentWidth) noexcept;

    // Inclusive 1-based range (e.g. {1, 13} for first bank when
    // stride==13). Used by the dynamic bank-button row.
    std::pair<int, int> rangeForBank (int bankIndex) const noexcept;
    static std::pair<int, int> rangeForBankAtWidth (int bankIndex,
                                                      int componentWidth) noexcept;

    void setBank (int bankIndex);
    int  getBank() const noexcept { return currentBank; }

    // Component accessors for the screenshot-capture harness. Return the
    // realised strip components so the capture pass can snapshot one strip
    // in isolation. nullptr if out of range.
    ChannelStripComponent* getStripComponent (int track) noexcept
    { return (track >= 0 && track < Session::kNumTracks) ? strips[(size_t) track].get() : nullptr; }
    BusComponent* getBusComponent (int bus) noexcept
    { return (bus >= 0 && bus < Session::kNumBuses) ? busStrips[(size_t) bus].get() : nullptr; }
    MasterStripComponent* getMasterStripComponent() noexcept { return masterStrip.get(); }

    // Called from MainComponent::requestQuit before systemRequestedQuit
    // so editor windows (real top-level DocumentWindows on Linux) die
    // in a quiet window rather than racing Mutter's teardown.
    void dropAllPluginEditors();

private:
    Session& sessionRef;

    std::array<std::unique_ptr<ChannelStripComponent>, Session::kNumTracks> strips;

public:
    // OR'd with auto-compact (engaged when window too narrow).
    void setStripsCompactMode (bool compact);

    // Swaps input/IN/ARM/PRINT for 4 AUX send knobs — tracking
    // controls only matter while recording.
    void setStripsMixingMode (bool mixing);

    // Forwards strip clicks to TapeStrip selection so A/S/X target
    // the just-touched strip even when no region was selected.
    void setOnStripFocusRequested (std::function<void (int)> cb);

private:
    // Applied state = userWantsCompact OR autoCompact.
    bool userWantsCompact = false;
    bool autoCompact      = false;
    void applyCompactState();

    std::array<std::unique_ptr<BusComponent>,       Session::kNumBuses> busStrips;
    std::unique_ptr<MasterStripComponent> masterStrip;

    int currentBank = 0;
    bool showingAllTracks = false;

    // -1 = none. Drives the focus ring + (via stripFocusCb) the A/S/X target.
    int focusedStrip = -1;
    std::function<void (int)> stripFocusCb;
    void focusStrip (int track);

    void updateBankVisibility();

    // Polls session.mcu.bank so the MCU surface's Bank Left/Right buttons
    // (handled on the audio thread) drive the visible bank + bank-relative
    // bindings, keeping the surface and the on-screen view on the same 8.
    void timerCallback() override;
};
} // namespace duskstudio
