#pragma once

#include "../../foundation/MidiBuffer.h"
#include "../MidiPanic.h"

#include <cstddef>
#include <cstdint>

// A plug-in host takes a block's MIDI into storage of its own, sized ahead of
// time. When a block does not fit, which events go decides whether a note is
// left sounding.
namespace duskstudio::hosting
{
// A message without which a sounding note may never end: a note-off (or the
// note-on at velocity 0 that is one), a pedal that holds notes coming up
// (sustain, sostenuto, hold 2), and the channel-mode messages, which all
// silence or reset a channel.
inline bool releasesNotes (const std::uint8_t* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes < 3) return false;
    const int status = data[0] & 0xF0;
    if (status == 0x80) return true;
    if (status == 0x90) return data[2] == 0;
    if (status != 0xB0) return false;
    const bool holdingPedal = data[1] == 64 || data[1] == 66 || data[1] == 69;
    return (holdingPedal && data[2] < 64) || data[1] >= 120;
}

// Hands `midi` to a host with `room` left for it, in the host's own unit; a
// message costs cost (data, numBytes) of it, nothing for one the host does not
// take. A block that fits goes whole. One that does not keeps every release
// and, in block order, as many of the other messages as the room left over
// allows. When the releases alone do not fit, the host gets the hanging reset
// instead, which needs `room` to hold it. emit (data, numBytes, samplePosition)
// receives what goes, in order. Audio thread: two passes over the block, no
// allocation.
template <typename Cost, typename Emit>
void deliverWithinRoom (const dusk::MidiBuffer& midi, std::size_t room, Cost&& cost,
                        Emit&& emit) noexcept
{
    std::size_t total = 0, releases = 0;
    for (const auto meta : midi)
    {
        const std::size_t c = cost (meta.data, meta.numBytes);
        total += c;
        if (releasesNotes (meta.data, meta.numBytes))
            releases += c;
    }

    if (total <= room)
    {
        for (const auto meta : midi)
            if (cost (meta.data, meta.numBytes) > 0)
                emit (meta.data, meta.numBytes, meta.samplePosition);
        return;
    }

    if (releases <= room)
    {
        std::size_t spare = room - releases;
        for (const auto meta : midi)
        {
            const std::size_t c = cost (meta.data, meta.numBytes);
            if (c == 0) continue;
            if (releasesNotes (meta.data, meta.numBytes))
                emit (meta.data, meta.numBytes, meta.samplePosition);
            else if (c <= spare)
            {
                spare -= c;
                emit (meta.data, meta.numBytes, meta.samplePosition);
            }
        }
        return;
    }

    midi::forEachHangingResetMessage ([&emit] (const std::uint8_t* data, int numBytes) noexcept
                                      {
                                          emit (data, numBytes, 0);
                                          return true;
                                      });
}
} // namespace duskstudio::hosting
