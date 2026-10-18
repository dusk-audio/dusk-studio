#pragma once

#include <algorithm>
#include <array>
#include <cstdint>

namespace duskstudio::sfz
{
// Direct HTTPS connections exclude private, reserved and multicast ranges.
// https://www.iana.org/assignments/iana-ipv4-special-registry/
// https://www.iana.org/assignments/iana-ipv6-special-registry/
inline bool publicIpv4 (const std::array<std::uint8_t, 4>& ip) noexcept
{
    return ! (ip[0] == 0 || ip[0] == 10 || ip[0] == 127 || ip[0] >= 224
        || (ip[0] == 100 && ip[1] >= 64 && ip[1] <= 127)
        || (ip[0] == 169 && ip[1] == 254)
        || (ip[0] == 172 && ip[1] >= 16 && ip[1] <= 31)
        || (ip[0] == 192 && ip[1] == 0 && (ip[2] == 0 || ip[2] == 2))
        || (ip[0] == 192 && ip[1] == 88 && ip[2] == 99)
        || (ip[0] == 192 && ip[1] == 168)
        || (ip[0] == 198 && (ip[1] == 18 || ip[1] == 19))
        || (ip[0] == 198 && ip[1] == 51 && ip[2] == 100)
        || (ip[0] == 203 && ip[1] == 0 && ip[2] == 113));
}

inline bool publicIpv6 (const std::array<std::uint8_t, 16>& ip) noexcept
{
    const bool mapped = std::all_of (ip.begin(), ip.begin() + 10, [] (auto byte) { return byte == 0; })
        && ip[10] == 0xff && ip[11] == 0xff;
    const bool translated = ip[0] == 0 && ip[1] == 0x64 && ip[2] == 0xff && ip[3] == 0x9b
        && std::all_of (ip.begin() + 4, ip.begin() + 12, [] (auto byte) { return byte == 0; });
    if (mapped || translated) return publicIpv4 ({ ip[12], ip[13], ip[14], ip[15] });
    if ((ip[0] & 0xe0) != 0x20) return false;
    return ! ((ip[0] == 0x20 && ip[1] == 0x01 && ip[2] < 2)
        || (ip[0] == 0x20 && ip[1] == 0x01 && ip[2] == 0x0d && ip[3] == 0xb8)
        || (ip[0] == 0x20 && ip[1] == 0x02)
        || (ip[0] == 0x3f && ip[1] == 0xff && (ip[2] & 0xf0) == 0));
}
} // namespace duskstudio::sfz
