#pragma once

#include <juce_gui_basics/juce_gui_basics.h>
#include <array>
#include "../engine/AudioEngine.h"

namespace duskstudio
{
// Embedded-modal panel that turns the user's typing keyboard into a MIDI
// note source. Pushes Note On / Note Off messages into the engine's
// synthetic "Virtual Keyboard (Dusk Studio)" MidiMessageCollector — to actually
// hear the notes, a track must select that device on its MIDI input
// dropdown and have an instrument plugin loaded.
//
// Key layout matches Reaper:
//   Z S X D C V G B H N J M  -> centreNote + 0..11 (lower octave)
//   Q 2 W 3 E R 5 T 6 Y 7 U  -> centreNote + 12..23
//   I 9 O 0 P                -> centreNote + 24..28
//   Up / Down  : octave shift (clamped to MIDI range)
//   Left/Right : channel shift (1..16)
//
// Note Off detection: keyPressed fires only for key-down (and for OS
// auto-repeat); JUCE doesn't deliver key-up events. A 30 Hz timer scans
// the held set and, for each tracked code, asks whether its typing key is
// still physically down — when it isn't, the matching Note Off is emitted on
// whatever channel/note the original Note On used (so an octave/channel
// shift mid-press doesn't orphan the off).
//
// The "still down?" query is the subtle part. On Linux/XWayland JUCE's
// juce::KeyPress::isKeyCurrentlyDown derives state from the X11 event stream,
// which goes stale during OS key auto-repeat (a held key reads false between
// repeats) — so a held note would drop after ~the debounce window and the
// next repeat's keyPressed would re-trigger it: one held key becomes a stream
// of notes. So the scan uses isCodePhysicallyDown(), which on Linux reads the
// X server's physical key state via XQueryKeymap (auto-repeat-immune; see
// KeyboardStateLinux) and falls back to the JUCE query elsewhere. A small
// kReleaseScans debounce still guards a one-off stale read, and each
// auto-repeat keyPressed resets it as a "still-held" heartbeat for the
// fallback path.
class VirtualKeyboardComponent final : public juce::Component,
                                          private juce::Timer
{
public:
    explicit VirtualKeyboardComponent (AudioEngine& engine);
    ~VirtualKeyboardComponent() override;

    void paint (juce::Graphics&) override;
    void resized() override;
    bool keyPressed (const juce::KeyPress&) override;
    void mouseDown (const juce::MouseEvent&) override;
    void mouseDrag (const juce::MouseEvent&) override;
    void mouseUp   (const juce::MouseEvent&) override;

    // Fired immediately after a Note On / Note Off is queued into
    // the engine's collector. Used by step-record into the piano
    // roll: MainComponent wires these to PianoRollComponent so each
    // typed note also lands as a MidiNote at the playhead. No-op
    // when no listener is attached, so the synth-only path
    // (default) still works.
    std::function<void (int noteNumber, int velocity, int channel)> onNoteOn  = [](int, int, int){};
    std::function<void (int noteNumber, int channel)>               onNoteOff = [](int, int){};

private:
    void timerCallback() override;

    void sendNoteOn  (int note, int vel, int chan);
    void sendNoteOff (int note, int chan);
    void releaseAll();

    // Resolve a JUCE key code to the MIDI note it should trigger. Returns
    // -1 when the key isn't part of the layout.
    int noteForKeyCode (int keyCode) const noexcept;

    AudioEngine& engine;

    // Initialised from appconfig::getVkbCentreNote() in the ctor — the
    // last-used centre is persisted, so re-opening the VKB lands on the
    // user's previous octave. Default on first run is C2 (MIDI 36).
    int centreNote { 36 };

    // Shifts the centre by ±semitones (clamped to MIDI range), releases
    // any held notes (their slot still refers to the old centre), and
    // persists the new value so reopening the VKB keeps it.
    void shiftCentre (int semitones);
    int channel    { 1 };   // 1..16; Left/Right shift.
    int velocity   { 100 };

    // Per-key-code slot tracking. Indexed by the ASCII key code (we only
    // map keys whose codes fit in this range). note >= 0 means the slot
    // holds a live Note On; we re-send the matching Note Off on release
    // using the stored note + channel so mid-press shifts don't orphan it.
    struct HeldNote
    {
        int note    { -1 };
        int channel { -1 };
        // Consecutive timer scans this code has read not-down. Reset to 0
        // when the key reads physically down and by an auto-repeat keyPressed.
        // Note Off fires once it reaches kReleaseScans (see debounce note above).
        int silentScans { 0 };
    };
    // ~100 ms at the 30 Hz scan rate. With XQueryKeymap ground truth a single
    // scan would do; the extra scans absorb a one-off stale read while keeping
    // release latency imperceptible — and leave a window (silentScans in
    // [kReleaseScans-1, kReleaseScans)) where a fast re-press can retrigger on
    // CONFIRMED release evidence rather than a lone stale increment.
    static constexpr int kReleaseScans = 3;
    std::array<HeldNote, 128> held {};

    // Single slot for the note currently being mouse-pressed (separate
    // from the keyboard 'held' array so a user can mouse-drag across
    // keys without competing with typed notes).
    HeldNote mouseHeld {};

    // Helper: which note sits under a given parent-local point? -1 when
    // the point is outside the keyboard rect or on no key. Walks the
    // same layout logic paint() uses; cached key rects would be faster
    // but the keyboard repaints every input event anyway, so the cost
    // is irrelevant on the message thread.
    int noteAtPoint (juce::Point<int> p) const;

    // Returns the typing-keyboard letter / digit that triggers this
    // note, or empty if the note isn't part of the current layout
    // (centreNote shift makes some notes unmapped).
    juce::String letterForNote (int note) const;

    // Transport-like buttons on the header strip for users without a
    // physical typing keyboard (e.g. trackpad-only laptops).
    juce::TextButton octDownBtn { "Oct -" };
    juce::TextButton octUpBtn   { "Oct +" };
    juce::TextButton chDownBtn  { "Ch -" };
    juce::TextButton chUpBtn    { "Ch +" };

    // Bounds of the keyboard rect (computed in resized + paint, used
    // by mouse hit-testing). Excludes header + footer.
    juce::Rectangle<int> keyboardArea;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (VirtualKeyboardComponent)
};
} // namespace duskstudio
