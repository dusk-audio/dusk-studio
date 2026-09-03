#include <catch2/catch_test_macros.hpp>

#include "engine/LegacyStateBase64.h"
#include "engine/hosting/NativeRestorePolicy.h"
#include "foundation/Base64.h"
#include "session/Session.h"
#include "session/SessionSerializer.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>
#include <vector>

using duskstudio::decodeLegacyStateBase64;
using duskstudio::decodeStoredStateBase64;
using duskstudio::transcodeLegacyStateBase64;

namespace
{
std::vector<std::uint8_t> decode (const std::string& s)
{
    return dusk::base64::decode (s.data(), s.size());
}
} // namespace

// The AU and multisample migrations move a slot from the JUCE plugin path onto a
// native key. Both encodings are base64 by name only, so a straight copy hands
// the native restore something it cannot read: the slot comes back at its
// defaults and the next save writes those defaults over the last good copy.
TEST_CASE ("legacy state transcodes onto the native keys", "[session][migration]")
{
    const std::vector<std::uint8_t> original { 0x00, 0x11, 0xfe, 0xff, 0x7f, 0x80, 0x2b, 0x2f, 0x41 };
    const auto legacy = juce::MemoryBlock (original.data(), original.size())
                            .toBase64Encoding().toStdString();

    std::string migrated;
    REQUIRE (transcodeLegacyStateBase64 (legacy, migrated));
    REQUIRE (migrated != legacy);
    REQUIRE (decode (migrated) == original);

    // The transcoded form must be byte-identical to what juce::Base64 used to
    // write, so sessions saved before and after the de-JUCE swap compare equal.
    REQUIRE (migrated
             == juce::Base64::toBase64 (original.data(), original.size()).toStdString());
}

TEST_CASE ("unreadable stored state is rejected without data loss",
           "[session][migration][native][regression][issue-454]")
{
    SECTION ("legacy migration leaves the source untouched")
    {
        std::string migrated { "untouched" };

        // No '.' byte-count prefix: not the legacy encoding at all. Migrating on a
        // false return would strand the only copy of the user's settings.
        REQUIRE_FALSE (transcodeLegacyStateBase64 ("Zm9vYmFy", migrated));
        REQUIRE_FALSE (transcodeLegacyStateBase64 ("not a blob", migrated));
        REQUIRE (migrated == "untouched");

        // An empty legacy slot has nothing to lose and migrates cleanly.
        REQUIRE (transcodeLegacyStateBase64 ({}, migrated));
        REQUIRE (migrated.empty());
    }

    SECTION ("native state remains saveable after rejection")
    {
        const std::string corruptState = "*** not base64 ***";
        const auto decoded = decodeStoredStateBase64 (corruptState);
        REQUIRE (decoded.supplied);
        REQUIRE (decoded.unreadable);
        REQUIRE (decoded.bytes.empty());
        REQUIRE (decoded.encodedBytes == corruptState.size());

        bool unloaded = false;
        const auto reason = duskstudio::hosting::enforceRestorePolicy (
            true, decoded.supplied, ! decoded.unreadable, "encoded state is unreadable",
            decoded.encodedBytes, [&] { unloaded = true; });
        REQUIRE (unloaded);
        REQUIRE (reason == "saved state was rejected (18 bytes): encoded state is unreadable; "
                           "slot left offline to preserve the saved state");

        const auto target = juce::File::getSpecialLocation (juce::File::tempDirectory)
                                .getNonexistentChildFile (
                                    "dusk-unreadable-native-state", ".json", false);
        const struct ScopedFile
        {
            juce::File file;
            ~ScopedFile() { file.deleteFile(); }
        } scopedFile { target };

        duskstudio::Session session;
        auto& track = session.track (0);
        track.nativeClapPath = "/plugins/Synth.clap";
        track.nativeClapPluginId = "studio.dusk.synth";
        track.nativeClapStateBase64 = corruptState;
        REQUIRE (duskstudio::SessionSerializer::save (session, target));

        duskstudio::Session restored;
        REQUIRE (duskstudio::SessionSerializer::load (restored, target));
        REQUIRE (restored.track (0).nativeClapPath == track.nativeClapPath);
        REQUIRE (restored.track (0).nativeClapPluginId == track.nativeClapPluginId);
        REQUIRE (restored.track (0).nativeClapStateBase64.toStdString() == corruptState);
    }
}

// A plugin that saved no state at all encodes as "0.". Reading that as a failure
// would abandon the migration, leaving a DuskMultisample slot on a JUCE format
// that no longer exists - the soundfont would simply never load.
TEST_CASE ("legacy state transcode accepts an empty saved state", "[session][migration]")
{
    const auto legacy = juce::MemoryBlock().toBase64Encoding().toStdString();
    REQUIRE (legacy == "0.");

    std::string migrated { "untouched" };
    REQUIRE (transcodeLegacyStateBase64 (legacy, migrated));
    REQUIRE (migrated.empty());

    const auto decoded = decodeStoredStateBase64 (legacy);
    REQUIRE (decoded.supplied);
    REQUIRE_FALSE (decoded.unreadable);
    REQUIRE (decoded.bytes.empty());
}

// 0.13.1 copied the legacy string onto the native key instead of transcoding it,
// and its reader took it. A session it converted, then never re-saved with the
// slot loaded, still holds that form; reading only RFC 4648 would reset it.
TEST_CASE ("0.13.1-migrated state still decodes",
           "[session][migration][regression][issue-355]")
{
    const std::vector<std::uint8_t> original { 0x7f, 0x00, 0xc3, 0x2a, 0xff };
    const auto carried = juce::MemoryBlock (original.data(), original.size())
                             .toBase64Encoding().toStdString();

    REQUIRE (decode (carried).empty());                    // not RFC 4648
    REQUIRE (decodeLegacyStateBase64 (carried) == original);
    const auto decoded = decodeStoredStateBase64 (carried);
    REQUIRE (decoded.supplied);
    REQUIRE_FALSE (decoded.unreadable);
    REQUIRE (decoded.bytes == original);

    // The fallback must not claim RFC 4648 strings: it needs the '.' byte-count
    // prefix, which base64 output never contains.
    REQUIRE (decodeLegacyStateBase64 (
                 juce::Base64::toBase64 (original.data(), original.size()).toStdString())
                 .empty());
    REQUIRE (decodeLegacyStateBase64 ({}).empty());
}

// The decoder no longer calls into JUCE, so its bit-order agreement with
// juce::MemoryBlock is an implementation promise rather than a shared code
// path. Sweep every payload length across a few group alignments so a
// bit-cursor mistake at any 6-bit boundary fails loudly here instead of
// silently corrupting a restored plugin state.
TEST_CASE ("legacy decode matches juce::MemoryBlock across lengths", "[session][migration]")
{
    for (int n = 0; n <= 32; ++n)
    {
        std::vector<std::uint8_t> original;
        original.reserve ((size_t) n);
        for (int i = 0; i < n; ++i)
            original.push_back ((std::uint8_t) (i * 37 + n * 11 + 3));

        const auto legacy = juce::MemoryBlock (original.data(), original.size())
                                .toBase64Encoding().toStdString();

        REQUIRE (decodeLegacyStateBase64 (legacy) == original);
    }
}

// A count that is not decimal, a payload that stops short of the declared byte
// count, a character outside the alphabet, or a final sextet whose bits past the
// declared count are set all mean the string is corrupt.
// Decoding them anyway hands the slot a partly zeroed state that the next save
// writes back over the last good copy, so each must read as unreadable.
TEST_CASE ("malformed legacy state reads as unreadable",
           "[session][migration][regression][issue-454]")
{
    const std::vector<std::uint8_t> original { 0x11, 0x22, 0x33, 0x44, 0x55, 0x66 };
    const auto legacy = juce::MemoryBlock (original.data(), original.size())
                            .toBase64Encoding().toStdString();
    REQUIRE (decodeLegacyStateBase64 (legacy) == original);

    const std::string malformed[] {
        "x.",                                        // count is not a number
        ".ABCDEFGH",                                 // no count at all
        "9999999999999999999999999999999999999999.", // count overflows size_t
        "6.",                                        // count with no payload
        legacy.substr (0, legacy.size() - 1),        // payload one character short
        legacy + "A",                                // payload one character long
        legacy.substr (0, legacy.size() - 1) + "*",  // character outside the alphabet
    };

    for (const auto& s : malformed)
    {
        CAPTURE (s);
        REQUIRE (decodeLegacyStateBase64 (s).empty());

        const auto decoded = decodeStoredStateBase64 (s);
        REQUIRE (decoded.supplied);
        REQUIRE (decoded.unreadable);
        REQUIRE (decoded.bytes.empty());

        std::string migrated { "untouched" };
        REQUIRE_FALSE (transcodeLegacyStateBase64 (s, migrated));
        REQUIRE (migrated == "untouched");
    }

    // The last character of a one-byte blob carries two data bits and four that
    // the encoder always writes as zero, because it reads them from past the end
    // of its own buffer. Setting one produces a string the encoder could not
    // have written, so it must be rejected rather than silently decoded.
    const std::vector<std::uint8_t> oneByte { 0xff };
    REQUIRE (juce::MemoryBlock (oneByte.data(), 1).toBase64Encoding().toStdString() == "1.+C");
    REQUIRE (decodeLegacyStateBase64 ("1.+C") == oneByte);
    REQUIRE (decodeLegacyStateBase64 ("1.+G").empty());
    REQUIRE (decodeStoredStateBase64 ("1.+G").unreadable);
}
