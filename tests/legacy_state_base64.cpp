#include <catch2/catch_test_macros.hpp>

#include "engine/LegacyStateBase64.h"
#include "foundation/Base64.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <vector>

using duskstudio::decodeLegacyStateBase64;
using duskstudio::transcodeLegacyStateBase64;

namespace
{
std::vector<std::uint8_t> decode (const juce::String& s)
{
    return dusk::base64::decode (s.toRawUTF8(), s.getNumBytesAsUTF8());
}
} // namespace

// The AU and multisample migrations move a slot from the JUCE plugin path onto a
// native key. Both encodings are base64 by name only, so a straight copy hands
// the native restore something it cannot read: the slot comes back at its
// defaults and the next save writes those defaults over the last good copy.
TEST_CASE ("legacy state transcodes onto the native keys", "[session][migration]")
{
    const std::vector<std::uint8_t> original { 0x00, 0x11, 0xfe, 0xff, 0x7f, 0x80, 0x2b, 0x2f, 0x41 };
    const auto legacy = juce::MemoryBlock (original.data(), original.size()).toBase64Encoding();

    juce::String migrated;
    REQUIRE (transcodeLegacyStateBase64 (legacy, migrated));
    REQUIRE (migrated != legacy);
    REQUIRE (decode (migrated) == original);
}

TEST_CASE ("legacy state transcode reports unreadable input", "[session][migration]")
{
    juce::String migrated { "untouched" };

    // No '.' byte-count prefix: not the legacy encoding at all. Migrating on a
    // false return would strand the only copy of the user's settings.
    REQUIRE_FALSE (transcodeLegacyStateBase64 ("Zm9vYmFy", migrated));
    REQUIRE_FALSE (transcodeLegacyStateBase64 ("not a blob", migrated));
    REQUIRE (migrated == "untouched");

    // An empty legacy slot has nothing to lose and migrates cleanly.
    REQUIRE (transcodeLegacyStateBase64 ({}, migrated));
    REQUIRE (migrated.isEmpty());
}

// A plugin that saved no state at all encodes as "0.". Reading that as a failure
// would abandon the migration, leaving a DuskMultisample slot on a JUCE format
// that no longer exists - the soundfont would simply never load.
TEST_CASE ("legacy state transcode accepts an empty saved state", "[session][migration]")
{
    const auto legacy = juce::MemoryBlock().toBase64Encoding();
    REQUIRE (legacy == "0.");

    juce::String migrated { "untouched" };
    REQUIRE (transcodeLegacyStateBase64 (legacy, migrated));
    REQUIRE (migrated.isEmpty());
}

// 0.13.1 copied the legacy string onto the native key instead of transcoding it,
// and its reader took it. A session it converted, then never re-saved with the
// slot loaded, still holds that form; reading only RFC 4648 would reset it.
TEST_CASE ("0.13.1-migrated state still decodes", "[session][migration]")
{
    const std::vector<std::uint8_t> original { 0x7f, 0x00, 0xc3, 0x2a, 0xff };
    const auto carried = juce::MemoryBlock (original.data(), original.size()).toBase64Encoding();

    REQUIRE (decode (carried).empty());                    // not RFC 4648
    REQUIRE (decodeLegacyStateBase64 (carried) == original);

    // The fallback must not claim RFC 4648 strings: it needs the '.' byte-count
    // prefix, which base64 output never contains.
    REQUIRE (decodeLegacyStateBase64 (
                 juce::Base64::toBase64 (original.data(), original.size())).empty());
    REQUIRE (decodeLegacyStateBase64 ({}).empty());
}
