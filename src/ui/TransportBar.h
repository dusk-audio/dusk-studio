#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include "../engine/AudioEngine.h"
#include <array>

namespace duskstudio
{
// Real-mixer look — dark disc with coloured rim that lights up when
// the button is the active state, vector icon (square / triangle /
// disc) centred. Reuses Button base for click/hover/state.
class TransportIconButton final : public juce::Button
{
public:
    enum class Icon { Stop, Play, Record, Rewind, Forward, Loop, Punch, Keyboard,
                       Bars, TimeClock, Metronome, Tuner };

    TransportIconButton (const juce::String& name, Icon icon, juce::Colour activeColour);

    // Used by timeFormatToggle to flip clock <-> bar/beat without rebuilding.
    void setIcon (Icon newIcon) noexcept { iconType = newIcon; repaint(); }

    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

    // Polled by the scrub timer for 10× playhead motion. Public so the
    // timer doesn't need to friend the bar.
    bool isHeldDown() const noexcept { return heldDown; }

protected:
    void buttonStateChanged() override;

private:
    Icon iconType;
    juce::Colour activeColour;
    bool heldDown = false;
};

class TransportBar final : public juce::Component, private juce::Timer
{
public:
    explicit TransportBar (AudioEngine& engineRef);
    ~TransportBar() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    // Catches right-clicks routed up from child buttons via
    // addMouseListener — jumpback button's preset menu uses this.
    void mouseDown (const juce::MouseEvent&) override;
    // Double-click on the BPM readout opens the tempo prompt.
    void mouseDoubleClick (const juce::MouseEvent&) override;

    // Hidden when MainComponent overlays stage + bank buttons on top.
    void setHintVisible (bool visible);

    // Keyboard-shortcut entry points (MainComponent::keyPressed) — same
    // code paths as the corresponding buttons.
    void tapTempo();
    void openTimeSigMenu();
    void toggleTimeFormat();

    // Hard-coding tuner X from outside is fragile — the right-anchored
    // cluster (BPM / tap / time-sig / mode toggles) shifts the tuner
    // left by ~376 px in expanded mode and ~280 px in compact mode.
    int getTunerLeftX() const noexcept { return tuneButton.getX(); }

    // MainComponent clamps the centered stage-tab overlay against this
    // so RECORDING/MIXING/MASTERING/AUX never slide left over the clock.
    int getClockRightX() const noexcept { return clockLabel.getRight(); }

    // Below this width SNAP->S, TIMELINE->chevron, clock shrinks,
    // MainComponent's bank overlay shrinks to match. Calibrated to
    // fire just above the OS-enforced resize floor (~1790 px) — the
    // previous 1600 never triggered.
    static constexpr int kCompactTransportWidth = 1850;

    // After engine.stop() drains, RecordManager populates
    // getLastRecordErrors with mid-take failures (WAV write returned
    // false / MIDI FIFO overflow). Surfaces as AlertWindow so the user
    // knows the take is partial before relying on it. Safe no-op when
    // no errors are pending.
    void notifyRecordStopped();

    // Re-sync the cached time-signature button text from session.beatsPerBar /
    // beatUnit. Call after changing the time signature outside this bar (e.g. a
    // DP song import) - the 20 Hz refresh does not touch this button.
    void refreshTimeSigButton();

private:
    void timerCallback() override;
    void refreshButtonStates();

    // After engine.record(), RecordManager populates getLastSetupFailures
    // with armed tracks whose writer couldn't be set up (disk full,
    // permission denied, missing audio dir). AlertWindow lists them so
    // the user doesn't think a silently-dropped take was captured.
    void surfaceRecordSetupFailures();

    // MCU REW/FFWD reuse the on-screen Rewind/Forward tap behaviour so the
    // control surface and the on-screen buttons never diverge.
    void rewindTap();
    void forwardTap();

    AudioEngine& engine;
    TransportIconButton stopButton   { "Stop",     TransportIconButton::Icon::Stop,
                                        juce::Colour (0xffd0d0d0) };
    TransportIconButton rewButton    { "Rewind",   TransportIconButton::Icon::Rewind,
                                        juce::Colour (0xffd0d0d0) };
    TransportIconButton playButton   { "Play",     TransportIconButton::Icon::Play,
                                        juce::Colour (0xff60c060) };
    TransportIconButton ffwdButton   { "Forward",  TransportIconButton::Icon::Forward,
                                        juce::Colour (0xffd0d0d0) };
    TransportIconButton recordButton { "Record",   TransportIconButton::Icon::Record,
                                        juce::Colour (0xffd03030) };
    TransportIconButton loopButton    { "Loop",    TransportIconButton::Icon::Loop,
                                          juce::Colour (0xff3aa860) };
    TransportIconButton punchButton   { "Punch",   TransportIconButton::Icon::Punch,
                                          juce::Colour (0xffd05a5a) };
    TransportIconButton keyboardButton { "VKB",    TransportIconButton::Icon::Keyboard,
                                          juce::Colour (0xff7fa0d0) };

    // < threshold = brief press (marker jump / stop modifier on mouse-up).
    // >= threshold = hold (scrub timer drives playhead until release).
    // Kept well above natural single-click duration (~100-300 ms) so a
    // deliberate tap is never misread as a scrub-hold and swallowed.
    static constexpr int kHoldThresholdMs   = 400;
    static constexpr float kScrubMultiplier = 10.0f;
    juce::int64 rewPressedAtMs  = 0;
    juce::int64 ffwdPressedAtMs = 0;
    bool        rewIsScrubbing  = false;
    bool        ffwdIsScrubbing = false;
    juce::int64 lastScrubTickMs = 0;
    TransportIconButton clickToggle { "Metronome",
                                          TransportIconButton::Icon::Metronome,
                                          juce::Colour (0xff60c060) };
    juce::TextButton countInToggle { "C/I" };
    juce::Label      bpmCaption;
    juce::Label      bpmValue;
    juce::TextButton tapButton      { "TAP" };
    // "N/M". Click opens common-time presets + Custom... for an
    // AlertWindow numerator/denominator input.
    juce::TextButton timeSigButton  { "4/4" };
    void showTimeSigMenu();
    void promptCustomTimeSig();
    void applyTimeSig (int numerator, int denominator);
    TransportIconButton tuneButton  { "Tune",
                                          TransportIconButton::Icon::Tuner,
                                          juce::Colour (0xff70d0a0) };

    void syncCompactLabels (bool compact);
    // Right-click PUNCH opens pre-roll / post-roll pickers + an
    // explanation banner.
    void showPunchSettingsMenu();
    // Right-click metronome toggles four flags (click-while-recording /
    // only-during-count-in / click-while-playing / polyphonic).
    void showMetronomeSettingsMenu();

    // Each click stamps now() into the ring; within kTapTimeoutMs we
    // average the last kTapWindow inter-tap intervals into the session
    // BPM. After timeout the ring resets (user's starting a new pulse).
    static constexpr int kTapWindow      = 4;
    static constexpr int kTapTimeoutMs   = 2000;
    std::array<juce::int64, kTapWindow> tapStamps {};
    int  tapStampCount = 0;
    void onTap();

    // Tallies tempo-locked + float MIDI regions + automation points
    // across every lane. Non-zero totals = AlertWindow before applying.
    // Zero totals apply directly. oldBpm restored on cancel so the
    // spinner doesn't lie about the active tempo.
    void confirmAndApplyBpm (float newBpm, float oldBpm);

    // Double-click on the BPM readout. Constant-tempo sessions edit the
    // session tempo (with the usual retime confirm); tempo-mapped sessions
    // edit the point governing the playhead — the value the readout shows.
    void promptEditTempoAtPlayhead();

    juce::TextButton tapeToggle    { juce::CharPointer_UTF8 ("\xe2\x96\xbe TIMELINE") };  // "▾ TIMELINE"
    juce::Label      clockLabel;
    // Label always shows the format the user will get on click (so
    // looking at bars shows "TIME"). Hidden in compact mode — right-
    // click on clockLabel is the always-available fallback.
    // Icon flips inverse of active mode so it shows "what you'll get
    // on click".
    TransportIconButton timeFormatToggle { "Time/Bars toggle",
                                              TransportIconButton::Icon::TimeClock,
                                              juce::Colour (0xff7080a0) };
    // Clock's right-click fires the same code path as the button's
    // onClick.
    std::function<void()> flipTimeModeOnClock { [] {} };
    juce::Label      hintLabel;

public:
    // Set by MainComponent. Fires after toggle flip; bool = new state.
    std::function<void (bool)> onTapeStripToggle;

    // Sync the TIMELINE button's visual state from outside (keyboard
    // shortcut path). dontSendNotification — caller updates the rest of
    // the chain (tapeStrip visibility, console compact mode) directly.
    void setTapeToggleVisualState (bool expanded) noexcept
    {
        tapeToggle.setToggleState (expanded, juce::dontSendNotification);
        // Refresh the chevron/label too, otherwise a keyboard toggle leaves
        // the arrow pointing the wrong way until the next resize.
        syncCompactLabels (getWidth() < kCompactTransportWidth);
    }

    // MainComponent owns the tuner overlay (similar to piano roll
    // modal) so this stays decoupled from track-selection lookup.
    std::function<void()> onTunerToggle;

    // MainComponent owns the VirtualKeyboardComponent embedded modal.
    std::function<void()> onVirtualKeyboardToggle;

    void setTapeStripExpanded (bool expanded);
    bool isTapeStripExpanded() const;
};
} // namespace duskstudio
