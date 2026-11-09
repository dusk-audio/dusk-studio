#pragma once

#include "../foundation/MidiBuffer.h"
#include "../foundation/SpscIndexFifo.h"

#include <algorithm>
#include <array>
#include <cstdint>

namespace duskstudio::midi
{
// One message the piano roll's keyboard sends to a track.
struct AuditionEvent
{
    int track = -1;
    std::array<std::uint8_t, 3> bytes {};
};

// The notes the piano roll's keyboard plays, handed from the message thread
// (the only producer) to the audio callback (the only consumer). Fixed
// storage: neither side allocates or locks.
//
// A held note's note-off is never refused. The producer keeps the ledger of
// the notes it has started and not ended, and takes a note-on only while the
// queue has room for it and for the note-off of every held note, this one
// included. The consumer only ever frees space, so the room a note-off needs
// is always there when it comes.
class AuditionQueue
{
public:
    static constexpr int kCapacity = 128;   // the FIFO keeps one slot free
    static constexpr int kMaxHeld  = 16;

    // Message thread. False when the note was not taken: out of range, already
    // held, or no room left for it and its note-off.
    bool noteOn (int track, int channel, int note, int velocity) noexcept
    {
        if (track < 0 || note < 0 || note > 127 || find (track, channel, note) >= 0
            || numHeld >= kMaxHeld || fifo.getFreeSpace() < numHeld + 2)
            return false;
        push (track, (std::uint8_t) (0x90 | channelBits (channel)), (std::uint8_t) note,
              (std::uint8_t) std::clamp (velocity, 1, 127));
        held[(std::size_t) numHeld++] = { track, channel, note };
        return true;
    }

    // Message thread. Ends a held note; false, and nothing sent, for a note
    // this queue is not holding.
    bool noteOff (int track, int channel, int note) noexcept
    {
        const int index = find (track, channel, note);
        if (index < 0) return false;
        end (index);
        return true;
    }

    // Message thread. Ends every held note, oldest first.
    void releaseAll() noexcept
    {
        while (numHeld > 0) end (0);
    }

    bool isHeld (int track, int channel, int note) const noexcept { return find (track, channel, note) >= 0; }
    int heldCount() const noexcept { return numHeld; }

    // Message thread, with the consumer quiescent (processing suspended). A
    // track move renumbers the queued messages and the held notes alike, so
    // each note-off still reaches the strip that heard its note-on.
    template <typename NewIndex>
    void renumberTracks (NewIndex&& newIndexOf) noexcept
    {
        const auto renumber = [&newIndexOf] (int& track) { track = newIndexOf (track); };
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (kCapacity, start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i) renumber (slots[(std::size_t) (start1 + i)].track);
        for (int i = 0; i < size2; ++i) renumber (slots[(std::size_t) (start2 + i)].track);
        for (int i = 0; i < numHeld; ++i) renumber (held[(std::size_t) i].track);
    }

    // Audio thread. Hands every queued message to fn in the order it was sent
    // and returns how many there were.
    template <typename Fn>
    int drain (Fn&& fn) noexcept
    {
        const auto& events = slots;
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToRead (kCapacity, start1, size1, start2, size2);
        for (int i = 0; i < size1; ++i) fn (events[(std::size_t) (start1 + i)]);
        for (int i = 0; i < size2; ++i) fn (events[(std::size_t) (start2 + i)]);
        if (size1 + size2 > 0) fifo.finishedRead (size1 + size2);
        return size1 + size2;
    }

private:
    struct Held { int track = -1, channel = 1, note = -1; };

    static int channelBits (int channel) noexcept { return std::clamp (channel, 1, 16) - 1; }

    int find (int track, int channel, int note) const noexcept
    {
        for (int i = 0; i < numHeld; ++i)
        {
            const auto& h = held[(std::size_t) i];
            if (h.track == track && channelBits (h.channel) == channelBits (channel) && h.note == note) return i;
        }
        return -1;
    }

    void end (int index) noexcept
    {
        const auto note = held[(std::size_t) index];
        std::copy (held.begin() + index + 1, held.begin() + numHeld, held.begin() + index);
        --numHeld;
        push (note.track, (std::uint8_t) (0x80 | channelBits (note.channel)), (std::uint8_t) note.note, 0);
    }

    void push (int track, std::uint8_t status, std::uint8_t data1, std::uint8_t data2) noexcept
    {
        int start1 = 0, size1 = 0, start2 = 0, size2 = 0;
        fifo.prepareToWrite (1, start1, size1, start2, size2);
        if (size1 + size2 < 1) return;   // unreachable while the ledger keeps its reserve
        slots[(std::size_t) (size1 > 0 ? start1 : start2)] = { track, { status, data1, data2 } };
        fifo.finishedWrite (1);
    }

    std::array<AuditionEvent, kCapacity> slots {};
    dusk::SpscIndexFifo fifo { kCapacity };
    std::array<Held, kMaxHeld> held {};
    int numHeld = 0;
};

// Appends one track's share of a block's auditioned messages at the block's
// first sample, after whatever the block already holds there. False when the
// buffer is full; the caller then gives the block up the way it gives up any
// block it could not carry whole.
inline bool appendAuditions (const AuditionEvent* events, int count, int track,
                             dusk::MidiBuffer& out) noexcept
{
    for (int i = 0; i < count; ++i)
        if (events[i].track == track && ! out.addEvent (events[i].bytes.data(), 3, 0))
            return false;
    return true;
}

// Whether a track's auditioned notes are heard while nothing else opens its
// strip (transport stopped, IN off). Open from a note-on until every note the
// keyboard started there has ended and the instrument has then stayed quiet
// for kQuietSeconds, so a release rings out instead of being cut, or until
// kMaxRingSeconds after the last note ended for one that never goes quiet.
// Audio thread.
class AuditionGate
{
public:
    static constexpr float  kQuietPeak      = 3.1622776e-5f;   // -90 dBFS
    static constexpr double kQuietSeconds   = 0.05;
    static constexpr double kMaxRingSeconds = 10.0;

    void hear (const std::array<std::uint8_t, 3>& bytes) noexcept
    {
        const int kind = bytes[0] & 0xF0;
        if (kind == 0x90 && bytes[2] > 0)
        {
            ++held;
            open = true;
            quietSamples = 0;
            releasedSamples = 0;
        }
        else if ((kind == 0x80 || kind == 0x90) && held > 0)
            --held;
    }

    // After the track's strip ran. peak is its instrument's output before the
    // fader, so the gate never judges by what it lets through itself.
    void afterBlock (int numSamples, float peak, double sampleRate) noexcept
    {
        if (! open || held > 0) return;
        releasedSamples += numSamples;
        quietSamples = peak <= kQuietPeak ? quietSamples + numSamples : 0;
        if ((double) quietSamples >= sampleRate * kQuietSeconds
            || (double) releasedSamples >= sampleRate * kMaxRingSeconds)
            open = false;
    }

    bool isOpen() const noexcept { return open; }
    int heldNotes() const noexcept { return held; }

private:
    int held = 0;
    bool open = false;
    std::int64_t quietSamples = 0, releasedSamples = 0;
};
} // namespace duskstudio::midi
