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
// the held set and queries juce::KeyPress::isKeyCurrentlyDown for each
// tracked code — when a key is no longer down, the matching Note Off is
// emitted on whatever channel/note the original Note On used (so an
// octave/channel shift mid-press doesn't orphan the off).
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

    int centreNote { 60 };  // C4 — Z plays this; arrow keys shift ±12.
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
    };
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
