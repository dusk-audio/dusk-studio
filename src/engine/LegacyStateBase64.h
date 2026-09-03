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

// Decode of the MemoryBlock form, mirroring JUCE's MemoryBlock encoder bit for
// bit: a decimal byte count, a '.', then 6-bit groups laid down LSB-first into a
// zero-filled buffer of exactly that count. The encoder always writes a decimal
// count and exactly ceil(count * 8 / 6) payload characters drawn from the
// alphabet above, so anything else - a non-decimal count, a short or over-long
// payload, an off-alphabet character, a set padding bit - is a corrupt string.
// False for all of them: the caller must see corruption as unreadable rather
// than restore a slot from a partly zeroed buffer and save that back over the
// last good copy.
inline bool decodeLegacyBlob (const std::string& s, std::vector<std::uint8_t>& out)
{
    out.clear();

    const std::size_t dot = s.find ('.');
    if (dot == std::string::npos || dot == 0) return false;

    std::size_t numBytes = 0;
    for (std::size_t i = 0; i < dot; ++i)
    {
        const char c = s[i];
        if (c < '0' || c > '9') return false;
        const auto digit = (std::size_t) (c - '0');
        constexpr std::size_t kMaxDecodedBytes = (std::size_t) 1 << 30;
        if (numBytes > (kMaxDecodedBytes - digit) / 10) return false;
        numBytes = numBytes * 10 + digit;
    }

    if (s.size() - dot - 1 != (numBytes * 8 + 5) / 6) return false;

    out.assign (numBytes, 0);
    std::size_t pos = 0;
    for (std::size_t i = dot + 1; i < s.size(); ++i)
    {
        const int v = legacySextet (s[i]);
        if (v < 0) { out.clear(); return false; }
        for (int k = 0; k < 6; ++k)
        {
            const std::size_t bit = pos + (std::size_t) k;
            const int b = (v >> k) & 1;

            // The final sextet carries up to five bits past the declared count.
            // The encoder reads those from beyond its buffer, where it sees
            // zeros, so a set one marks a string it could not have written.
            if ((bit >> 3) >= out.size())
            {
                if (b) { out.clear(); return false; }
                continue;
            }

            out[bit >> 3] = (std::uint8_t) (out[bit >> 3] | (b << (bit & 7)));
        }
        pos += 6;
    }
    return true;
}
} // namespace detail

struct DecodedStateBlob
{
    std::vector<std::uint8_t> bytes;
    std::size_t encodedBytes = 0;
    bool supplied = false;
    bool unreadable = false;
};

inline DecodedStateBlob decodeStoredStateBase64 (const std::string& encoded)
{
    DecodedStateBlob result;
    result.encodedBytes = encoded.size();
    result.supplied = ! encoded.empty();
    if (! result.supplied)
        return result;

    result.bytes = dusk::base64::decode (encoded.data(), encoded.size());
    if (! result.bytes.empty())
        return result;

    if (detail::decodeLegacyBlob (encoded, result.bytes))
        return result;

    result.unreadable = true;
    return result;
}

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
