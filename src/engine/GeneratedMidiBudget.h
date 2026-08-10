#pragma once

namespace duskstudio
{
// Fixed-capacity accounting for MIDI generated inside the audio callback.
// Structural bytes are protected from discretionary scheduling until the
// corresponding all-or-nothing message group is emitted.
class GeneratedMidiBudget
{
public:
    explicit constexpr GeneratedMidiBudget (int capacityBytes) noexcept
        : remainingBytes_ (capacityBytes > 0 ? capacityBytes : 0)
    {}

    constexpr bool reserveStructural (int bytes) noexcept
    {
        if (bytes < 0 || bytes > discretionaryBytesAvailable())
            return false;
        reservedStructuralBytes_ += bytes;
        return true;
    }

    constexpr bool spendDiscretionary (int bytes) noexcept
    {
        if (bytes < 0 || bytes > discretionaryBytesAvailable())
            return false;
        remainingBytes_ -= bytes;
        return true;
    }

    constexpr bool consumeStructural (int bytes) noexcept
    {
        if (bytes < 0 || bytes > reservedStructuralBytes_
            || bytes > remainingBytes_)
            return false;
        reservedStructuralBytes_ -= bytes;
        remainingBytes_ -= bytes;
        return true;
    }

    constexpr int discretionaryBytesAvailable() const noexcept
    {
        return remainingBytes_ - reservedStructuralBytes_;
    }

    constexpr int remainingBytes() const noexcept { return remainingBytes_; }
    constexpr int reservedStructuralBytes() const noexcept
    {
        return reservedStructuralBytes_;
    }

private:
    int remainingBytes_ = 0;
    int reservedStructuralBytes_ = 0;
};
} // namespace duskstudio
