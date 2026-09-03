#include "MidiFileReader.h"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>

namespace duskstudio::midi
{
namespace
{
constexpr std::size_t kMaxFileBytes = 200u * 1024u * 1024u;

struct VariableLength
{
    std::uint32_t value    = 0;
    std::size_t   numBytes = 0;
    bool          valid    = false;
};

VariableLength readVariableLength (const std::uint8_t* data, std::size_t remaining) noexcept
{
    std::uint32_t v = 0;
    const std::size_t limit = std::min<std::size_t> (4, remaining);
    for (std::size_t i = 0; i < limit; ++i)
    {
        const std::uint32_t byte = data[i];
        v = (v << 7) + (byte & 0x7f);
        if ((byte & 0x80) == 0)
            return { v, i + 1, true };
    }
    return {};
}

std::uint32_t bigEndian32 (const std::uint8_t* d) noexcept
{
    return ((std::uint32_t) d[0] << 24) | ((std::uint32_t) d[1] << 16)
         | ((std::uint32_t) d[2] << 8)  | (std::uint32_t) d[3];
}

std::uint16_t bigEndian16 (const std::uint8_t* d) noexcept
{
    return (std::uint16_t) (((std::uint32_t) d[0] << 8) | (std::uint32_t) d[1]);
}

int numDataBytes (std::uint8_t status) noexcept
{
    if (status >= 0xf0)
    {
        if (status == 0xf1 || status == 0xf3) return 1;
        if (status == 0xf2) return 2;
        return 0;
    }

    const int kind = status & 0xf0;
    return (kind == 0xc0 || kind == 0xd0) ? 1 : 2;
}

std::vector<MidiFileEvent> parseTrack (const std::uint8_t* data, std::size_t size)
{
    std::vector<MidiFileEvent> events;
    std::int64_t tick = 0;
    std::uint8_t runningStatus = 0;
    std::size_t  pos = 0;

    while (pos < size)
    {
        const auto delta = readVariableLength (data + pos, size - pos);
        if (! delta.valid) break;
        pos  += delta.numBytes;
        tick += (std::int64_t) delta.value;
        if (pos >= size) break;

        std::uint8_t status = data[pos];
        if (status < 0x80)
        {
            if (runningStatus == 0) break;
            status = runningStatus;
        }
        else
        {
            ++pos;
            // Sysex and meta events do not become the running status, but they
            // do not clear it either - matching the reader this replaces, which
            // is the more forgiving reading of files that span one across them.
            if (status < 0xf0)
                runningStatus = status;
        }

        if (status == 0xff)
        {
            if (pos >= size) break;
            const std::uint8_t type = data[pos++];
            const auto length = readVariableLength (data + pos, size - pos);
            if (! length.valid) break;
            pos += length.numBytes;
            if (length.value > size - pos) break;
            pos += length.value;
            events.push_back ({ tick, 0xff, type, 0 });
        }
        else if (status == 0xf0 || status == 0xf7)
        {
            const auto length = readVariableLength (data + pos, size - pos);
            if (! length.valid) break;
            pos += length.numBytes;
            if (length.value > size - pos) break;
            pos += length.value;
            events.push_back ({ tick, status, 0, 0 });
        }
        else if (status >= 0x80)
        {
            const int wanted = numDataBytes (status);
            if (pos + (std::size_t) wanted > size) break;
            MidiFileEvent ev;
            ev.tick   = tick;
            ev.status = status;
            if (wanted > 0)
                ev.data1 = data[pos];
            if (wanted > 1)
                ev.data2 = data[pos + 1];
            pos += (std::size_t) wanted;
            events.push_back (ev);
        }
        else
        {
            break;
        }
    }

    return events;
}

// Within one timestamp, move the first note-on behind the last note-off of the
// same pitch so a re-struck note closes before it re-opens.
void reorderGroup (std::vector<MidiFileEvent>& events, std::size_t begin, std::size_t end)
{
    for (std::size_t i = begin; i < end;)
    {
        std::size_t firstNoteOn = end;
        for (std::size_t k = i; k < end; ++k)
            if (events[k].isNoteOn()) { firstNoteOn = k; break; }
        if (firstNoteOn == end) return;

        const int channel = events[firstNoteOn].channel();
        const int note    = events[firstNoteOn].noteNumber();

        std::size_t lastNoteOff = end;
        for (std::size_t k = end; k > firstNoteOn; --k)
            if (events[k - 1].isNoteOff() && events[k - 1].channel() == channel
                && events[k - 1].noteNumber() == note)
            { lastNoteOff = k - 1; break; }
        if (lastNoteOff == end)
        {
            i = firstNoteOn + 1;
            continue;
        }

        std::swap (events[firstNoteOn], events[lastNoteOff]);
        i = firstNoteOn + 1;
    }
}

void reorderNoteOnsAfterNoteOffs (std::vector<MidiFileEvent>& events)
{
    const std::size_t n = events.size();
    for (std::size_t groupStart = 0; groupStart < n;)
    {
        std::size_t groupEnd = groupStart + 1;
        while (groupEnd < n && events[groupEnd].tick == events[groupStart].tick)
            ++groupEnd;

        reorderGroup (events, groupStart, groupEnd);
        groupStart = groupEnd;
    }
}

// A note-on for a pitch that is already sounding ends the previous one; the
// file need not say so, so insert the note-off the importer's pairing expects.
void insertRetriggerNoteOffs (std::vector<MidiFileEvent>& events)
{
    std::array<std::array<bool, 128>, 16> sounding {};
    std::vector<MidiFileEvent> paired;
    paired.reserve (events.size());

    for (const auto& ev : events)
    {
        const int channel = ev.channel();
        if (channel >= 1 && channel <= 16 && ev.noteNumber() < 128)
        {
            auto& open = sounding[(std::size_t) (channel - 1)][(std::size_t) ev.noteNumber()];
            if (ev.isNoteOn())
            {
                if (open)
                {
                    MidiFileEvent off;
                    off.tick   = ev.tick;
                    off.status = (std::uint8_t) (0x80 | (channel - 1));
                    off.data1  = (std::uint8_t) ev.noteNumber();
                    paired.push_back (off);
                }
                open = true;
            }
            else if (ev.isNoteOff())
            {
                open = false;
            }
        }
        paired.push_back (ev);
    }

    events = std::move (paired);
}
} // namespace

bool MidiFileReader::readFile (const std::filesystem::path& path)
{
    trackEvents.clear();
    format = 0;

    std::ifstream in (path, std::ios::binary);
    if (! in) return false;

    in.seekg (0, std::ios::end);
    const auto end = in.tellg();
    if (end <= 0) return false;

    const auto size = (std::size_t) end;
    if (size > kMaxFileBytes) return false;

    in.seekg (0, std::ios::beg);
    std::vector<std::uint8_t> bytes (size);
    in.read (reinterpret_cast<char*> (bytes.data()), (std::streamsize) size);
    if ((std::size_t) in.gcount() != size) return false;

    return readData (bytes.data(), size);
}

bool MidiFileReader::readData (const std::uint8_t* data, std::size_t size)
{
    trackEvents.clear();
    format = 0;

    if (data == nullptr || size < 4) return false;

    std::size_t pos = 0;
    if (std::memcmp (data, "MThd", 4) != 0)
    {
        // RMID wraps the MIDI file in a RIFF container; the header sits a few
        // words in.
        if (std::memcmp (data, "RIFF", 4) != 0) return false;

        bool found = false;
        for (pos = 4; pos + 4 <= size && pos <= 32; pos += 4)
            if (std::memcmp (data + pos, "MThd", 4) == 0) { found = true; break; }
        if (! found) return false;
    }
    pos += 4;

    if (pos + 4 > size) return false;
    const std::uint32_t headerLength = bigEndian32 (data + pos);
    pos += 4;
    if (headerLength < 6 || headerLength > size - pos) return false;

    const std::uint16_t fileType  = bigEndian16 (data + pos);
    const std::uint16_t numTracks = bigEndian16 (data + pos + 2);
    format = (std::int16_t) bigEndian16 (data + pos + 4);
    pos += headerLength;

    if (fileType > 2) return false;
    if (fileType == 0 && numTracks != 1) return false;

    trackEvents.reserve (numTracks);
    while (trackEvents.size() < numTracks)
    {
        if (pos + 8 > size) return false;

        const bool isTrackChunk = (std::memcmp (data + pos, "MTrk", 4) == 0);
        const std::uint32_t chunkLength = bigEndian32 (data + pos + 4);
        pos += 8;
        if (chunkLength > size - pos) return false;

        if (isTrackChunk)
        {
            auto events = parseTrack (data + pos, chunkLength);
            reorderNoteOnsAfterNoteOffs (events);
            insertRetriggerNoteOffs (events);
            trackEvents.push_back (std::move (events));
        }
        pos += chunkLength;
    }

    return true;
}

double MidiFileReader::smpteTicksPerSecond() const noexcept
{
    if (format >= 0) return 0.0;

    const int framesPerSecond = -(format >> 8);
    const int ticksPerFrame   = format & 0xff;
    return (double) framesPerSecond * (double) ticksPerFrame;
}
} // namespace duskstudio::midi
