#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace duskstudio
{
class MidiButtonEdges
{
public:
    bool controller (int channel, int number, int value) noexcept
    { return update (controllers, channel, number, value >= 64); }

    bool note (int channel, int number, int velocity) noexcept
    { return update (notes, channel, number, velocity > 0); }

    void reset() noexcept
    {
        controllers.fill (0);
        notes.fill (0);
    }

private:
    using Bits = std::array<std::uint64_t, 32>;
    Bits controllers {}, notes {};

    static bool update (Bits& bits, int channel, int number, bool down) noexcept
    {
        if (channel < 1 || channel > 16 || number < 0 || number > 127) return false;
        auto& word = bits[static_cast<std::size_t> ((channel - 1) * 2 + number / 64)];
        const auto mask = std::uint64_t { 1 } << (number % 64);
        const bool wasDown = (word & mask) != 0;
        if (down) word |= mask;
        else word &= ~mask;
        return down && ! wasDown;
    }
};
}
