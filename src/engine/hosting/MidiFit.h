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
// Releases and neutral expression must survive a block's ordinary event drops.
inline bool endsNotesOrExpression (const std::uint8_t* data, int numBytes) noexcept
{
    if (data == nullptr || numBytes < 2) return false;
    const int status = data[0] & 0xF0;
    if (status == 0xD0) return data[1] == 0;
    if (numBytes < 3) return false;
    if (status == 0xE0) return data[1] == 0 && data[2] == 64;
    if (status == 0x80) return true;
    if (status == 0x90) return data[2] == 0;
    if (status != 0xB0) return false;
    const bool holdingPedal = data[1] == 64 || data[1] == 66 || data[1] == 69;
    return (holdingPedal && data[2] < 64) || data[1] >= 120;
}

// Hands `midi` to a host with `room` left for it, in the host's own unit; a
// message costs cost (data, numBytes) of it, nothing for one the host does not
// take. A block that fits goes whole. One that does not keeps every release or
// neutral expression and as many other messages as the room left over allows,
// in block order. When these alone do not fit, the host gets a reset retaining
// a Stop packet's neutral expression. `room` must hold the reset.
// emit (data, numBytes, samplePosition)
// receives what goes, in order. Each pass over the block asks its own copy of
// `cost`, once per message in block order, so a cost may depend on what came
// before. Audio thread: two passes over the block, no allocation.
template <typename Cost, typename Emit>
void deliverWithinRoom (const dusk::MidiBuffer& midi, std::size_t room, const Cost& cost,
                        Emit&& emit) noexcept
{
    std::size_t total = 0, releases = 0;
    unsigned controllersReset = 0, pitchCentered = 0, pressureZeroed = 0;
    auto counting = cost;
    for (const auto meta : midi)
    {
        const std::size_t c = counting (meta.data, meta.numBytes);
        total += c;
        if (endsNotesOrExpression (meta.data, meta.numBytes))
            releases += c;
        // Parameter-based hosts can reuse reset points already present at zero.
        if (meta.samplePosition != 0 || meta.numBytes < 2) continue;
        const auto* data = meta.data;
        const unsigned channel = 1u << (data[0] & 0x0F);
        const auto status = data[0] & 0xF0;
        if (status == 0xD0 && data[1] == 0) pressureZeroed |= channel;
        else if (meta.numBytes >= 3)
        {
            if (status == 0xB0 && data[1] == 121 && data[2] == 0) controllersReset |= channel;
            else if (status == 0xE0 && data[1] == 0 && data[2] == 64) pitchCentered |= channel;
        }
    }

    auto delivering = cost;
    if (total <= room)
    {
        for (const auto meta : midi)
            if (delivering (meta.data, meta.numBytes) > 0)
                emit (meta.data, meta.numBytes, meta.samplePosition);
        return;
    }

    if (releases <= room)
    {
        std::size_t spare = room - releases;
        for (const auto meta : midi)
        {
            const std::size_t c = delivering (meta.data, meta.numBytes);
            if (c == 0) continue;
            if (endsNotesOrExpression (meta.data, meta.numBytes))
                emit (meta.data, meta.numBytes, meta.samplePosition);
            else if (c <= spare)
            {
                spare -= c;
                emit (meta.data, meta.numBytes, meta.samplePosition);
            }
        }
        return;
    }

    const auto reset = [&emit] (const std::uint8_t* data, int numBytes) noexcept
    {
        emit (data, numBytes, 0);
        return true;
    };
    if ((controllersReset & pitchCentered & pressureZeroed) == 0xFFFFu)
        midi::forEachTransportStopResetMessage (reset);
    else
        midi::forEachHangingResetMessage (reset);
}
} // namespace duskstudio::hosting
