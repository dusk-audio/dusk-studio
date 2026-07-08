#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "foundation/Text.h"

#include <juce_core/juce_core.h>

#include <string>
#include <vector>

using namespace dusk;

namespace
{
const std::vector<std::string> kInputs = {
    "", " ", "  hello  ", "Hello World", "audio.wav", "a/b/c.txt",
    "\tpadded\n", "MixEDCase", "123abc", "  -42px", "3.14xyz", "no-digits",
    "prefix::suffix", "a::b::c", "trailing::", "::leading", "repeat-x-repeat-x-end",
};
} // namespace

TEST_CASE ("dusk::text matches juce::String", "[foundation][text]")
{
    for (const auto& s : kInputs)
    {
        const juce::String j (s);

        REQUIRE (text::trim (s)              == j.trim().toStdString());
        REQUIRE (text::toLowerCase (s)       == j.toLowerCase().toStdString());
        REQUIRE (text::toUpperCase (s)       == j.toUpperCase().toStdString());
        REQUIRE (text::getIntValue (s)       == j.getIntValue());
        REQUIRE_THAT (text::getDoubleValue (s), Catch::Matchers::WithinAbs (j.getDoubleValue(), 1e-12));

        // Empty substring omitted: juce's empty-needle semantics are quirky and
        // no call site passes one.
        for (const std::string sub : { "l", "::", "x", "Hello", "z" })
        {
            const juce::String js (sub);
            REQUIRE (text::contains (s, sub)           == j.contains (js));
            REQUIRE (text::containsIgnoreCase (s, sub) == j.containsIgnoreCase (js));
            REQUIRE (text::startsWith (s, sub)         == j.startsWith (js));
            REQUIRE (text::endsWith (s, sub)           == j.endsWith (js));
            REQUIRE (text::indexOf (s, sub)            == j.indexOf (js));

            REQUIRE (text::upToFirstOccurrenceOf (s, sub, false) == j.upToFirstOccurrenceOf (js, false, false).toStdString());
            REQUIRE (text::upToFirstOccurrenceOf (s, sub, true)  == j.upToFirstOccurrenceOf (js, true,  false).toStdString());
            REQUIRE (text::upToLastOccurrenceOf  (s, sub, false) == j.upToLastOccurrenceOf  (js, false, false).toStdString());
            REQUIRE (text::fromFirstOccurrenceOf (s, sub, false) == j.fromFirstOccurrenceOf (js, false, false).toStdString());
            REQUIRE (text::fromFirstOccurrenceOf (s, sub, true)  == j.fromFirstOccurrenceOf (js, true,  false).toStdString());
            REQUIRE (text::fromLastOccurrenceOf  (s, sub, false) == j.fromLastOccurrenceOf  (js, false, false).toStdString());
        }

        for (int start : { -3, 0, 2, 100 })
        {
            REQUIRE (text::substring (s, start) == j.substring (start).toStdString());
            for (int end : { -1, 0, 3, 100 })
                REQUIRE (text::substring (s, start, end) == j.substring (start, end).toStdString());
        }

        for (int n : { 0, 2, 100 })
            REQUIRE (text::dropLastCharacters (s, n) == j.dropLastCharacters (n).toStdString());

        for (int len : { 0, 5, 10 })
            REQUIRE (text::paddedLeft (s, '0', len) == j.paddedLeft ('0', len).toStdString());

        REQUIRE (text::replace (s, "x", "Q") == j.replace ("x", "Q").toStdString());
        REQUIRE (text::replace (s, "::", "/") == j.replace ("::", "/").toStdString());
    }
}

TEST_CASE ("dusk::text::joinIntoString / split", "[foundation][text]")
{
    const std::vector<std::string> parts { "a", "b", "c" };
    REQUIRE (text::joinIntoString (parts, ", ") == "a, b, c");
    REQUIRE (text::joinIntoString ({}, ",") == "");

    REQUIRE (text::split ("a,b,c", ',') == std::vector<std::string> { "a", "b", "c" });
    REQUIRE (text::split ("", ',')      == std::vector<std::string> { "" });
    REQUIRE (text::split ("a,,c", ',')  == std::vector<std::string> { "a", "", "c" });
}
