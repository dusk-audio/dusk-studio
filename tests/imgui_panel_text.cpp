#include <catch2/catch_test_macros.hpp>

#include "ui/imgui/PanelText.h"

#include <src/Resources.hpp>

#include <string>

using duskstudio::imgui::buildPanelFonts;
using duskstudio::imgui::displayText;
using duskstudio::imgui::GlyphTally;
using duskstudio::imgui::nameFace;

namespace
{
DuskWidgets::Fonts bakePanelFonts (ImFontAtlas& atlas, float scale, bool replacementMark)
{
    const auto fonts = buildPanelFonts (atlas, daf_resources::dejavusans_ttf,
                                        static_cast<int> (daf_resources::dejavusans_ttf_size),
                                        scale, replacementMark);
    REQUIRE (atlas.Build());
    return fonts;
}
} // namespace

TEST_CASE ("Panel display text turns full-width ASCII forms into ASCII", "[imgui][sfz]")
{
    CHECK (displayText ("Bösendorfer Upright 120 Model（sfz）") == "Bösendorfer Upright 120 Model(sfz)");
    CHECK (displayText ("Model（sfz）") == "Model(sfz)");
    CHECK (displayText ("！～") == "!~");
    CHECK (displayText ("Ｐｉａｎｏ　Ｓｅｔ") == "Piano Set");
    CHECK (displayText ("Жук（ｖ２） אבג 日本") == "Жук(v2) אבג 日本");
    CHECK (displayText ("") == "");
    CHECK (displayText ("plain ASCII (sfz)") == "plain ASCII (sfz)");
}

TEST_CASE ("Panel display text leaves characters beside the full-width range alone", "[imgui][sfz]")
{
    const std::string neighbours[] = {
        "\xef\xbc\x80", // U+FF00, below the first full-width form
        "\xef\xbd\x9f", // U+FF5F, full-width white parenthesis
        "\xef\xbd\xa1", // U+FF61, half-width ideographic full stop
        "\xef\xbf\xbd", // U+FFFD
        "\xe3\x80\x81", // U+3001, ideographic comma
        "\xe2\xbf\xbf", // U+2FFF
    };
    for (const auto& text : neighbours)
        CHECK (displayText (text) == text);
}

TEST_CASE ("Panel display text passes invalid UTF-8 through byte for byte", "[imgui][sfz]")
{
    const std::string invalid[] = {
        "\xef\xbc",              // full-width form cut short at the end
        "\xef\xbc" "A",          // lead and one continuation, then ASCII
        "\xef" "A" "\x88",       // continuation missing in the middle
        "\x88\x89",              // stray continuations
        "\xe3\x80",              // ideographic space cut short
        std::string ("\xef\x00\x88", 3),
        "\xff\xfe(",
    };
    for (const auto& text : invalid)
        CHECK (displayText (text) == text);

    CHECK (displayText ("\xbc\xef\xbc\x88" "x") == "\xbc(x");
}

TEST_CASE ("A panel that marks missing glyphs draws them as the replacement mark", "[imgui][sfz]")
{
    for (const float scale : { 1.0f, 2.0f })
    {
        ImFontAtlas marked, plain;
        const auto markedFonts = bakePanelFonts (marked, scale, true);
        const auto plainFonts = bakePanelFonts (plain, scale, false);
        auto* const face = markedFonts.textEntry;
        REQUIRE (face != nullptr);

        CHECK (face->FindGlyphNoFallback (0x0416) != nullptr);
        CHECK (face->FindGlyphNoFallback (0x05d0) != nullptr);
        CHECK (face->FindGlyphNoFallback (0xfffd) != nullptr);
        CHECK (face->FallbackChar == 0xfffd);
        CHECK (face->FindGlyph (0x4e2d) == face->FindGlyphNoFallback (0xfffd));

        // The mark is the whole cost, and only the opting view pays it.
        CHECK (face->Glyphs.Size <= plainFonts.textEntry->Glyphs.Size + 1);
        CHECK (markedFonts.caption->Glyphs.Size == plainFonts.caption->Glyphs.Size);
        CHECK (markedFonts.valueLarge->Glyphs.Size == plainFonts.valueLarge->Glyphs.Size);
        CHECK (marked.Fonts.Size == plain.Fonts.Size);
    }
}

TEST_CASE ("Panels draw names in a face that has the scripts names use", "[imgui]")
{
    for (const float scale : { 1.0f, 2.0f })
    {
        ImFontAtlas atlas;
        const auto fonts = bakePanelFonts (atlas, scale, true);
        auto* const face = nameFace (fonts);
        REQUIRE (face != nullptr);

        CHECK (face->FindGlyphNoFallback (0x0416) != nullptr);
        CHECK (face->FindGlyphNoFallback (0x05d0) != nullptr);
        CHECK (face->FindGlyphNoFallback (0x03a9) != nullptr);
        CHECK (face->FallbackChar == 0xfffd);
        CHECK (face->FindGlyph (0x4e2d) == face->FindGlyphNoFallback (0xfffd));
    }
}

TEST_CASE ("A glyph tally counts the characters a face has no glyph for", "[imgui]")
{
    ImFontAtlas atlas;
    const auto fonts = bakePanelFonts (atlas, 1.0f, true);

    GlyphTally console;
    console.add (fonts.title, "Bass \xd0\x96\xd1\x83\xd0\xba");
    CHECK (console.drawn == 8);
    CHECK (console.missing == 3);

    GlyphTally names;
    names.add (nameFace (fonts), "Bass \xd0\x96\xd1\x83\xd0\xba \xd7\x90\xd7\x91");
    names.add (nameFace (fonts), "");
    CHECK (names.drawn == 11);
    CHECK (names.missing == 0);

    GlyphTally han;
    han.add (nameFace (fonts), "\xe4\xb8\xad");
    CHECK (han.drawn == 1);
    CHECK (han.missing == 1);
}
