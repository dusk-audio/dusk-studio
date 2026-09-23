#pragma once

#include <cstdint>

namespace duskstudio
{
// 4K band/section palette. Names also drive the track colour-picker labels.
namespace fourKColors
{
    inline constexpr std::uint32_t kHpfBlue   = 0xff4a7c9e;
    inline constexpr std::uint32_t kLfGreen   = 0xff5c9a5c;
    inline constexpr std::uint32_t kLmAmber   = 0xffd9a35a;
    inline constexpr std::uint32_t kHmOrange  = 0xffc47a44;
    inline constexpr std::uint32_t kHfRed     = 0xffc44444;
    inline constexpr std::uint32_t kCompGold  = 0xffd09060;
    inline constexpr std::uint32_t kSendPurple= 0xff9080c0;
    inline constexpr std::uint32_t kPanCyan   = 0xff70b8c0;
    inline constexpr std::uint32_t kMasterTan = 0xffd0a060;
}
} // namespace duskstudio
