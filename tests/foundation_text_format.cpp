#include <catch2/catch_test_macros.hpp>

#include "foundation/Text.h"

#include <juce_core/juce_core.h>

#include <cstdint>
#include <string>

using namespace dusk;

TEST_CASE ("dusk::text::format matches juce::String::formatted (numeric)", "[foundation][text]")
{
    for (int v : { 0, 7, 42, -5, 1234, -999999 })
    {
        REQUIRE (text::format ("%d", v)   == juce::String::formatted ("%d", v).toStdString());
        REQUIRE (text::format ("%04d", v) == juce::String::formatted ("%04d", v).toStdString());
        REQUIRE (text::format ("%02d", v) == juce::String::formatted ("%02d", v).toStdString());
        REQUIRE (text::format ("%x", v)   == juce::String::formatted ("%x", v).toStdString());
    }

    for (double v : { 0.0, 3.14159, -2.5, 1000.0, 0.001, -0.0009 })
    {
        REQUIRE (text::format ("%.1f", v) == juce::String::formatted ("%.1f", v).toStdString());
        REQUIRE (text::format ("%.2f", v) == juce::String::formatted ("%.2f", v).toStdString());
        REQUIRE (text::format ("%.3f", v) == juce::String::formatted ("%.3f", v).toStdString());
        REQUIRE (text::format ("%g", v)   == juce::String::formatted ("%g", v).toStdString());
    }

    for (std::int64_t v : { std::int64_t (0), std::int64_t (42), std::int64_t (-1),
                            std::int64_t (9000000000LL), std::int64_t (-9000000000LL) })
    {
        REQUIRE (text::format ("%lld", (long long) v)
                     == juce::String::formatted ("%lld", (long long) v).toStdString());
    }

    for (std::uint64_t v : { std::uint64_t (0), std::uint64_t (255),
                             std::uint64_t (18000000000ULL) })
    {
        REQUIRE (text::format ("%llu", (unsigned long long) v)
                     == juce::String::formatted ("%llu", (unsigned long long) v).toStdString());
    }

    REQUIRE (text::format ("%04d%02d%02d-%02d%02d%02d", 2026, 7, 13, 9, 5, 30)
                 == juce::String::formatted ("%04d%02d%02d-%02d%02d%02d",
                                             2026, 7, 13, 9, 5, 30).toStdString());
}

TEST_CASE ("dusk::text::format direct expectations", "[foundation][text]")
{
    SECTION ("%s with char*")
    {
        const std::string name = "kick";
        REQUIRE (text::format ("track: %s", name.c_str()) == "track: kick");
        REQUIRE (text::format ("%s.wav", "master") == "master.wav");
        REQUIRE (text::format ("%s/%s", "a", "b") == "a/b");
    }

    SECTION ("mixed patterns")
    {
        REQUIRE (text::format ("%s %d %.1f dB", "gain", 3, -6.0) == "gain 3 -6.0 dB");
    }

    SECTION ("empty format")
    {
        REQUIRE (text::format ("") == "");
    }

    SECTION ("literal with no conversions")
    {
        REQUIRE (text::format ("no percents here") == "no percents here");
    }

    SECTION ("heap path - output longer than the 256-byte stack buffer")
    {
        const std::string chunk (300, 'x');
        const std::string expected = "[" + chunk + "]";
        REQUIRE (text::format ("[%s]", chunk.c_str()) == expected);
        REQUIRE (text::format ("[%s]", chunk.c_str()).size() == 302);
    }
}
