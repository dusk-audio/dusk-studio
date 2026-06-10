#include <catch2/catch_test_macros.hpp>

#include "ui/UpdateChecker.h"

using namespace duskstudio::updatecheck;

TEST_CASE ("version triplets parse with and without v prefix / prerelease",
           "[update][version]")
{
    int v[3];
    REQUIRE (parseVersionTriplet ("v0.11.0", v));
    REQUIRE ((v[0] == 0 && v[1] == 11 && v[2] == 0));

    REQUIRE (parseVersionTriplet ("1.2.3", v));
    REQUIRE ((v[0] == 1 && v[1] == 2 && v[2] == 3));

    REQUIRE (parseVersionTriplet ("v0.9.0-beta.3", v));
    REQUIRE ((v[0] == 0 && v[1] == 9 && v[2] == 0));

    REQUIRE_FALSE (parseVersionTriplet ("v0.11", v));
    REQUIRE_FALSE (parseVersionTriplet ("nightly", v));
    REQUIRE_FALSE (parseVersionTriplet ("", v));
}

TEST_CASE ("newer comparison is component-wise, prerelease-blind",
           "[update][version]")
{
    auto newer = [] (const char* a, const char* b)
    {
        int va[3], vb[3];
        REQUIRE (parseVersionTriplet (a, va));
        REQUIRE (parseVersionTriplet (b, vb));
        return isNewer (va, vb);
    };

    REQUIRE (newer ("v0.12.0", "0.11.0"));
    REQUIRE (newer ("v1.0.0",  "0.99.9"));
    REQUIRE (newer ("v0.11.1", "0.11.0"));
    REQUIRE_FALSE (newer ("v0.11.0", "0.11.0"));
    REQUIRE_FALSE (newer ("v0.11.0-beta.2", "0.11.0"));  // same base, not newer
    REQUIRE_FALSE (newer ("v0.10.9", "0.11.0"));
}

TEST_CASE ("highestTag picks the max release from GitHub tags JSON",
           "[update][version]")
{
    const auto json = juce::String (
        R"([{"name":"v0.9.0-beta.3"},{"name":"v0.11.0"},{"name":"v0.10.0"},)"
        R"({"name":"not-a-version"},{"name":"v0.11.1"}])");
    REQUIRE (highestTag (json) == "v0.11.1");

    REQUIRE (highestTag ("") == juce::String());
    REQUIRE (highestTag ("{\"message\":\"rate limited\"}") == juce::String());
    REQUIRE (highestTag ("[]") == juce::String());
}
