#pragma once

#include <string>

// Sharp-spelled name plus octave number for a MIDI note number, with middle C
// (note 60) as "C4" - the spelling and octave numbering JUCE's MidiMessage
// note-name helper produces for the arguments Dusk Studio passes it. Out-of-
// range notes return an empty string.
namespace dusk
{
inline std::string midiNoteName (int note)
{
    if (note < 0 || note > 127) return {};

    static constexpr const char* kNames[12]
        = { "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B" };

    return std::string (kNames[note % 12]) + std::to_string (note / 12 - 1);
}
} // namespace dusk
