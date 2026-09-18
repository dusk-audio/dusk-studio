#pragma once

#include "../../foundation/MidiBuffer.h"

#include <array>
#include <cstddef>
#include <cstdint>

namespace duskstudio::midi
{
// One parser per source. Storage is bounded by what the input collector can
// carry; an oversized SysEx is discarded whole, through its terminating F7.
// Messages emit on completion: realtime can precede a fragmented message whose
// first-byte timestamp is older. Never hold clock ticks behind incomplete SysEx.
class PacketDecoder
{
public:
    void reset() noexcept
    {
        size = 0;
        expected = 0;
        runningStatus = 0;
        inSysex = false;
        overflow = false;
    }

    template <class Receiver>
    void push (const std::uint8_t* bytes, int count, double timeMs, Receiver&& receive)
    {
        for (int i = 0; bytes != nullptr && i < count; ++i)
        {
            const auto byte = bytes[i];
            if (byte >= 0xf8)
            {
                receive (&bytes[i], 1, timeMs);
                continue;
            }

            if (inSysex)
            {
                if (byte < 0x80 || byte == 0xf7)
                {
                    if (size < data.size()) data[size++] = byte;
                    else overflow = true;
                    if (byte == 0xf7)
                    {
                        if (! overflow) receive (data.data(), static_cast<int> (size), startMs);
                        reset();
                    }
                    continue;
                }
                reset();
            }

            if (byte >= 0x80)
            {
                size = 0;
                expected = 0;
                runningStatus = byte < 0xf0 ? byte : 0;
                if (byte == 0xf7) continue;
                startMs = timeMs;
                data[size++] = byte;
                if (byte == 0xf0)
                {
                    inSysex = true;
                    continue;
                }
                expected = messageSize (byte);
                if (expected == 1)
                {
                    receive (data.data(), 1, startMs);
                    size = 0;
                }
                continue;
            }

            if (size == 0)
            {
                if (runningStatus == 0) continue;
                data[size++] = runningStatus;
                expected = messageSize (runningStatus);
                startMs = timeMs;
            }
            data[size++] = byte;
            if (size == expected)
            {
                receive (data.data(), static_cast<int> (size), startMs);
                size = 0;
            }
        }
    }

private:
    static std::size_t messageSize (std::uint8_t status) noexcept
    {
        if (status < 0xf0) return (status & 0xe0) == 0xc0 ? 2 : 3;
        if (status == 0xf1 || status == 0xf3) return 2;
        if (status == 0xf2) return 3;
        return 1;
    }

    static constexpr std::size_t kMaxMessageBytes = dusk::kMidiBlockBytes - sizeof (double) - sizeof (std::int32_t);
    std::array<std::uint8_t, kMaxMessageBytes> data {};
    std::size_t size = 0;
    std::size_t expected = 0;
    std::uint8_t runningStatus = 0;
    double startMs = 0.0;
    bool inSysex = false;
    bool overflow = false;
};
} // namespace duskstudio::midi
