#include <catch2/catch_test_macros.hpp>

#include "engine/sfz/SfzHttpHeaders.h"

#include <cstdint>
#include <string>

namespace
{
using namespace duskstudio::sfz::http;

std::uint64_t status (const std::string& line)
{
    long out = -1;
    return parseHttpStatus (line.data(), line.size(), out) ? static_cast<std::uint64_t> (out)
                                                           : 0xffffffffu;
}
} // namespace

// The libcurl socket glue is untested by design, but every rule it delegates to
// this header decides whether a resume is honoured or a size is trusted, so the
// pure logic is covered here.

TEST_CASE ("SFZ Content-Range start parsing", "[sfz][http]")
{
    std::uint64_t out = 0;

    SECTION ("a matching range reports its start")
    {
        REQUIRE (parseContentRangeStart ("bytes 100-199/1234", out));
        CHECK (out == 100);
    }

    SECTION ("a zero start parses to zero, not to a failure")
    {
        REQUIRE (parseContentRangeStart ("bytes 0-0/1", out));
        CHECK (out == 0);
    }

    SECTION ("a 64-bit start does not truncate")
    {
        REQUIRE (parseContentRangeStart ("bytes 4294967296-4294967300/8589934592", out));
        CHECK (out == 4294967296ULL);
    }

    SECTION ("an unsatisfied range has no start")
    {
        CHECK_FALSE (parseContentRangeStart ("bytes */1234", out));
    }

    SECTION ("a dash with no leading digits is refused")
    {
        CHECK_FALSE (parseContentRangeStart ("bytes -100/200", out));
    }

    SECTION ("a value with no dash is refused")
    {
        CHECK_FALSE (parseContentRangeStart ("nonsense", out));
    }
}

TEST_CASE ("SFZ Content-Range total parsing", "[sfz][http]")
{
    std::uint64_t out = 0;
    REQUIRE (parseContentRangeTotal ("bytes 100-199/1234", out));
    CHECK (out == 1234);
    CHECK_FALSE (parseContentRangeTotal ("bytes 0-0/*", out));
    CHECK_FALSE (parseContentRangeTotal ("no-slash", out));
}

TEST_CASE ("SFZ unsigned header parsing rejects overflow and junk", "[sfz][http]")
{
    std::uint64_t out = 0;
    REQUIRE (parseUnsigned ("0", out));
    CHECK (out == 0);
    REQUIRE (parseUnsigned ("18446744073709551615", out));
    CHECK (out == 18446744073709551615ULL);
    CHECK_FALSE (parseUnsigned ("18446744073709551616", out));
    CHECK_FALSE (parseUnsigned ("", out));
    CHECK_FALSE (parseUnsigned ("12a", out));
}

TEST_CASE ("SFZ HTTP status line parsing", "[sfz][http]")
{
    CHECK (status ("HTTP/1.1 206 Partial Content\r\n") == 206);
    CHECK (status ("HTTP/2 200\r\n") == 200);
    long ignored = 0;
    CHECK_FALSE (parseHttpStatus ("Content-Length: 5\r\n",
                                  std::string ("Content-Length: 5\r\n").size(), ignored));
}

TEST_CASE ("SFZ header name matching is case-insensitive", "[sfz][http]")
{
    CHECK (headerNameMatches ("Content-Range", "content-range"));
    CHECK (headerNameMatches ("CONTENT-LENGTH", "content-length"));
    CHECK_FALSE (headerNameMatches ("content-type", "content-range"));
    CHECK_FALSE (headerNameMatches ("content-lengt", "content-length"));
}

TEST_CASE ("SFZ protocols-string libcurl version constant", "[sfz][http]")
{
    // CURLOPT_PROTOCOLS_STR arrived in libcurl 7.85.0; the transport's #if uses
    // the literal because the preprocessor cannot see a constexpr.
    CHECK (kProtocolsStrVersion == 0x075500);
    CHECK (kProtocolsStrVersion == ((7 << 16) | (85 << 8) | 0));
}
