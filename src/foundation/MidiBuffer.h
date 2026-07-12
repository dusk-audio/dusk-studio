#pragma once

#include <cstdint>
#include <cstring>
#include <vector>

// Minimal packed MIDI event buffer + message view, mirroring the slice of the
// JUCE MidiBuffer / MidiMessage API the engine's MIDI decoders consume: iterate
// events in order, read each message's raw status/data bytes and its
// sample offset within the block. Owning + pre-sizable so the audio thread can
// clear() and refill it every block with no allocation (reserveBytes() off the
// RT path). It is a byte-level container - it does not parse MIDI semantics;
// the decoders read raw bytes directly.
namespace dusk
{
// Non-owning view of one message's raw bytes (valid while its MidiBuffer lives).
class MidiMessage
{
public:
    MidiMessage() = default;
    MidiMessage (const std::uint8_t* d, int n) noexcept : bytes (d), size (n) {}

    const std::uint8_t* getRawData()     const noexcept { return bytes; }
    int                 getRawDataSize() const noexcept { return size; }

private:
    const std::uint8_t* bytes = nullptr;
    int                 size  = 0;
};

// One iterated event: the message view plus its sample offset within the block.
struct MidiBufferMetadata
{
    MidiMessage message;
    int         samplePosition = 0;

    const MidiMessage& getMessage() const noexcept { return message; }
};

// Packed event storage. Per event: [int samplePosition][int numBytes][bytes...].
// Events are kept in insertion order (NOT auto-sorted by position like JUCE's
// MidiBuffer); the engine bridge fills this by walking the already-sorted JUCE
// buffer, so the order carries over.
class MidiBuffer
{
public:
    void clear()               noexcept { data.clear(); }
    bool isEmpty()       const noexcept { return data.empty(); }

    // Reserves capacity AND caps it: once reserved, addEvent never grows past
    // `n` bytes - it drops events that would exceed the cap instead of
    // reallocating. This is what makes the per-block refill allocation-free on
    // the audio thread. Left unreserved (message-thread use) the buffer grows
    // freely like a plain vector.
    void reserveBytes (std::size_t n) { data.reserve (n); capBytes = n; }

    // Returns false when the event was dropped (invalid, or over the reserved
    // cap) so RT callers can keep whole-block semantics instead of splitting
    // paired events at the cap.
    bool addEvent (const std::uint8_t* bytes, int numBytes, int samplePosition)
    {
        if (numBytes <= 0 || bytes == nullptr) return false;
        const std::size_t base = data.size();
        const std::size_t need = kHeader + (std::size_t) numBytes;
        if (base + need > capBytes) return false;   // over the reserved cap: drop, never realloc
        data.resize (base + need);
        std::memcpy (data.data() + base,               &samplePosition, sizeof (int));
        std::memcpy (data.data() + base + sizeof (int), &numBytes,       sizeof (int));
        std::memcpy (data.data() + base + kHeader, bytes, (std::size_t) numBytes);
        return true;
    }

    class Iterator
    {
    public:
        explicit Iterator (const std::uint8_t* p) noexcept : ptr (p) {}
        bool operator!= (const Iterator& o) const noexcept { return ptr != o.ptr; }
        void operator++ ()                        noexcept { ptr += kHeader + readInt (ptr + 4); }
        MidiBufferMetadata operator* () const noexcept
        {
            return { MidiMessage (ptr + kHeader, readInt (ptr + 4)), readInt (ptr) };
        }
    private:
        const std::uint8_t* ptr;
    };

    Iterator begin() const noexcept { return Iterator (data.data()); }
    Iterator end()   const noexcept { return Iterator (data.data() + data.size()); }

private:
    static constexpr std::size_t kHeader = 2 * sizeof (int);
    static int readInt (const std::uint8_t* p) noexcept
    {
        int v;
        std::memcpy (&v, p, sizeof (int));
        return v;
    }

    static constexpr std::size_t kUnbounded = static_cast<std::size_t> (-1);
    std::vector<std::uint8_t> data;
    std::size_t capBytes = kUnbounded;
};
} // namespace dusk
