#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include "../engine/AudioEngine.h"

namespace focal
{
// Circular icon button used for the transport row (Stop / Play / Record).
// Replaces the text-only buttons with a real-mixer look: a dark filled disc
// with a coloured rim that lights up when the button is the active state,
// and a vector icon (square / triangle / disc) drawn in the centre. Reuses
// JUCE's Button base for click/hover/state plumbing.
class TransportIconButton final : public juce::Button
{
public:
    enum class Icon { Stop, Play, Record, Rewind, Forward, Loop, Punch, Keyboard,
                       Bars, TimeClock, Metronome, Tuner };

    TransportIconButton (const juce::String& name, Icon icon, juce::Colour activeColour);

    // Swap which glyph the button paints. Used by timeFormatToggle so the
    // button can flip between a clock and a bar/beat icon as the user
    // cycles modes without rebuilding the button.
    void setIcon (Icon newIcon) noexcept { iconType = newIcon; repaint(); }

    void paintButton (juce::Graphics&, bool isMouseOver, bool isButtonDown) override;

    // True while held since the most recent mouseDown. Used by the
    // TransportBar's scrub timer to drive 10x playhead motion. Public so
    // the timer can poll it without friending the bar; the read is cheap
    // (a single member access) and it's set by JUCE's standard
    // Button::isDown() infrastructure that we shadow here.
    bool isHeldDown() const noexcept { return heldDown; }

protected:
    // Track held-state on top of JUCE's stateChanged so we can poll
    // synchronously from a timer without needing Button::isDown which
    // queries via the underlying state machine.
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
    // addMouseListener. Currently used by the jumpback button to surface
    // its preset menu on a secondary click.
    void mouseDown (const juce::MouseEvent&) override;

    // MainComponent overlays the stage + bank buttons on top of this bar so
    // it can show all the controls in a single row. When that overlay is
    // active the hintLabel ("Arm a track and press REC to record...") is
    // hidden so it doesn't clash with the buttons sitting on top of it.
    void setHintVisible (bool visible);

    // Below this transport-bar width, the right-side toggles collapse to
    // short labels (SNAP→S, SUMMARY→chevron), the clock label shrinks,
    // and MainComponent shrinks the bank-button overlay to match. The
    // threshold is calibrated to fire just above the OS-enforced resize
    // floor (ConsoleView::minimumContentWidth()+24, ~1790 px) so compact
    // mode actually engages on smaller displays — the previous 1600 px
    // value never triggered because the user couldn't drag below 1790.
    static constexpr int kCompactTransportWidth = 1850;

    // Public surface for any stop path outside this component (hotkeys
    // in MainComponent, external transport calls, MIDI bindings). After
    // engine.stop() drains the take, RecordManager populates
    // getLastRecordErrors() with per-track mid-take failures: WAV write
    // returned false (disk full / writer error) or the MIDI FIFO ran out
    // of capacity and dropped events. Surfaces as an AlertWindow so the
    // user knows the take is partial / corrupt before relying on it.
    // Safe no-op when no errors are pending.
    void notifyRecordStopped();

private:
    void timerCallback() override;
    void refreshButtonStates();

    // After engine.record() returns, RecordManager populates
    // getLastSetupFailures() with the indices of armed tracks whose
    // WAV writer couldn't be set up (disk full, permission denied,
    // missing audio dir, format error). This surfaces those failures
    // as an AlertWindow listing affected tracks so the user doesn't
    // think a silently-dropped take was captured.
    void surfaceRecordSetupFailures();

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

    // Brief-press vs hold dispatch for REW / FFWD.
    //   < kHoldThresholdMs   -> brief press; fires the marker-jump or
    //                            stop-modifier intent on mouse-up.
    //   >= kHoldThresholdMs  -> hold; the scrub timer drives the playhead
    //                            until the button releases.
    static constexpr int kHoldThresholdMs   = 180;
    static constexpr float kScrubMultiplier = 10.0f;
    juce::int64 rewPressedAtMs  = 0;
    juce::int64 ffwdPressedAtMs = 0;
    bool        rewIsScrubbing  = false;
    bool        ffwdIsScrubbing = false;
    juce::int64 lastScrubTickMs = 0;
    // SNAP relocated to TapeStrip's zoom HUD - the gesture controls
    // that govern arrangement-surface behaviour all live on that
    // surface now.
    TransportIconButton clickToggle { "Metronome",
                                          TransportIconButton::Icon::Metronome,
                                          juce::Colour (0xff60c060) };
    juce::TextButton countInToggle { "C/I" };
    juce::Label      bpmCaption;
    juce::Label      bpmValue;
    juce::TextButton tapButton      { "TAP" };
    TransportIconButton tuneButton  { "Tune",
                                          TransportIconButton::Icon::Tuner,
                                          juce::Colour (0xff70d0a0) };

    void syncCompactLabels (bool compact);
    // Right-click on the PUNCH toggle opens a popup with pre-roll /
    // post-roll value pickers + an explanation banner.
    void showPunchSettingsMenu();

    // Tap-tempo state. Each click stamps now() into the ring; on each
    // subsequent click within the kTapTimeoutMs window we average the
    // inter-tap interval over the last kTapWindow stamps and write the
    // resulting BPM to the session. A tap after the timeout resets the
    // ring (the user's starting a new pulse). Message-thread-only state.
    static constexpr int kTapWindow      = 4;
    static constexpr int kTapTimeoutMs   = 2000;
    std::array<juce::int64, kTapWindow> tapStamps {};
    int  tapStampCount = 0;
    void onTap();

    juce::TextButton tapeToggle    { juce::CharPointer_UTF8 ("\xe2\x96\xbe SUMMARY") };  // "▾ SUMMARY" - toggles the arrangement/summary view
    juce::Label      clockLabel;
    // Flips session.timeDisplayMode between Bars/Beats and mm:ss.ms.
    // The toggle's label always shows the FORMAT THE USER WILL SEE
    // NEXT (so a user looking at bars sees "TIME" on the button).
    // Hidden in compact mode to avoid colliding with MainComponent's
    // bank-button overlay; right-click on clockLabel is the always-
    // available fallback.
    // Icon swaps between a bar/beat glyph (when in Time mode — clicking
    // returns to Bars) and a clock glyph (when in Bars mode — clicking
    // switches to Time). Visual flips inverse of the active mode so the
    // icon always shows "what you'll get on click", matching the text
    // convention this replaced.
    TransportIconButton timeFormatToggle { "Time/Bars toggle",
                                              TransportIconButton::Icon::TimeClock,
                                              juce::Colour (0xff7080a0) };
    // Action stored so the clock's right-click handler can fire the
    // same code path as the button's onClick.
    std::function<void()> flipTimeModeOnClock { [] {} };
    juce::Label      hintLabel;

public:
    // Set by MainComponent to receive toggle clicks. Fires after the button's
    // toggle state has flipped - the new collapsed/expanded state is the
    // boolean argument (true = expanded).
    std::function<void (bool)> onTapeStripToggle;

    // Fired when the TUNE button is clicked. MainComponent owns the
    // overlay (similar to the piano roll modal) so this stays decoupled
    // from any specific track-selection lookup.
    std::function<void()> onTunerToggle;

    // Fired when the keyboard icon is clicked (or the K hotkey is pressed
    // and routed back through MainComponent). MainComponent owns the
    // VirtualKeyboardComponent embedded modal.
    std::function<void()> onVirtualKeyboardToggle;

    void setTapeStripExpanded (bool expanded);
    bool isTapeStripExpanded() const;
};
} // namespace focal
