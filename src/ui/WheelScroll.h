#pragma once

#include <algorithm>
#include <cmath>

namespace duskstudio::wheel
{
// How a platform's window peer scales a wheel into its delta. X11 sends 50/256
// per click, Windows 0.5 * 120/256, and macOS 10/256 per line of a line-based
// wheel, more once the OS accelerates a fast spin.
struct Profile
{
    float lineNotchDelta;
    // macOS reports every touchpad as precise, so a line-based event there is
    // always a wheel click and is worth at least a notch. Elsewhere touchpads
    // send small line-based events too, and that minimum would make them race.
    bool lineEventIsClick;
};

inline constexpr Profile kMacProfile     { 10.0f / 256.0f, true };
inline constexpr Profile kWindowsProfile { 60.0f / 256.0f, false };
inline constexpr Profile kLinuxProfile   { 50.0f / 256.0f, false };

#if defined (__APPLE__)
inline constexpr Profile kPlatformProfile = kMacProfile;
#elif defined (_WIN32)
inline constexpr Profile kPlatformProfile = kWindowsProfile;
#else
inline constexpr Profile kPlatformProfile = kLinuxProfile;
#endif

// A precise delta is the scrolled distance in points times this.
inline constexpr float kSmoothDeltaPerPoint = 0.5f / 256.0f;
// Trackpad travel worth one notch where a view steps instead of following the
// fingers: the line step browsers use on macOS.
inline constexpr float kPointsPerNotch = 40.0f;

inline Profile& activeProfile() noexcept
{
    static Profile active = kPlatformProfile;
    return active;
}

inline const Profile& profile() noexcept { return activeProfile(); }

// Lets a scenario put another platform's magnitudes through the real views.
inline void setProfileForScenario (const Profile& p) noexcept { activeProfile() = p; }

// False for zero and NaN alike.
inline bool isNonZero (double v) noexcept { return v < 0.0 || v > 0.0; }

// Notches in one event, signed as the delta.
inline float notches (float delta, bool isSmooth, const Profile& p = profile()) noexcept
{
    if (! isNonZero (delta) || ! std::isfinite (delta)) return 0.0f;
    if (isSmooth) return delta / kSmoothDeltaPerPoint / kPointsPerNotch;
    const float n = delta / p.lineNotchDelta;
    return p.lineEventIsClick && std::abs (n) < 1.0f ? std::copysign (1.0f, n) : n;
}

// factorPerNotch per notch, so a trackpad's stream of small deltas zooms as far
// as the notches it adds up to rather than a whole step per event.
inline float zoomFactor (float delta, bool isSmooth, float factorPerNotch,
                         const Profile& p = profile()) noexcept
{
    return std::pow (factorPerNotch, notches (delta, isSmooth, p));
}

// Whole pixels from a stream of wheel events, one per view. A trackpad moves the
// view a pixel per point it travels; a line-based event moves it pixelsPerNotch
// per notch and never less than a pixel, as JUCE's Viewport does. The fraction
// left over is carried to the next event rather than dropped.
class Accumulator
{
public:
    int pixels (float delta, bool isSmooth, float pixelsPerNotch, const Profile& p = profile()) noexcept
    {
        if (isSmooth)
            return take (static_cast<double> (delta) / kSmoothDeltaPerPoint);
        const double amount = static_cast<double> (notches (delta, false, p)) * pixelsPerNotch;
        return take (isNonZero (amount) && std::abs (amount) < 1.0 ? std::copysign (1.0, amount) : amount);
    }

    void reset() noexcept { pending = 0.0; }

private:
    int take (double amount) noexcept
    {
        if (! isNonZero (amount) || ! std::isfinite (amount)) return 0;
        amount = std::clamp (amount, -kLimit, kLimit);
        // A reversal must answer at once, not first pay back what was owed the
        // other way.
        if (isNonZero (pending) && (pending > 0.0) != (amount > 0.0))
            pending = 0.0;
        pending += amount;
        // A delta divided by a notch and scaled lands a hair under a whole
        // pixel it should reach.
        const double whole = std::trunc (pending + std::copysign (kSnap, pending));
        pending -= whole;
        if (std::abs (pending) < kSnap)
            pending = 0.0;
        return static_cast<int> (whole);
    }

    static constexpr double kSnap  = 1.0e-4;
    static constexpr double kLimit = 1.0e6;
    double pending = 0.0;
};
} // namespace duskstudio::wheel
