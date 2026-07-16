#pragma once

#include <cstdint>

// A fixed-width channel activation mask: the dusk replacement for the JUCE
// BigInteger the audio device layer carries to mark which channels of a device
// are active. Audio devices
// top out at a few dozen channels (the app ceiling is 32 out / 16 in), so a
// 64-bit backing is ample and keeps the type trivially copyable and allocation-
// free - safe to pass by value across the device/callback boundary. Bit index i
// is channel i; setRange / count / highestSetBit mirror the JUCE BigInteger
// operations the device code and selector UI relied on.
namespace duskstudio::device
{
class ChannelSet
{
public:
    static constexpr int kMaxChannels = 64;

    ChannelSet() = default;

    void clear() noexcept { bits = 0; }
    bool isZero() const noexcept { return bits == 0; }

    void setBit (int index, bool shouldBeSet = true) noexcept
    {
        if (index < 0 || index >= kMaxChannels) return;
        const std::uint64_t mask = std::uint64_t (1) << index;
        if (shouldBeSet) bits |=  mask;
        else             bits &= ~mask;
    }

    // Set (or clear) `num` consecutive bits starting at `start`. Matches
    // BigInteger::setRange; out-of-range bits are ignored (channels never
    // exceed kMaxChannels).
    void setRange (int start, int num, bool shouldBeSet) noexcept
    {
        for (int i = start; i < start + num; ++i)
            setBit (i, shouldBeSet);
    }

    bool operator[] (int index) const noexcept
    {
        if (index < 0 || index >= kMaxChannels) return false;
        return (bits & (std::uint64_t (1) << index)) != 0;
    }

    // Number of active channels (BigInteger::countNumberOfSetBits).
    int count() const noexcept
    {
        int n = 0;
        for (std::uint64_t b = bits; b != 0; b &= (b - 1))
            ++n;
        return n;
    }

    // Highest set bit index, or -1 if none (BigInteger::getHighestBit).
    int highestSetBit() const noexcept
    {
        for (int i = kMaxChannels - 1; i >= 0; --i)
            if ((bits & (std::uint64_t (1) << i)) != 0)
                return i;
        return -1;
    }

    bool operator== (const ChannelSet& o) const noexcept { return bits == o.bits; }
    bool operator!= (const ChannelSet& o) const noexcept { return bits != o.bits; }

    // Raw access for bridging to/from a JUCE BigInteger at the seam boundary.
    std::uint64_t raw() const noexcept { return bits; }
    static ChannelSet fromRaw (std::uint64_t r) noexcept { ChannelSet c; c.bits = r; return c; }

private:
    std::uint64_t bits = 0;
};
} // namespace duskstudio::device
