#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <vector>

// Standard MIDI File reader covering the slice the import path needs: every
// track's events in file order, timestamped in the file's own ticks, with the
// note-on/note-off pairing JUCE's MidiFile applied on load (retriggered notes
// get a synthesised note-off, and a note-off sharing a timestamp with a note-on
// of the same pitch sorts first). Meta and sysex events are kept as bare
// timestamped markers - the importer never reads their payload, but their
// positions still set a region's length.
//
// Timestamps stay integer ticks; converting them to seconds needs either the
// SMPTE frame rate below or a tempo map the importer supplies from the session
// tempo, so no seconds conversion lives here.
//
// Three deliberate divergences from the JUCE reader: an 0xF7 escape chunk is
// read with its variable-length size (JUCE treats the byte as a one-byte message
// and then desynchronises), bytes trailing the last announced track are ignored
// rather than failing the whole parse, and an event whose data bytes run past
// the end of its track chunk is dropped rather than zero-filled.
namespace duskstudio::midi
{
struct MidiFileEvent
{
    std::int64_t tick   = 0;
    std::uint8_t status = 0;   // resolved status byte (running status expanded)
    std::uint8_t data1  = 0;   // meta events carry their type byte here
    std::uint8_t data2  = 0;

    bool isChannelMessage() const noexcept { return status >= 0x80 && status < 0xf0; }
    bool isNoteOn()         const noexcept { return (status & 0xf0) == 0x90 && data2 != 0; }
    bool isNoteOff()        const noexcept { return (status & 0xf0) == 0x80
                                                 || ((status & 0xf0) == 0x90 && data2 == 0); }
    bool isController()     const noexcept { return (status & 0xf0) == 0xb0; }

    int channel()          const noexcept { return isChannelMessage() ? (status & 0x0f) + 1 : 0; }
    int noteNumber()       const noexcept { return data1; }
    int velocity()         const noexcept { return data2; }
    int controllerNumber() const noexcept { return data1; }
    int controllerValue()  const noexcept { return data2; }
};

class MidiFileReader
{
public:
    bool readFile (const std::filesystem::path& path);
    bool readData (const std::uint8_t* data, std::size_t size);

    // Positive: ticks per quarter note. Negative: an SMPTE division.
    int  timeFormat() const noexcept          { return format; }
    bool isSmpteTimeFormat() const noexcept   { return format < 0; }

    // Ticks per second for an SMPTE-format file; 0 for a PPQ file or an SMPTE
    // division with no subframe resolution.
    double smpteTicksPerSecond() const noexcept;

    const std::vector<std::vector<MidiFileEvent>>& tracks() const noexcept { return trackEvents; }

private:
    std::vector<std::vector<MidiFileEvent>> trackEvents;
    std::int16_t format = 0;
};
} // namespace duskstudio::midi
