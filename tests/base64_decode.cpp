#include <catch2/catch_test_macros.hpp>

#include "foundation/Base64.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

namespace
{
std::vector<std::uint8_t> decode (const std::string& s)
{
    return dusk::base64::decode (s.data(), s.size());
}

std::vector<std::uint8_t> bytesOf (const std::string& s)
{
    return { s.begin(), s.end() };
}
} // namespace

// Native plugin state is written with juce::Base64::toBase64 (RFC 4648) and read
// back through dusk::base64::decode. These were once mismatched: the reader used
// juce::MemoryBlock::fromBase64Encoding, a size-prefixed private format that
// rejects every RFC string, so every CLAP / LV2 / VST3 / AU / multisample slot
// silently restored to its default state.
TEST_CASE ("dusk::base64 decodes what juce::Base64 encodes", "[base64][foundation]")
{
    juce::Random random (0x5eed);

    for (int size : { 1, 2, 3, 4, 7, 64, 201, 1024, 8702 })
    {
        std::vector<std::uint8_t> original ((size_t) size);
        for (auto& byte : original)
            byte = (std::uint8_t) random.nextInt (256);

        const auto encoded = juce::Base64::toBase64 (original.data(), original.size());
        REQUIRE (decode (encoded.toStdString()) == original);
    }
}

TEST_CASE ("dusk::base64 matches the RFC 4648 vectors", "[base64][foundation]")
{
    REQUIRE (decode ("").empty());
    REQUIRE (decode ("Zg==")     == bytesOf ("f"));
    REQUIRE (decode ("Zm8=")     == bytesOf ("fo"));
    REQUIRE (decode ("Zm9v")     == bytesOf ("foo"));
    REQUIRE (decode ("Zm9vYg==") == bytesOf ("foob"));
    REQUIRE (decode ("Zm9vYmE=") == bytesOf ("fooba"));
    REQUIRE (decode ("Zm9vYmFy") == bytesOf ("foobar"));

    // The real session payloads use the full alphabet including + and /.
    REQUIRE (decode ("+/8=") == std::vector<std::uint8_t> { 0xfb, 0xff });
}

TEST_CASE ("dusk::base64 accepts missing padding", "[base64][foundation]")
{
    REQUIRE (decode ("Zg")     == bytesOf ("f"));
    REQUIRE (decode ("Zm8")    == bytesOf ("fo"));
    REQUIRE (decode ("Zm9vYg") == bytesOf ("foob"));
}

// The bug this whole header exists to prevent: a JUCE MemoryBlock string must
// never decode as if it were RFC 4648.
TEST_CASE ("dusk::base64 rejects the JUCE MemoryBlock encoding", "[base64][foundation]")
{
    REQUIRE (decode ("12.ABC").empty());

    const std::string payload = "CLAPSTATEblob";
    const juce::MemoryBlock block (payload.data(), payload.size());
    REQUIRE (decode (block.toBase64Encoding().toStdString()).empty());
}

TEST_CASE ("dusk::base64 rejects malformed input", "[base64][foundation]")
{
    REQUIRE (decode ("!!!!").empty());
    REQUIRE (decode ("Zm9v YmFy").empty());       // embedded whitespace
    REQUIRE (decode ("Zm9v\nYmFy").empty());      // wrapped lines
    REQUIRE (decode ("QQ==QQ==").empty());        // padding mid-string
    REQUIRE (decode ("Zm9vYmFyZ").empty());       // group of one carries no byte
    REQUIRE (dusk::base64::decode (nullptr, 8).empty());
}
