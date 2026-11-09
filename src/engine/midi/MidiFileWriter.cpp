#include "MidiFileWriter.h"

#include <algorithm>

namespace duskstudio::midi
{
namespace
{
constexpr std::uint64_t kMaxVariableLength  = 0x0fffffff;
constexpr std::uint64_t kMaxChunkLength     = 0xffffffff;
constexpr std::size_t   kMaxTracks          = 0xffff;
constexpr int           kMaxTicksPerQuarter = 0x7fff;
constexpr int           kMaxTempo           = 0xffffff;

constexpr std::uint8_t kMetaTrackName     = 0x03;
constexpr std::uint8_t kMetaEndOfTrack    = 0x2f;
constexpr std::uint8_t kMetaTempo         = 0x51;
constexpr std::uint8_t kMetaTimeSignature = 0x58;

constexpr std::uint8_t kClocksPerClick          = 24;
constexpr std::uint8_t kThirtySecondsPerQuarter = 8;

bool hasSecondDataByte (std::uint8_t status) noexcept
{
    const int kind = status & 0xf0;
    return kind != 0xc0 && kind != 0xd0;
}

bool isWritable (const MidiFileEvent& ev) noexcept
{
    return ev.tick >= 0 && ev.isChannelMessage() && ev.data1 < 0x80
        && (ev.data2 < 0x80 || ! hasSecondDataByte (ev.status));
}

int equalTickRank (const MidiFileEvent& ev) noexcept
{
    if (ev.isNoteOff()) return 0;
    if (ev.isNoteOn()) return 2;
    return (ev.status & 0xf0) == 0xa0 ? 3 : 1;
}

int denominatorPower (int denominator) noexcept
{
    for (int power = 0; power <= 6; ++power)
        if (denominator == (1 << power)) return power;
    return -1;
}

void appendBigEndian (std::vector<std::uint8_t>& out, std::uint32_t value, int numBytes)
{
    for (int shift = 8 * (numBytes - 1); shift >= 0; shift -= 8)
        out.push_back ((std::uint8_t) (value >> shift));
}

bool appendVariableLength (std::vector<std::uint8_t>& out, std::uint64_t value)
{
    if (value > kMaxVariableLength) return false;

    std::uint8_t groups[4] {};
    int count = 0;
    do
    {
        groups[count++] = (std::uint8_t) (value & 0x7f);
        value >>= 7;
    }
    while (value != 0);

    while (--count > 0)
        out.push_back ((std::uint8_t) (groups[count] | 0x80));
    out.push_back (groups[0]);
    return true;
}

// Events must arrive in tick order: a step back wraps to a delta too large to
// encode and fails.
class TrackChunk
{
public:
    explicit TrackChunk (std::vector<std::uint8_t>& fileBytes) : out (fileBytes)
    {
        out.insert (out.end(), { 'M', 'T', 'r', 'k', 0, 0, 0, 0 });
        bodyStart = out.size();
    }

    bool meta (std::int64_t tick, std::uint8_t type, const std::uint8_t* payload, std::size_t size)
    {
        if (! advanceTo (tick)) return false;
        out.push_back (0xff);
        out.push_back (type);
        if (! appendVariableLength (out, size)) return false;
        out.insert (out.end(), payload, payload + size);
        return true;
    }

    bool trackName (const std::string& text)
    {
        return text.empty()
            || meta (0, kMetaTrackName, reinterpret_cast<const std::uint8_t*> (text.data()), text.size());
    }

    bool channel (const MidiFileEvent& ev)
    {
        if (! advanceTo (ev.tick)) return false;
        out.push_back (ev.status);
        out.push_back (ev.data1);
        if (hasSecondDataByte (ev.status))
            out.push_back (ev.data2);
        return true;
    }

    bool finish()
    {
        if (! meta (lastTick, kMetaEndOfTrack, nullptr, 0)) return false;

        const std::uint64_t length = out.size() - bodyStart;
        if (length > kMaxChunkLength) return false;

        auto* lengthField = out.data() + bodyStart - 4;
        lengthField[0] = (std::uint8_t) (length >> 24);
        lengthField[1] = (std::uint8_t) (length >> 16);
        lengthField[2] = (std::uint8_t) (length >> 8);
        lengthField[3] = (std::uint8_t) length;
        return true;
    }

private:
    bool advanceTo (std::int64_t tick)
    {
        const bool fits = appendVariableLength (out, (std::uint64_t) (tick - lastTick));
        lastTick = tick;
        return fits;
    }

    std::vector<std::uint8_t>& out;
    std::size_t  bodyStart = 0;
    std::int64_t lastTick  = 0;
};

bool writeConductor (std::vector<std::uint8_t>& file, const MidiFileConductor& conductor)
{
    const int numerator = conductor.timeSignatureNumerator;
    const int power = denominatorPower (conductor.timeSignatureDenominator);
    if (numerator < 1 || numerator > 255 || power < 0) return false;

    auto tempos = conductor.tempos;
    std::stable_sort (tempos.begin(), tempos.end(),
                      [] (const MidiFileTempo& a, const MidiFileTempo& b) { return a.tick < b.tick; });
    if (tempos.empty() || tempos.front().tick != 0) return false;

    TrackChunk chunk (file);
    if (! chunk.trackName (conductor.name)) return false;

    const std::uint8_t meter[] { (std::uint8_t) numerator, (std::uint8_t) power,
                                 kClocksPerClick, kThirtySecondsPerQuarter };
    if (! chunk.meta (0, kMetaTimeSignature, meter, sizeof (meter))) return false;

    for (const auto& tempo : tempos)
    {
        const int us = tempo.microsecondsPerQuarter;
        if (us < 1 || us > kMaxTempo) return false;

        const std::uint8_t payload[] { (std::uint8_t) (us >> 16), (std::uint8_t) (us >> 8),
                                       (std::uint8_t) us };
        if (! chunk.meta (tempo.tick, kMetaTempo, payload, sizeof (payload))) return false;
    }

    return chunk.finish();
}

bool writeTrack (std::vector<std::uint8_t>& file, const MidiFileTrack& track)
{
    if (! std::all_of (track.events.begin(), track.events.end(), isWritable)) return false;

    auto events = track.events;
    std::stable_sort (events.begin(), events.end(),
                      [] (const MidiFileEvent& a, const MidiFileEvent& b)
                      {
                          return a.tick < b.tick
                              || (a.tick == b.tick && equalTickRank (a) < equalTickRank (b));
                      });

    TrackChunk chunk (file);
    if (! chunk.trackName (track.name)) return false;
    for (const auto& ev : events)
        if (! chunk.channel (ev)) return false;
    return chunk.finish();
}
} // namespace

std::vector<std::uint8_t> writeMidiFileData (int ticksPerQuarter,
                                             const MidiFileConductor& conductor,
                                             const std::vector<MidiFileTrack>& tracks)
{
    if (ticksPerQuarter < 1 || ticksPerQuarter > kMaxTicksPerQuarter
        || tracks.size() + 1 > kMaxTracks)
        return {};

    std::vector<std::uint8_t> file { 'M', 'T', 'h', 'd', 0, 0, 0, 6, 0, 1 };
    appendBigEndian (file, (std::uint32_t) (tracks.size() + 1), 2);
    appendBigEndian (file, (std::uint32_t) ticksPerQuarter, 2);

    if (! writeConductor (file, conductor)) return {};
    for (const auto& track : tracks)
        if (! writeTrack (file, track)) return {};
    return file;
}
} // namespace duskstudio::midi
