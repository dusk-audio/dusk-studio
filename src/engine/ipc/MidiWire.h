#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// A block's MIDI on the shared-memory wire between Dusk Studio and its plug-in
// host child, both ways: per event a native-endian int sample position, a
// uint16 length, then that many bytes.
namespace duskstudio::ipc::midiwire
{
constexpr std::size_t kHeaderBytes = sizeof (std::int32_t) + sizeof (std::uint16_t);
// The longest event the 16-bit length can carry.
constexpr int kMaxEventBytes = 0xFFFF;

// Appends one event to the `capacity` bytes at `out`, `written` of them used.
// False, with nothing written, for an event the wire cannot carry or that does
// not fit; the caller goes on with the next one.
inline bool append (std::uint8_t* out, std::uint32_t& written, std::size_t capacity,
                    const std::uint8_t* data, int numBytes, int samplePosition) noexcept
{
    if (data == nullptr || numBytes <= 0 || numBytes > kMaxEventBytes) return false;
    if ((std::size_t) written + kHeaderBytes + (std::size_t) numBytes > capacity) return false;
    const auto sample = (std::int32_t) samplePosition;
    const auto length = (std::uint16_t) numBytes;
    std::memcpy (out + written, &sample, sizeof sample);
    std::memcpy (out + written + sizeof sample, &length, sizeof length);
    std::memcpy (out + written + kHeaderBytes, data, (std::size_t) numBytes);
    written += (std::uint32_t) (kHeaderBytes + (std::size_t) numBytes);
    return true;
}

// Hands each event of the `numBytes` at `in` to fn (data, numBytes,
// samplePosition), in order, straight from `in` whatever its length. An event
// that runs past the end ends the read.
template <typename Fn>
void forEachEvent (const std::uint8_t* in, std::uint32_t numBytes, Fn&& fn) noexcept
{
    std::size_t off = 0;
    while (off + kHeaderBytes <= numBytes)
    {
        std::int32_t sample = 0;
        std::uint16_t length = 0;
        std::memcpy (&sample, in + off, sizeof sample);
        std::memcpy (&length, in + off + sizeof sample, sizeof length);
        off += kHeaderBytes;
        if (off + length > numBytes) return;
        if (length > 0)
            fn (in + off, (int) length, (int) sample);
        off += length;
    }
}
} // namespace duskstudio::ipc::midiwire
