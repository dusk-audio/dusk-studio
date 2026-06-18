#include <catch2/catch_test_macros.hpp>

#include "ui/UpdateChecker.h"

using namespace duskstudio::updatecheck;

TEST_CASE ("versions parse with and without v prefix / prerelease",
           "[update][version]")
{
    ParsedVersion v;
    REQUIRE (parseVersion ("v0.11.0", v));
    REQUIRE ((v.nums[0] == 0 && v.nums[1] == 11 && v.nums[2] == 0));
    REQUIRE (v.prerelease.isEmpty());

    REQUIRE (parseVersion ("1.2.3", v));
    REQUIRE ((v.nums[0] == 1 && v.nums[1] == 2 && v.nums[2] == 3));

    REQUIRE (parseVersion ("v0.9.0-beta.3", v));
    REQUIRE ((v.nums[0] == 0 && v.nums[1] == 9 && v.nums[2] == 0));
    REQUIRE (v.prerelease == "beta.3");

    REQUIRE_FALSE (parseVersion ("v0.11", v));
    REQUIRE_FALSE (parseVersion ("nightly", v));
    REQUIRE_FALSE (parseVersion ("", v));
}

TEST_CASE ("newer comparison is component-wise with semver prerelease rules",
           "[update][version]")
{
    auto newer = [] (const char* a, const char* b)
    {
        ParsedVersion va, vb;
        REQUIRE (parseVersion (a, va));
        REQUIRE (parseVersion (b, vb));
        return isNewer (va, vb);
    };

    REQUIRE (newer ("v0.12.0", "0.11.0"));
    REQUIRE (newer ("v1.0.0",  "0.99.9"));
    REQUIRE (newer ("v0.11.1", "0.11.0"));
    REQUIRE_FALSE (newer ("v0.11.0", "0.11.0"));
    REQUIRE_FALSE (newer ("v0.10.9", "0.11.0"));

    // A stable release outranks its own prereleases — and only those.
    REQUIRE (newer ("v0.11.0", "0.11.0-beta.2"));
    REQUIRE_FALSE (newer ("v0.11.0-beta.2", "0.11.0"));
    REQUIRE_FALSE (newer ("v0.11.0-beta.2", "0.11.0-beta.2"));

    // Prerelease ordering: numeric identifiers compare numerically,
    // "beta" is a prefix of (and ranks below) "beta.N".
    REQUIRE (newer ("v0.11.0-beta.3",  "0.11.0-beta.2"));
    REQUIRE (newer ("v0.11.0-beta.10", "0.11.0-beta.9"));
    REQUIRE (newer ("v0.11.0-beta.1",  "0.11.0-beta"));
    REQUIRE (newer ("v0.11.0-rc.1",    "0.11.0-beta.9"));
    REQUIRE_FALSE (newer ("v0.11.0-beta.2", "0.11.0-beta.3"));

    // A prerelease of the NEXT version still flags from the current stable.
    REQUIRE (newer ("v0.12.0-beta.1", "0.11.0"));
}

TEST_CASE ("highestTag picks the max release from GitHub tags JSON",
           "[update][version]")
{
    const auto json = juce::String (
        R"([{"name":"v0.9.0-beta.3"},{"name":"v0.11.0"},{"name":"v0.10.0"},)"
        R"({"name":"not-a-version"},{"name":"v0.11.1"}])");
    REQUIRE (highestTag (json) == "v0.11.1");

    // Stable outranks its own prerelease regardless of listing order.
    const auto json2 = juce::String (
        R"([{"name":"v0.11.0"},{"name":"v0.11.0-beta.9"}])");
    REQUIRE (highestTag (json2) == "v0.11.0");

    REQUIRE (highestTag ("") == juce::String());
    REQUIRE (highestTag ("{\"message\":\"rate limited\"}") == juce::String());
    REQUIRE (highestTag ("[]") == juce::String());
}
