#pragma once

#include "../session/SessionLayout.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace duskstudio::consolelayout
{
constexpr int kMinChannelWidth       = 154;
constexpr int kMinBusWidth           = 172;
constexpr int kMinMasterWidth        = 210;
constexpr int kEightUpChannelWidth   = 116;
constexpr int kEightUpThresholdWidth = 1902;
constexpr int kEightUpChannelCount   = SessionLayout::kBankSize;
constexpr int kRefChannelWidth       = 188;
constexpr int kRefBusWidth           = 192;
constexpr int kRefMasterWidth        = 260;
constexpr int kStripGap              = 4;
constexpr int kSectionGap            = 12;
constexpr int kComponentInset        = 6;
constexpr int kOuterPadding          = kComponentInset * 2;
constexpr int kMainPadding           = 8;
constexpr int kMaxScreenPages        = 8;
constexpr int kMinStride = (SessionLayout::kNumTracks + kMaxScreenPages - 1)
                         / kMaxScreenPages;
constexpr int kMinPageButtonWidth = 32;
constexpr int kMinPageButtonGap   = 2;

enum class HorizontalDensity
{
    Comfortable,
    EightUp
};

struct ChannelFit
{
    int channels = kMinStride;
    HorizontalDensity density = HorizontalDensity::Comfortable;
};

constexpr int rightColumnWidth() noexcept
{
    return SessionLayout::kNumBuses * kMinBusWidth
         + (SessionLayout::kNumBuses - 1) * kStripGap
         + kSectionGap * 2
         + kMinMasterWidth;
}

static_assert (kEightUpThresholdWidth
               == kEightUpChannelCount * kEightUpChannelWidth
                + (kEightUpChannelCount - 1) * kStripGap
                + rightColumnWidth()
                + kOuterPadding);

constexpr int fullFloorContentWidthForStride (int stride) noexcept
{
    return stride * kMinChannelWidth
         + (stride - 1) * kStripGap
         + rightColumnWidth()
         + kOuterPadding;
}

constexpr int fullFloorPageContentWidth() noexcept
{
    return fullFloorContentWidthForStride (kMinStride);
}

constexpr int oneChannelFloorContentWidth() noexcept
{
    return fullFloorContentWidthForStride (1);
}

constexpr int minimumPageButtonBandWidth() noexcept
{
    return kMaxScreenPages * kMinPageButtonWidth
         + (kMaxScreenPages - 1) * kMinPageButtonGap;
}

// In compact transport layout, the fixed playback/clock and right utility
// clusters plus their safety insets consume 892 px of MainComponent width.
// Keeping this named makes the true resize floor reflect the tightest usable
// eight-page button band, rather than the much wider three-strip control floor.
constexpr int compactPageButtonBandWidthForMainWidth (int mainWidth) noexcept
{
    return std::max (0, mainWidth - 892);
}

constexpr int responsiveMinimumMainComponentWidth() noexcept
{
    const int oneChannelMainWidth = oneChannelFloorContentWidth() + kMainPadding * 2;
    const int pageButtonMainWidth = 892 + minimumPageButtonBandWidth();
    return std::max (oneChannelMainWidth, pageButtonMainWidth);
}

constexpr int responsiveMinimumContentWidth() noexcept
{
    return responsiveMinimumMainComponentWidth() - kMainPadding * 2;
}

constexpr int allTracksContentWidth() noexcept
{
    return fullFloorContentWidthForStride (SessionLayout::kNumTracks);
}

struct PageButtonMetrics
{
    int width = 0;
    int gap = 0;
    int clusterWidth = 0;

    bool fits (int bandWidth) const noexcept { return clusterWidth <= bandWidth; }
};

inline PageButtonMetrics fitPageButtons (int bandWidth,
                                         int pageCount,
                                         int preferredWidth,
                                         int preferredGap) noexcept
{
    PageButtonMetrics result;
    if (pageCount <= 0) return result;

    result.gap = preferredGap;
    if (pageCount > 1)
        result.gap = std::clamp ((bandWidth - pageCount * kMinPageButtonWidth)
                                   / (pageCount - 1),
                                 kMinPageButtonGap, preferredGap);
    result.width = std::clamp ((bandWidth - (pageCount - 1) * result.gap) / pageCount,
                               kMinPageButtonWidth, preferredWidth);
    result.clusterWidth = pageCount * result.width + (pageCount - 1) * result.gap;
    return result;
}

inline int channelsThatFitForWidth (int componentWidth) noexcept
{
    const int available = std::max (0, componentWidth - kOuterPadding - rightColumnWidth());
    const int perSlot = kMinChannelWidth + kStripGap;
    const int comfortableFit = available > 0
                             ? (available + kStripGap) / perSlot
                             : kMinStride;
    if (componentWidth >= kEightUpThresholdWidth
        && comfortableFit < kEightUpChannelCount)
        return kEightUpChannelCount;
    return std::clamp (comfortableFit, kMinStride, SessionLayout::kNumTracks);
}

inline ChannelFit channelFitForWidth (int componentWidth) noexcept
{
    const int channels = channelsThatFitForWidth (componentWidth);
    // At 2206 px the comfortable eight-strip floor becomes available and
    // supersedes EightUp; below that, 1902..2205 px uses the 116 px tier.
    const bool eightUp = componentWidth >= kEightUpThresholdWidth
                      && channels == kEightUpChannelCount
                      && componentWidth < fullFloorContentWidthForStride (kEightUpChannelCount);
    return { channels, eightUp ? HorizontalDensity::EightUp
                               : HorizontalDensity::Comfortable };
}

inline int screenPageCountForWidth (int componentWidth) noexcept
{
    const int stride = channelsThatFitForWidth (componentWidth);
    return (SessionLayout::kNumTracks + stride - 1) / stride;
}

inline std::pair<int, int> channelRangeForPage (int page, int componentWidth) noexcept
{
    const int stride = channelsThatFitForWidth (componentWidth);
    const int pages = screenPageCountForWidth (componentWidth);
    page = std::clamp (page, 0, std::max (0, pages - 1));
    const int first = page * stride;
    return { first + 1, std::min (SessionLayout::kNumTracks, first + stride) };
}

// Complete horizontal geometry for a realised ConsoleView width. At the named
// full-floor width the normal path preserves every documented strip minimum.
// Responsive legal widths and direct native-window resizes use the defensive
// path, progressively compressing strips and then gaps. Both paths share one
// budget and therefore cannot place the last channel underneath Bus 1.
struct StripGeometry
{
    int componentWidth   = 0;
    int budgetedChannels = 0;
    int visibleChannels  = 0;
    int channelWidth     = 0;
    int busWidth         = 0;
    int masterWidth      = 0;
    int stripGap         = 0;
    int sectionGap       = 0;
    int contentLeft      = kComponentInset;
    int contentRight     = kComponentInset;
    int busColumnLeft    = kComponentInset;
    int lastChannelRight = kComponentInset;
    bool channelsClearBus() const noexcept
    {
        return lastChannelRight + sectionGap <= busColumnLeft;
    }
};

inline StripGeometry makeStripGeometry (int componentWidth,
                                        int budgetedChannels,
                                        int visibleChannels,
                                        HorizontalDensity density = HorizontalDensity::Comfortable) noexcept
{
    StripGeometry g;
    g.componentWidth = std::max (0, componentWidth);
    g.budgetedChannels = std::clamp (budgetedChannels, 1, SessionLayout::kNumTracks);
    g.visibleChannels = std::clamp (visibleChannels, 0, g.budgetedChannels);
    g.contentRight = std::max (g.contentLeft, g.componentWidth - kComponentInset);

    const int areaWidth = std::max (0, g.contentRight - g.contentLeft);
    g.stripGap = kStripGap;
    g.sectionGap = kSectionGap;

    const int fixedGapCount = (g.budgetedChannels - 1) + (SessionLayout::kNumBuses - 1);
    const int initialGapWidth = fixedGapCount * g.stripGap + 2 * g.sectionGap;
    const int availableForStrips = std::max (0, areaWidth - initialGapWidth);
    const int refTotal = g.budgetedChannels * kRefChannelWidth
                       + SessionLayout::kNumBuses * kRefBusWidth
                       + kRefMasterWidth;
    const double scale = refTotal > 0
                       ? std::min (1.0, static_cast<double> (availableForStrips)
                                       / static_cast<double> (refTotal))
                       : 0.0;

    const int channelFloor = density == HorizontalDensity::EightUp
                           ? kEightUpChannelWidth : kMinChannelWidth;
    g.channelWidth = std::max (channelFloor,
                               static_cast<int> (std::lround (kRefChannelWidth * scale)));
    g.busWidth = std::max (kMinBusWidth,
                           static_cast<int> (std::lround (kRefBusWidth * scale)));
    g.masterWidth = std::max (kMinMasterWidth,
                              static_cast<int> (std::lround (kRefMasterWidth * scale)));

    const auto totalWidth = [&]
    {
        return g.budgetedChannels * g.channelWidth
             + SessionLayout::kNumBuses * g.busWidth
             + g.masterWidth
             + fixedGapCount * g.stripGap
             + 2 * g.sectionGap;
    };
    const auto overflow = [&] { return std::max (0, totalWidth() - areaWidth); };
    auto reduceRepeated = [&] (int& value, int count, int floor)
    {
        const int excess = overflow();
        if (excess <= 0 || count <= 0 || value <= floor) return;
        const int perItem = std::min (value - floor, (excess + count - 1) / count);
        value -= perItem;
    };

    // Normal fit: retain all control-size floors.
    reduceRepeated (g.channelWidth, g.budgetedChannels, channelFloor);
    reduceRepeated (g.busWidth, SessionLayout::kNumBuses, kMinBusWidth);
    reduceRepeated (g.masterWidth, 1, kMinMasterWidth);

    // Defensive fit for native or scripted resizes that bypass the window
    // constraint. Compact mode engages when the channel falls below its normal
    // floor; preserving separation is more important than clipping controls.
    reduceRepeated (g.channelWidth, g.budgetedChannels, 1);
    reduceRepeated (g.busWidth, SessionLayout::kNumBuses, 1);
    reduceRepeated (g.masterWidth, 1, 1);
    reduceRepeated (g.sectionGap, 2, 0);
    reduceRepeated (g.stripGap, fixedGapCount, 0);
    reduceRepeated (g.channelWidth, g.budgetedChannels, 0);
    reduceRepeated (g.busWidth, SessionLayout::kNumBuses, 0);
    reduceRepeated (g.masterWidth, 1, 0);

    const int busColumnWidth = SessionLayout::kNumBuses * g.busWidth
                             + (SessionLayout::kNumBuses - 1) * g.stripGap;
    const int rightColumnWidth = busColumnWidth + g.sectionGap + g.masterWidth;
    g.busColumnLeft = g.contentRight - rightColumnWidth;
    g.lastChannelRight = g.contentLeft
                       + g.visibleChannels * g.channelWidth
                       + std::max (0, g.visibleChannels - 1) * g.stripGap;
    return g;
}
} // namespace duskstudio::consolelayout
