#pragma once

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

namespace duskstudio
{
// Slots that used to be hosted through the JUCE plugin path stored their state
// in JUCE's MemoryBlock encoding; the native keys those slots migrate onto are
// RFC 4648. The two decoders reject each other's output outright, so the string
// has to be transcoded on the way across.
//
// False means the legacy string held something that could not be read back. The
// caller must then abandon the migration and leave the legacy pair untouched:
// moving a slot whose state is dropped en route restores it at its defaults, and
// the next save writes those defaults over the only surviving copy.
inline bool transcodeLegacyStateBase64 (const juce::String& legacy, juce::String& out)
{
    if (legacy.isEmpty()) { out.clear(); return true; }

    juce::MemoryBlock decoded;
    if (! decoded.fromBase64Encoding (legacy)) return false;

    // A plugin that saved nothing encodes as "0.", which reads back as zero
    // bytes. That is a successful read of an empty state, not a failure - and
    // failing here would strand the slot's identity along with it.
    out = decoded.getSize() == 0
              ? juce::String()
              : juce::Base64::toBase64 (decoded.getData(), decoded.getSize());
    return true;
}

// 0.13.1 ran the migrations above by copying the string across rather than
// transcoding it, so a session it converted carries the MemoryBlock form on a
// native key. Reading one back is unambiguous: that form always contains a '.',
// which never appears in RFC 4648 output. Empty on anything else.
inline std::vector<std::uint8_t> decodeLegacyStateBase64 (const juce::String& s)
{
    std::vector<std::uint8_t> blob;

    juce::MemoryBlock decoded;
    if (! decoded.fromBase64Encoding (s) || decoded.getSize() == 0)
        return blob;

    const auto* bytes = static_cast<const std::uint8_t*> (decoded.getData());
    blob.assign (bytes, bytes + decoded.getSize());
    return blob;
}
} // namespace duskstudio
