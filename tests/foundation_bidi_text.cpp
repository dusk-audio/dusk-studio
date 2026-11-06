#include <catch2/catch_test_macros.hpp>

#include "foundation/BidiText.h"

#include <DuskSheenBidi.h>

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

using namespace dusk;

namespace
{
std::string utf8 (std::u32string_view codepoints)
{
    std::string out;
    for (const auto c : codepoints)
    {
        if (c < 0x80)
            out += static_cast<char> (c);
        else if (c < 0x800)
        {
            out += static_cast<char> (0xc0 | (c >> 6));
            out += static_cast<char> (0x80 | (c & 0x3f));
        }
        else
        {
            out += static_cast<char> (0xe0 | (c >> 12));
            out += static_cast<char> (0x80 | ((c >> 6) & 0x3f));
            out += static_cast<char> (0x80 | (c & 0x3f));
        }
    }
    return out;
}

// The levels SheenBidi resolves for each code point of a single-paragraph line, and
// that line put in display order by rule L2 alone, reversing every run at or above
// each level from the highest down to the lowest odd one.
struct Reference
{
    std::vector<SBLevel> levels;
    std::u32string visual;
};

Reference reference (std::u32string_view text)
{
    const auto bytes = utf8 (text);
    SBCodepointSequence sequence { SBStringEncodingUTF8, bytes.data(), bytes.size() };
    const auto algorithm = SBAlgorithmCreate (&sequence);
    const auto paragraph = SBAlgorithmCreateParagraph (algorithm, 0, bytes.size(), SBLevelDefaultLTR);
    const auto line = SBParagraphCreateLine (paragraph, 0, bytes.size());
    std::vector<SBLevel> byteLevels (bytes.size());
    const auto* runs = SBLineGetRunsPtr (line);
    for (SBUInteger r = 0; r < SBLineGetRunCount (line); ++r)
        std::fill_n (byteLevels.begin() + static_cast<std::ptrdiff_t> (runs[r].offset), runs[r].length, runs[r].level);
    SBLineRelease (line);
    SBParagraphRelease (paragraph);
    SBAlgorithmRelease (algorithm);

    Reference result;
    std::size_t at = 0;
    for (const auto c : text)
    {
        result.levels.push_back (byteLevels[at]);
        at += utf8 (std::u32string (1, c)).size();
    }
    result.visual = std::u32string (text);
    const auto highest = *std::max_element (result.levels.begin(), result.levels.end());
    auto levels = result.levels;
    for (int level = highest; level >= 1; --level)
        for (std::size_t i = 0; i < levels.size();)
        {
            if (levels[i] < level) { ++i; continue; }
            auto end = i;
            while (end < levels.size() && levels[end] >= level) ++end;
            std::reverse (result.visual.begin() + static_cast<std::ptrdiff_t> (i), result.visual.begin() + static_cast<std::ptrdiff_t> (end));
            std::reverse (levels.begin() + static_cast<std::ptrdiff_t> (i), levels.begin() + static_cast<std::ptrdiff_t> (end));
            i = end;
        }
    return result;
}
} // namespace

TEST_CASE ("A Hebrew word is drawn reversed", "[foundation][bidi]")
{
    CHECK (text::needsDisplayOrder (utf8 (U"שלום")));
    CHECK (text::displayOrder (utf8 (U"שלום")) == utf8 (U"םולש"));
}

TEST_CASE ("A Latin name with a Hebrew word and digits keeps Latin first", "[foundation][bidi]")
{
    CHECK (text::displayOrder (utf8 (U"Piano שלום 12")) == utf8 (U"Piano 12 םולש"));
}

TEST_CASE ("Digits inside right-to-left text read left to right", "[foundation][bidi]")
{
    CHECK (text::displayOrder (utf8 (U"שיר 2024 חדש"))
           == utf8 (U"שדח 2024 ריש"));
    CHECK (text::displayOrder (utf8 (U"مسار ١٢"))
           == utf8 (U"١٢ ﺭﺎﺴﻣ"));
}

TEST_CASE ("Brackets in a right-to-left run are mirrored", "[foundation][bidi]")
{
    CHECK (text::displayOrder (utf8 (U"(שלום)")) == utf8 (U"(םולש)"));
    CHECK (text::displayOrder (utf8 (U"א [ב] <ג")) == utf8 (U"ג> [ב] א"));
    CHECK (text::displayOrder (utf8 (U"Mix (שלום)")) == utf8 (U"Mix (םולש)"));
}

TEST_CASE ("Arabic letters join as presentation forms", "[foundation][bidi]")
{
    SECTION ("kitab: initial, medial, final, then isolated after a right-joining alef")
    {
        CHECK (text::shapeArabic (utf8 (U"كتاب")) == utf8 (U"ﻛﺘﺎﺏ"));
        CHECK (text::displayOrder (utf8 (U"كتاب")) == utf8 (U"ﺏﺎﺘﻛ"));
    }
    SECTION ("muhammad: two medial forms")
    {
        CHECK (text::shapeArabic (utf8 (U"محمد")) == utf8 (U"ﻣﺤﻤﺪ"));
    }
    SECTION ("a letter on its own is isolated, a space breaks joining")
    {
        CHECK (text::shapeArabic (utf8 (U"ب ب")) == utf8 (U"ﺏ ﺏ"));
    }
    SECTION ("tatweel joins on both sides and stays")
    {
        CHECK (text::shapeArabic (utf8 (U"بـب")) == utf8 (U"ﺑـﺐ"));
    }
    SECTION ("a letter without a drawable form passes through and does not join")
    {
        CHECK (text::shapeArabic (utf8 (U"ہب")) == utf8 (U"ہﺏ"));
        CHECK (text::shapeArabic (utf8 (U"بںب")) == utf8 (U"ﺑﮟﺏ"));
    }
}

TEST_CASE ("Lam followed by alef draws as one ligature", "[foundation][bidi]")
{
    CHECK (text::shapeArabic (utf8 (U"لا")) == utf8 (U"ﻻ"));
    CHECK (text::shapeArabic (utf8 (U"سلام")) == utf8 (U"ﺳﻼﻡ"));
    CHECK (text::displayOrder (utf8 (U"سلام")) == utf8 (U"ﻡﻼﺳ"));
    CHECK (text::shapeArabic (utf8 (U"لآ لأ لإ")) == utf8 (U"ﻵ ﻷ ﻹ"));
    CHECK (text::shapeArabic (utf8 (U"بلإ")) == utf8 (U"ﺑﻺ"));
}

TEST_CASE ("Arabic marks do not break joining and stay on their letter", "[foundation][bidi]")
{
    CHECK (text::shapeArabic (utf8 (U"بَب")) == utf8 (U"ﺑَﺐ"));
    CHECK (text::displayOrder (utf8 (U"بَب")) == utf8 (U"ﺐﺑَ"));
    CHECK (text::shapeArabic (utf8 (U"مُحَمَّد")) == utf8 (U"ﻣُﺤَﻤَّﺪ"));
    CHECK (text::shapeArabic (utf8 (U"لّا")) == utf8 (U"ﻻّ"));
    CHECK (text::shapeArabic (utf8 (U"بٰب")) == utf8 (U"ﺑٰﺐ"));
}

TEST_CASE ("Persian and Urdu letters join with their Forms-A shapes", "[foundation][bidi]")
{
    CHECK (text::shapeArabic (utf8 (U"پدر")) == utf8 (U"ﭘﺪﺭ"));
    CHECK (text::shapeArabic (utf8 (U"گچ")) == utf8 (U"ﮔﭻ"));
    CHECK (text::shapeArabic (utf8 (U"ژ")) == utf8 (U"ﮊ"));
    CHECK (text::shapeArabic (utf8 (U"کیک")) == utf8 (U"ﮐﯿﮏ"));
    CHECK (text::shapeArabic (utf8 (U"بىب")) == utf8 (U"ﺑﯩﺐ"));
}

TEST_CASE ("Text without right-to-left characters comes back unchanged", "[foundation][bidi]")
{
    for (const auto* name : { "", "Kick In", "Lead Vox (take 3) [comp]", "Caf\xc3\xa9 \xd0\xa1\xd0\xb5\xd1\x81\xd1\x81\xd0\xb8\xd1\x8f",
                              "\xe6\x97\xa5\xe6\x9c\xac\xe8\xaa\x9e" })
    {
        CHECK_FALSE (text::needsDisplayOrder (name));
        CHECK (text::displayOrder (name) == name);
        CHECK (text::shapeArabic (name) == name);
        CHECK (text::visualOrder (name) == name);
    }
}

TEST_CASE ("Invalid UTF-8 passes through byte for byte", "[foundation][bidi]")
{
    for (const std::string& bad : { std::string ("\xd7\x90\xff"), std::string ("\xd8\xa8\xd8"), std::string ("\xd7\x90\xc0\xaf"),
                                   std::string ("\xd8\xa8\xed\xa0\x80\xd8\xa8"), std::string ("\xf8\x88\x80\x80\x80") })
    {
        CHECK_FALSE (text::needsDisplayOrder (bad));
        CHECK (text::displayOrder (bad) == bad);
        CHECK (text::shapeArabic (bad) == bad);
        CHECK (text::visualOrder (bad) == bad);
    }
}

TEST_CASE ("Each line of a broken paragraph is reordered on its own", "[foundation][bidi]")
{
    const auto paragraph = utf8 (U"אבג דהו");
    const auto space = paragraph.find (' ');
    CHECK (text::visualOrder (paragraph, { { 0, space }, { space + 1, paragraph.size() } })
           == utf8 (U"גבא\nוהד"));
    CHECK (text::visualOrder (utf8 (U"אב\nAB גד")) == utf8 (U"בא\nAB דג"));
}

TEST_CASE ("Display order matches SheenBidi's own levels", "[foundation][bidi]")
{
    SECTION ("a Latin paragraph with a Hebrew word and a number")
    {
        const std::u32string text = U"Piano שלום 12";
        const auto expected = reference (text);
        CHECK (expected.levels == std::vector<SBLevel> { 0, 0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 2, 2 });
        CHECK (text::visualOrder (utf8 (text)) == utf8 (expected.visual));
    }
    SECTION ("a Hebrew paragraph with an embedded Latin phrase")
    {
        const std::u32string text = U"שיר Kick In חדש";
        const auto expected = reference (text);
        CHECK (expected.levels.front() == 1);
        CHECK (expected.levels[4] == 2);
        CHECK (text::visualOrder (utf8 (text)) == utf8 (expected.visual));
        CHECK (expected.visual == U"שדח Kick In ריש");
    }
    SECTION ("Arabic letters with Arabic-Indic and European digits")
    {
        const std::u32string text = U"ﻣﺴﺎﺭ ١٢ - 34";
        const auto expected = reference (text);
        CHECK (expected.levels.front() == 1);
        CHECK (text::visualOrder (utf8 (text)) == utf8 (expected.visual));
    }
}

TEST_CASE ("Every presentation form the shaper emits is listed for baking", "[foundation][bidi]")
{
    const auto& forms = text::arabicPresentationForms();
    REQUIRE (std::is_sorted (forms.begin(), forms.end()));
    CHECK (std::adjacent_find (forms.begin(), forms.end()) == forms.end());
    for (const auto c : forms)
        CHECK (((c >= 0xfb50 && c <= 0xfdff) || (c >= 0xfe70 && c <= 0xfeff)));
    for (const char32_t c : { U'ﻻ', U'ﻼ', U'ﭘ', U'ﯩ', U'ﯿ', U'ﺀ', U'ﻴ' })
        CHECK (std::binary_search (forms.begin(), forms.end(), c));

    std::u32string everyLetter;
    for (char32_t c = 0x0621; c <= 0x06ff; ++c)
        everyLetter += { U'ـ', c, U'ـ', U' ', c, U'ـ', U' ', U'ـ', c, U' ', c, U' ' };
    const auto shaped = text::shapeArabic (utf8 (everyLetter));
    for (std::size_t at = 0; at < shaped.size();)
    {
        const auto lead = static_cast<unsigned char> (shaped[at]);
        const std::size_t length = lead < 0x80 ? 1 : lead < 0xe0 ? 2 : 3;
        char32_t c = length == 1 ? lead : length == 2 ? (lead & 0x1fu) : (lead & 0x0fu);
        for (std::size_t i = 1; i < length; ++i) c = (c << 6) | (static_cast<unsigned char> (shaped[at + i]) & 0x3fu);
        if (c >= 0xfb50) CHECK (std::binary_search (forms.begin(), forms.end(), c));
        at += length;
    }
}
