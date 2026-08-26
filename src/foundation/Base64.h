#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// RFC 4648 base64 decoding. JUCE's MemoryBlock base64 pair is a DIFFERENT format
// despite the name - it prefixes a decimal byte count and uses a private alphabet
// - so the two must never be mixed.
//
// Lenient about canonical form, because the job is recovering bytes rather than
// validating: up to two trailing '=' are dropped without checking the group
// needed them, and bits left over in the final group are discarded. Rejection is
// reserved for input that cannot yield bytes at all - a character outside the
// alphabet, or a length whose last group holds a single character.
namespace dusk::base64
{
inline int sextet (char c) noexcept
{
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '+') return 62;
    if (c == '/') return 63;
    return -1;
}

inline std::vector<std::uint8_t> decode (const char* utf8, std::size_t length)
{
    std::vector<std::uint8_t> out;
    if (utf8 == nullptr) return out;

    std::size_t numChars = length;
    for (int pad = 0; pad < 2 && numChars > 0 && utf8[numChars - 1] == '='; ++pad)
        --numChars;

    // A four-char group carries 1..3 bytes, so a remainder of one char is a
    // truncated blob rather than an unpadded one.
    if (numChars % 4 == 1) return out;

    out.reserve ((numChars / 4) * 3 + 2);

    std::uint32_t accumulator = 0;
    int bits = 0;
    for (std::size_t i = 0; i < numChars; ++i)
    {
        const int value = sextet (utf8[i]);
        if (value < 0) { out.clear(); return out; }

        accumulator = (accumulator << 6) | (std::uint32_t) value;
        bits += 6;
        if (bits >= 8)
        {
            bits -= 8;
            out.push_back ((std::uint8_t) ((accumulator >> bits) & 0xffu));
        }
    }
    return out;
}

// Canonical RFC 4648 encoding, '=' padding included: byte-for-byte what JUCE's
// Base64 helper emitted, so state strings written before the de-JUCE swap
// compare equal to ones written after it.
inline std::string encode (const std::uint8_t* data, std::size_t length)
{
    static constexpr char alphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

    std::string out;
    if (data == nullptr || length == 0) return out;
    out.reserve (((length + 2) / 3) * 4);

    std::size_t i = 0;
    for (; i + 3 <= length; i += 3)
    {
        const std::uint32_t v = ((std::uint32_t) data[i] << 16)
                              | ((std::uint32_t) data[i + 1] << 8)
                              | (std::uint32_t) data[i + 2];
        out.push_back (alphabet[(v >> 18) & 63]);
        out.push_back (alphabet[(v >> 12) & 63]);
        out.push_back (alphabet[(v >> 6) & 63]);
        out.push_back (alphabet[v & 63]);
    }

    const std::size_t rem = length - i;
    if (rem == 1)
    {
        const std::uint32_t v = (std::uint32_t) data[i] << 16;
        out.push_back (alphabet[(v >> 18) & 63]);
        out.push_back (alphabet[(v >> 12) & 63]);
        out.push_back ('=');
        out.push_back ('=');
    }
    else if (rem == 2)
    {
        const std::uint32_t v = ((std::uint32_t) data[i] << 16)
                              | ((std::uint32_t) data[i + 1] << 8);
        out.push_back (alphabet[(v >> 18) & 63]);
        out.push_back (alphabet[(v >> 12) & 63]);
        out.push_back (alphabet[(v >> 6) & 63]);
        out.push_back ('=');
    }
    return out;
}
} // namespace dusk::base64
