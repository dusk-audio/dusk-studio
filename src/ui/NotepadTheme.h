#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace duskstudio::notepad
{
// Every colour the notepad draws, as 0xRRGGBBAA, in two surfaces: stage (dark,
// the default) and paper (light). Chrome stays dark in both because the host
// DAW has no light theme, so the paper choice re-skins the writing surface
// only.
struct Palette
{
    std::uint32_t shell;   // header, toolbar, status bar
    std::uint32_t stage;   // the writing surface
    std::uint32_t lyric;   // lyric and source text
    std::uint32_t muted;   // section labels, status bar, secondary text
    std::uint32_t chord;   // chords, and nothing else in the panel
    std::uint32_t rule;    // hairlines, section rules, focus rings
};

inline constexpr Palette kStagePalette {
    0x15161bff, 0x1b1d24ff, 0xe4e5eaff, 0x8a8d99ff, 0xe89b34ff, 0x2a2d37ff
};

inline constexpr Palette kPaperPalette {
    0xe8e7e3ff, 0xf3f3f1ff, 0x22232aff, 0x5e6069ff, 0x744008ff, 0xd3d2ccff
};

inline constexpr const Palette& palette (bool darkPage) noexcept
{
    return darkPage ? kStagePalette : kPaperPalette;
}

// Selection and code washes are derived from a named colour rather than named
// themselves, so they follow the page with no second literal to keep in sync.
inline constexpr std::uint32_t withAlpha (std::uint32_t rgba, std::uint8_t alpha) noexcept
{
    return (rgba & 0xffffff00u) | alpha;
}

// A 1.25 scale. Sizes are logical pixels before the display scale factor.
struct TypeScale
{
    float ui = 12.0f;        // toolbar, tooltips, status bar
    float section = 12.0f;   // section labels, tracked and uppercased
    float chord = 15.0f;     // the chord lane
    float lyric = 17.0f;     // lyrics, and the Markdown source
    float title = 26.0f;     // the song title, once
};

inline constexpr TypeScale kTypeScale {};

// Reading mode steps the lyric and chord sizes together so the two lanes keep
// their proportion at arm's length. Step 0 is the writing size.
inline constexpr int kMinReadingStep = -1;
inline constexpr int kMaxReadingStep = 4;

inline float readingScale (int step) noexcept
{
    const auto clamped = std::clamp (step, kMinReadingStep, kMaxReadingStep);
    return 1.0f + 0.15f * static_cast<float> (clamped);
}

// WCAG relative luminance and contrast ratio, so the palette's legibility is
// asserted by a test rather than eyeballed against a screenshot.
inline double relativeLuminance (std::uint32_t rgba) noexcept
{
    const auto channel = [] (std::uint32_t byte)
    {
        const auto value = static_cast<double> (byte) / 255.0;
        return value <= 0.04045 ? value / 12.92 : std::pow ((value + 0.055) / 1.055, 2.4);
    };
    return 0.2126 * channel ((rgba >> 24) & 0xffu)
         + 0.7152 * channel ((rgba >> 16) & 0xffu)
         + 0.0722 * channel ((rgba >> 8) & 0xffu);
}

inline double contrastRatio (std::uint32_t a, std::uint32_t b) noexcept
{
    const auto la = relativeLuminance (a);
    const auto lb = relativeLuminance (b);
    return (std::max (la, lb) + 0.05) / (std::min (la, lb) + 0.05);
}
} // namespace duskstudio::notepad
