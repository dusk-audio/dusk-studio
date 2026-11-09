#pragma once

#include "MidiFileReader.h"

#include <cstdint>
#include <string>
#include <vector>

// Standard MIDI File writer: format 1, the conductor track first and then one
// track per part, built in memory. Every status byte is written out (no running
// status), and program change and channel pressure carry data1 only; their
// data2 is ignored. A track ends at its last event, the conductor at its last
// tempo change.
//
// Ticks are absolute, and neither events nor tempo changes need arrive sorted.
// At one tick the writer puts note-offs (0x8n, or 0x9n at velocity 0) first,
// then every other channel message but poly pressure, then note-ons, then poly
// pressure (0xAn), keeping the caller's order within each of those four groups.
// A note that ends where the next one of its pitch starts is then never read as
// the new note's release, and pressure for a key arrives once that key is down.
//
// Input the format cannot carry exactly is refused whole: the result is then an
// empty vector, never a file that drops or clamps part of what it was given.
// Refused: ticks per quarter outside 1..0x7FFF; more tracks, conductor
// included, than the header's 0xFFFF; a negative tick; a delta time (the gap to
// the previous event of its track) above 0x0FFFFFFF ticks; a name longer than
// 0x0FFFFFFF bytes; a status outside 0x80..0xEF; a data byte above 0x7F; a time
// signature numerator outside 1..255 or a denominator other than 1, 2, 4, 8,
// 16, 32 or 64; a tempo outside 1..0xFFFFFF microseconds per quarter; a tempo
// map with no change at tick 0.
namespace duskstudio::midi
{
struct MidiFileTempo
{
    std::int64_t tick = 0;
    int microsecondsPerQuarter = 500000;
};

struct MidiFileConductor
{
    std::string name;                   // UTF-8; empty writes no name event
    int timeSignatureNumerator   = 4;   // written at tick 0
    int timeSignatureDenominator = 4;
    std::vector<MidiFileTempo> tempos { MidiFileTempo {} };   // changes sharing a tick keep their order
};

struct MidiFileTrack
{
    std::string name;                   // UTF-8; empty writes no name event
    std::vector<MidiFileEvent> events;  // channel voice messages only
};

[[nodiscard]] std::vector<std::uint8_t> writeMidiFileData (int ticksPerQuarter,
                                                           const MidiFileConductor& conductor,
                                                           const std::vector<MidiFileTrack>& tracks);
} // namespace duskstudio::midi
