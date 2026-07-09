#pragma once

#include <algorithm>
#include <cmath>

// Decibel <-> linear-gain conversions, matching JUCE Decibels exactly
// (0 dB = gain 1.0; any dB at/below minusInfinityDb maps to gain 0, and a gain
// of 0 or below maps back to minusInfinityDb). Default floor -100 dB.
namespace dusk::audio
{
template <typename T>
inline T decibelsToGain (T decibels, T minusInfinityDb = T (-100)) noexcept
{
    return decibels > minusInfinityDb ? std::pow (T (10), decibels * T (0.05)) : T();
}

template <typename T>
inline T gainToDecibels (T gain, T minusInfinityDb = T (-100)) noexcept
{
    return gain > T() ? std::max (minusInfinityDb, static_cast<T> (std::log10 (gain)) * T (20))
                      : minusInfinityDb;
}
} // namespace dusk::audio
