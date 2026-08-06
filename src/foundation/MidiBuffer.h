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
// One ceiling for the whole MIDI path: the capacity of the input ring AND the
// reserve of every buffer a whole-block drain lands in. Both come from here
// because a destination smaller than the ring is a silent bug - a backlog that
// fits the ring but not the destination arrives as an EMPTY block under
// whole-block-or-nothing, losing note-offs for notes already sounding.
//
// On the ring -> MidiBuffer hop equal sizes make that impossible: a MidiBuffer
// record carries 8 bytes of header against the ring's 12, so any record set the
// ring accepted fits a destination of the same size. The later juce -> dusk hops
// have no such margin - a juce record is 6 bytes of header, and a per-track
// buffer accumulates from several sources under no cap at all - so they lean on
// the matched sizing here plus the whole-block escape to stay safe rather than
// on arithmetic.
//
// 16 KB clears a dense controller burst plus a 1 kB-class sysex, so a drop means
// the audio thread has genuinely stopped draining.
constexpr std::size_t kMidiBlockBytes = 16 * 1024;

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
// data / numBytes mirror JUCE's MidiMessageMetadata raw-byte fields so the same
// iteration body (meta.data, meta.numBytes, meta.samplePosition) works over a
// dusk::MidiBuffer - what the native plugin hosts read.
struct MidiBufferMetadata
{
    MidiMessage         message;
    int                 samplePosition = 0;
    const std::uint8_t* data           = nullptr;
    int                 numBytes       = 0;

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

    // Whether a record of this size fits an EMPTY buffer. A failed addEvent
    // means either "this block has run out of room" or "this record can never
    // be delivered here at all"; whole-block-or-nothing callers must tell those
    // apart, because dropping the block over a record no cap could ever hold
    // silences it forever rather than once.
    bool fitsWhenEmpty (int numBytes) const noexcept
    {
        return numBytes > 0 && kHeader + (std::size_t) numBytes <= capBytes;
    }

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
        void operator++ ()                        noexcept { ptr += kHeader + (std::size_t) readInt (ptr + 4); }
        MidiBufferMetadata operator* () const noexcept
        {
            const std::uint8_t* d = ptr + kHeader;
            const int           n = readInt (ptr + 4);
            return { MidiMessage (d, n), readInt (ptr), d, n };
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

// Refill dest with every event of src (any buffer whose iteration yields
// .data / .numBytes / .samplePosition), whole-block-or-nothing: if the events
// cumulatively overrun dest's reserved cap, dest is left empty rather than
// holding a torn prefix - a note-on delivered without the note-off that fell
// off the end hangs the note. A single record too big for dest even when empty
// is undeliverable no matter how the block is cut, so it drops on its own and
// the rest of the block still gets through (an oversized sysex must not silence
// the notes around it).
//
// Audio-thread safe: dest must have been reserveBytes()'d off the RT path.
template <typename SourceBuffer>
inline void copyEventsWhole (const SourceBuffer& src, MidiBuffer& dest)
{
    dest.clear();
    for (const auto meta : src)
    {
        if (dest.addEvent (meta.data, meta.numBytes, meta.samplePosition))
            continue;
        if (dest.fitsWhenEmpty (meta.numBytes))
        {
            dest.clear();
            return;
        }
    }
}
} // namespace dusk
