#pragma once

#include "../foundation/Base64.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace duskstudio
{
namespace detail
{
// JUCE MemoryBlock's private base64 alphabet: index 0 is '.', then the rest in
// an order that matches nothing in RFC 4648. Kept verbatim so blobs written by
// the JUCE plugin path keep reading back after the de-JUCE swap.
inline int legacySextet (char c) noexcept
{
    static constexpr char alphabet[] =
        ".ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+";
    static const auto reverse = []
    {
        std::array<signed char, 256> t {};
        t.fill (-1);
        for (int i = 0; i < 64; ++i)
            t[(unsigned char) alphabet[i]] = (signed char) i;
        return t;
    }();
    return reverse[(unsigned char) c];
}

// Decode of the MemoryBlock form, mirroring JUCE's MemoryBlock fromBase64Encoding
// bit for bit: a decimal byte count, a '.', then 6-bit groups laid down LSB-first
// into a zero-filled buffer of exactly that count. Characters outside the
// alphabet are skipped without advancing the bit cursor, bits past the declared
// size are dropped, and a shorter-than-declared payload leaves the tail zeroed,
// all exactly as the JUCE reader behaved. False only when there is no '.' at
// all (not this encoding), or when the declared count is beyond what the
// payload could ever have carried, which the real encoder cannot produce.
inline bool decodeLegacyBlob (const std::string& s, std::vector<std::uint8_t>& out)
{
    out.clear();

    const std::size_t dot = s.find ('.');
    if (dot == std::string::npos) return false;

    std::size_t numBytes = 0;
    for (std::size_t i = 0; i < dot; ++i)
    {
        const char c = s[i];
        if (c < '0' || c > '9') break;          // getIntValue semantics: digits, then stop
        numBytes = numBytes * 10 + (std::size_t) (c - '0');
        if (numBytes > (std::size_t) 1 << 30) return false;
    }

    // The encoder writes ceil(bytes * 8 / 6) payload characters, so a count the
    // payload cannot fill is corruption, not a state to allocate.
    const std::size_t dataChars = s.size() - dot - 1;
    if (numBytes > (dataChars * 6) / 8 + 8) return false;

    out.assign (numBytes, 0);
    std::size_t pos = 0;
    for (std::size_t i = dot + 1; i < s.size(); ++i)
    {
        const int v = legacySextet (s[i]);
        if (v < 0) continue;
        for (int k = 0; k < 6; ++k)
        {
            const std::size_t bit = pos + (std::size_t) k;
            if ((bit >> 3) >= out.size()) break;
            out[bit >> 3] = (std::uint8_t) (out[bit >> 3] | (((v >> k) & 1) << (bit & 7)));
        }
        pos += 6;
    }
    return true;
}
} // namespace detail

// Slots that used to be hosted through the JUCE plugin path stored their state
// in JUCE's MemoryBlock encoding; the native keys those slots migrate onto are
// RFC 4648. The two decoders reject each other's output outright, so the string
// has to be transcoded on the way across.
//
// False means the legacy string held something that could not be read back. The
// caller must then abandon the migration and leave the legacy pair untouched:
// moving a slot whose state is dropped en route restores it at its defaults, and
// the next save writes those defaults over the only surviving copy.
inline bool transcodeLegacyStateBase64 (const std::string& legacy, std::string& out)
{
    if (legacy.empty()) { out.clear(); return true; }

    std::vector<std::uint8_t> decoded;
    if (! detail::decodeLegacyBlob (legacy, decoded)) return false;

    // A plugin that saved nothing encodes as "0.", which reads back as zero
    // bytes. That is a successful read of an empty state, not a failure - and
    // failing here would strand the slot's identity along with it.
    out = decoded.empty() ? std::string()
                          : dusk::base64::encode (decoded.data(), decoded.size());
    return true;
}

// 0.13.1 ran the migrations above by copying the string across rather than
// transcoding it, so a session it converted carries the MemoryBlock form on a
// native key. Reading one back is unambiguous: that form always contains a '.',
// which never appears in RFC 4648 output. Empty on anything else.
inline std::vector<std::uint8_t> decodeLegacyStateBase64 (const std::string& s)
{
    std::vector<std::uint8_t> blob;
    if (! detail::decodeLegacyBlob (s, blob))
        blob.clear();
    return blob;
}
} // namespace duskstudio
