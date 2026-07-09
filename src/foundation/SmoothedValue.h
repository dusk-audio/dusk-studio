#pragma once

#include <cmath>
#include <limits>

// Linear-ramp smoothed value, matching JUCE SmoothedValue<T,
// ValueSmoothingTypes::Linear> exactly (the only smoothing type Dusk uses):
// setTargetValue ramps currentValue toward target over the reset() step count,
// one step per getNextValue(). reset()/setTargetValue()/getNextValue() semantics
// are bit-for-bit the JUCE ones so the DSP behaviour is unchanged.
namespace dusk::audio
{
namespace detail
{
// JUCE approximatelyEqual (finite: absolute floor of the smallest normal, or a
// relative epsilon; non-finite: exact equality).
template <typename T>
inline bool approximatelyEqual (T a, T b) noexcept
{
    if (! (std::isfinite (a) && std::isfinite (b)))
        return a == b;
    const T diff = std::abs (a - b);
    return diff <= std::numeric_limits<T>::min()
        || diff <= std::numeric_limits<T>::epsilon() * std::max (std::abs (a), std::abs (b));
}
} // namespace detail

template <typename FloatType>
class SmoothedValue
{
public:
    SmoothedValue() noexcept = default;
    explicit SmoothedValue (FloatType initialValue) noexcept
        : currentValue (initialValue), target (initialValue) {}

    bool      isSmoothing()     const noexcept { return countdown > 0; }
    FloatType getCurrentValue() const noexcept { return currentValue; }
    FloatType getTargetValue()  const noexcept { return target; }

    void setCurrentAndTargetValue (FloatType newValue) noexcept
    {
        target = currentValue = newValue;
        countdown = 0;
    }

    void reset (double sampleRate, double rampLengthInSeconds) noexcept
    {
        reset ((int) std::floor (rampLengthInSeconds * sampleRate));
    }

    void reset (int numSteps) noexcept
    {
        stepsToTarget = numSteps;
        setCurrentAndTargetValue (target);
    }

    void setTargetValue (FloatType newValue) noexcept
    {
        if (detail::approximatelyEqual (newValue, target))
            return;
        if (stepsToTarget <= 0)
        {
            setCurrentAndTargetValue (newValue);
            return;
        }
        target = newValue;
        countdown = stepsToTarget;
        step = (target - currentValue) / (FloatType) countdown;
    }

    FloatType getNextValue() noexcept
    {
        if (! isSmoothing())
            return target;
        --countdown;
        if (isSmoothing()) currentValue += step;
        else               currentValue = target;
        return currentValue;
    }

    FloatType skip (int numSamples) noexcept
    {
        if (numSamples >= countdown)
        {
            setCurrentAndTargetValue (target);
            return target;
        }
        currentValue += step * (FloatType) numSamples;
        countdown -= numSamples;
        return currentValue;
    }

private:
    FloatType currentValue = 0, target = 0, step = 0;
    int countdown = 0, stepsToTarget = 0;
};
} // namespace dusk::audio
