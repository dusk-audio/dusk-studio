#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/Json.h"

using namespace dusk;
using Catch::Matchers::WithinAbs;

TEST_CASE ("dusk::json accessors coerce-or-default like juce::var", "[foundation][json]")
{
    const auto j = json::Json::parse (R"({
        "i": 42, "d": 3.5, "big": 5000000000, "b": true, "bnum": 1,
        "s": "hi", "neg": -7, "f": 0.25
    })");

    SECTION ("present values")
    {
        REQUIRE (json::getInt   (j, "i", 0)    == 42);
        REQUIRE (json::getInt   (j, "d", 0)    == 3);          // double truncates
        REQUIRE_THAT (json::getDouble (j, "d", 0.0), WithinAbs (3.5, 1e-12));
        REQUIRE (json::getInt64 (j, "big", 0)  == 5000000000LL);
        REQUIRE (json::getBool  (j, "b", false) == true);
        REQUIRE (json::getBool  (j, "bnum", false) == true);   // number coerces to bool
        REQUIRE (json::getString(j, "s")       == "hi");
        REQUIRE (json::getInt   (j, "neg", 0)  == -7);
        REQUIRE_THAT (json::getFloat (j, "f", 0.0f), WithinAbs (0.25f, 1e-12));
    }

    SECTION ("missing keys return defaults")
    {
        REQUIRE (json::getInt    (j, "nope", 99)   == 99);
        REQUIRE (json::getBool   (j, "nope", true) == true);
        REQUIRE (json::getString (j, "nope", "x")  == "x");
        REQUIRE_FALSE (json::has (j, "nope"));
    }

    SECTION ("type mismatch falls back, never throws")
    {
        REQUIRE (json::getInt    (j, "s", 5)   == 5);      // string, want int
        REQUIRE_THAT (json::getDouble (j, "b", 1.0), WithinAbs (1.0, 1e-12));  // bool, want double
        REQUIRE (json::getString (j, "i", "d") == "d");    // number, want string
    }

    SECTION ("range guard rejects an out-of-range double")
    {
        const auto big = json::Json::parse (R"({"x": 1e300})");   // finite double, past float + int range
        REQUIRE_THAT (json::getFiniteFloat (big, "x", 7.0f), WithinAbs (7.0f, 1e-12));
        REQUIRE_THAT (json::getFiniteFloat (j, "f", 0.0f),   WithinAbs (0.25f, 1e-12));
        // getInt / getInt64 must return the default rather than narrow (UB).
        REQUIRE (json::getInt   (big, "x", 42) == 42);
        REQUIRE (json::getInt64 (big, "x", 77) == 77);
        // Signed value that fits int64 but exceeds int must fall back, not truncate.
        REQUIRE (json::getInt   (j, "big", -1) == -1);
        REQUIRE (json::getInt64 (j, "big", 0)  == 5000000000LL);   // fits int64: real value
        // Unsigned past signed range must fall back, not wrap negative.
        const auto huge = json::Json::parse (R"({"u": 18446744073709551615})");   // UINT64_MAX
        REQUIRE (json::getInt   (huge, "u", 42) == 42);
        REQUIRE (json::getInt64 (huge, "u", 77) == 77);
    }

    SECTION ("child / array on missing or wrong type are empty, safe")
    {
        REQUIRE (json::child (j, "nope").empty());
        REQUIRE (json::array (j, "i").empty());            // "i" is a number, not array
        REQUIRE (json::getInt (json::child (j, "nope"), "deep", 3) == 3);
    }
}

TEST_CASE ("dusk::json survives a non-object root", "[foundation][json]")
{
    const auto arr = json::Json::parse ("[1,2,3]");
    REQUIRE_FALSE (json::has (arr, "k"));
    REQUIRE (json::getInt (arr, "k", 11) == 11);

    const auto discarded = json::Json::parse ("{bad", nullptr, false);
    REQUIRE (discarded.is_discarded());
    REQUIRE (json::getInt (discarded, "k", 22) == 22);
}
