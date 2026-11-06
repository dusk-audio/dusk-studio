#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/imgui/PanelText.h"
#include "ui/imgui/PanelFontFallback.h"

#include <src/Resources.hpp>

#include <string>
#include <algorithm>
#include <cmath>

using duskstudio::imgui::PanelFontFallback;

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

std::vector<unsigned char> glyphPixels (const ImFontAtlas& atlas, ImFont& font, ImWchar character)
{
    const auto* glyph = font.FindGlyphNoFallback (character);
    REQUIRE (glyph != nullptr);
    std::vector<unsigned char> pixels;
    for (int y = static_cast<int> (std::round (glyph->V0 * atlas.TexHeight));
         y < static_cast<int> (std::round (glyph->V1 * atlas.TexHeight)); ++y)
        for (int x = static_cast<int> (std::round (glyph->U0 * atlas.TexWidth));
             x < static_cast<int> (std::round (glyph->U1 * atlas.TexWidth)); ++x)
            pixels.push_back (atlas.TexPixelsAlpha8[y * atlas.TexWidth + x]);
    return pixels;
}

std::string hanRange (unsigned int first, unsigned int last)
{
    std::string text;
    for (auto c = first; c <= last; ++c)
    {
        text += static_cast<char> (0xe0 | (c >> 12));
        text += static_cast<char> (0x80 | ((c >> 6) & 0x3f));
        text += static_cast<char> (0x80 | (c & 0x3f));
    }
    return text;
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

TEST_CASE ("CJK fallback rasterizes static and variable system font formats", "[imgui][cjk]")
{
    for (const char* file : { "DuskCjkTest.otf"
                             #if defined (__linux__)
                             , "DuskCjkVariableTest.otf"
                             #endif
                           })
    {
        duskstudio::imgui::CjkFont source (std::filesystem::path (DUSKSTUDIO_SOURCE_DIR) / "tests/fixtures/fonts" / file);
        for (const float size : { 13.0f, 26.0f })
            for (const auto c : { 0x65e5u, 0x4e2du, 0x3042u, 0x30a2u, 0xd55cu })
            {
                duskstudio::imgui::CjkGlyph glyph;
                CAPTURE (file, size, c);
                REQUIRE (source.rasterize (c, size, glyph));
                CHECK (glyph.width > 0);
                CHECK (glyph.height > 0);
                CHECK (glyph.advance > 0);
                CHECK (std::any_of (glyph.alpha.begin(), glyph.alpha.end(), [] (auto alpha) { return alpha != 0; }));
            }
        duskstudio::imgui::CjkGlyph absent;
        CHECK_FALSE (source.rasterize (0x65e6, 13, absent));
    }
}

TEST_CASE ("Panel CJK glyphs appear between frames without changing the Latin face", "[imgui][cjk]")
{
    PanelFontFallback fallback (std::filesystem::path (DUSKSTUDIO_SOURCE_DIR) / "tests/fixtures/fonts/DuskCjkTest.otf");
    for (const float scale : { 1.0f, 2.0f })
    {
        fallback.request ("日本語 中 あア 한");
        REQUIRE (fallback.update (13 * scale));
        ImFontAtlas atlas, baseline;
        const auto plain = bakePanelFonts (baseline, scale, true);
        const auto fonts = buildPanelFonts (atlas, daf_resources::dejavusans_ttf,
                                            static_cast<int> (daf_resources::dejavusans_ttf_size), scale, true);
        fallback.reserve (atlas);
        REQUIRE (atlas.Build());
        fallback.rasterise (atlas, *fonts.textEntry);
        for (auto c : { 0x65e5, 0x4e2d, 0x3042, 0x30a2, 0xd55c })
            REQUIRE (fonts.textEntry->FindGlyphNoFallback (static_cast<ImWchar> (c)) != nullptr);
        CHECK (fonts.textEntry->FallbackChar == 0xfffd);
        CHECK (fonts.textEntry->FindGlyph (0x8a9e) == fonts.textEntry->FindGlyphNoFallback (0xfffd));
        CHECK_THAT (fonts.textEntry->GetCharAdvance ('A'),
                    Catch::Matchers::WithinAbs (plain.textEntry->GetCharAdvance ('A'), 1.0e-6f));
        CHECK (glyphPixels (atlas, *fonts.textEntry, 'A') == glyphPixels (baseline, *plain.textEntry, 'A'));
        CHECK (fonts.title->Glyphs.Size == plain.title->Glyphs.Size);
        CHECK (atlas.TexWidth * atlas.TexHeight <= baseline.TexWidth * baseline.TexHeight * 2);
        fallback.request ("日本語 中 あア 한");
        CHECK_FALSE (fallback.update (13 * scale));
    }
}

TEST_CASE ("CJK fallback supplies all notepad font faces from one atlas", "[imgui][cjk][notepad]")
{
    PanelFontFallback fallback (std::filesystem::path (DUSKSTUDIO_SOURCE_DIR) / "tests/fixtures/fonts/DuskCjkTest.otf");
    fallback.request ("日本 中 あア 한");
    REQUIRE (fallback.update (16));
    ImFontAtlas atlas;
    ImFontConfig config;
    config.FontDataOwnedByAtlas = false;
    std::vector<ImFont*> faces;
    for (int i = 0; i < 5; ++i)
        faces.push_back (atlas.AddFontFromMemoryTTF ((void*) daf_resources::dejavusans_ttf,
            static_cast<int> (daf_resources::dejavusans_ttf_size), 16, &config));
    fallback.reserve (atlas);
    REQUIRE (atlas.Build());
    const auto pixels = glyphPixels (atlas, *faces.front(), 'A');
    const auto advance = faces.front()->GetCharAdvance ('A');
    const auto rectangles = atlas.CustomRects.Size;
    for (auto* face : faces)
    {
        fallback.rasterise (atlas, *face);
        for (auto c : { 0x65e5, 0x4e2d, 0x3042, 0x30a2, 0xd55c })
        {
            REQUIRE (face->FindGlyphNoFallback (static_cast<ImWchar> (c)) != nullptr);
            CHECK (glyphPixels (atlas, *face, static_cast<ImWchar> (c))
                   == glyphPixels (atlas, *faces.front(), static_cast<ImWchar> (c)));
        }
        CHECK (glyphPixels (atlas, *face, 'A') == pixels);
        CHECK_THAT (face->GetCharAdvance ('A'), Catch::Matchers::WithinAbs (advance, 1.0e-6f));
        const auto count = face->Glyphs.Size;
        fallback.rasterise (atlas, *face);
        CHECK (face->Glyphs.Size == count);
    }
    CHECK (atlas.CustomRects.Size == rectangles);
}

TEST_CASE ("Missing CJK fonts preserve replacement marks without repeated rebuilds", "[imgui][cjk]")
{
    PanelFontFallback fallback (std::filesystem::path (DUSKSTUDIO_SOURCE_DIR) / "tests/fixtures/fonts/absent.otf");
    fallback.request ("日本 한");
    REQUIRE_FALSE (fallback.update (13));
    fallback.request ("日本 한");
    REQUIRE_FALSE (fallback.update (13));
    ImFontAtlas atlas;
    auto fonts = buildPanelFonts (atlas, daf_resources::dejavusans_ttf,
                                  static_cast<int> (daf_resources::dejavusans_ttf_size), 1, true);
    fallback.reserve (atlas);
    REQUIRE (atlas.Build());
    fallback.rasterise (atlas, *fonts.textEntry);
    CHECK (fonts.textEntry->FindGlyph (0x65e5) == fonts.textEntry->FindGlyphNoFallback (0xfffd));
}

TEST_CASE ("Panel CJK cache stays bounded and replaces old browsing glyphs", "[imgui][cjk]")
{
    PanelFontFallback fallback (std::filesystem::path (DUSKSTUDIO_SOURCE_DIR) / "tests/fixtures/fonts/DuskCjkTest.otf");
    fallback.request (hanRange (0x4e00, 0x5e00));
    SECTION ("Character capacity retains the latest requested names")
    {
        REQUIRE (fallback.update (13));
        ImFontAtlas first;
        fallback.reserve (first);
        REQUIRE (first.CustomRects.Size == 4096);

        fallback.request (hanRange (0x5dff, 0x5e00));
        REQUIRE (fallback.update (13));
        ImFontAtlas next;
        const auto fonts = buildPanelFonts (next, daf_resources::dejavusans_ttf,
                                            static_cast<int> (daf_resources::dejavusans_ttf_size), 1, true);
        fallback.reserve (next);
        REQUIRE (next.Build());
        fallback.rasterise (next, *fonts.textEntry);
        CHECK (fonts.textEntry->FindGlyphNoFallback (0x4e00) == nullptr);
        CHECK (fonts.textEntry->FindGlyphNoFallback (0x5dff) != nullptr);
        CHECK (fonts.textEntry->FindGlyphNoFallback (0x5e00) != nullptr);
        fallback.request (hanRange (0x5dff, 0x5e00));
        CHECK_FALSE (fallback.update (13));
    }
    SECTION ("Large glyphs also obey the bitmap memory limit")
    {
        REQUIRE (fallback.update (128));
        ImFontAtlas atlas;
        fallback.reserve (atlas);
        REQUIRE (atlas.CustomRects.Size > 0);
        CHECK (atlas.CustomRects.Size < 4096);
        std::size_t bytes = 0;
        for (const auto& rectangle : atlas.CustomRects)
            bytes += static_cast<std::size_t> (rectangle.Width) * rectangle.Height;
        CHECK (bytes <= 4 * 1024 * 1024);
        fallback.request (hanRange (0x4e00, 0x5dff));
        CHECK_FALSE (fallback.update (128));
    }
}

TEST_CASE ("Panel CJK requests stay with the drawing panel and leave search text pure", "[imgui][cjk]")
{
    const auto file = std::filesystem::path (DUSKSTUDIO_SOURCE_DIR) / "tests/fixtures/fonts/DuskCjkTest.otf";
    PanelFontFallback outer (file), inner (file);
    using duskstudio::imgui::panelText;
    using duskstudio::imgui::ScopedPanelFontFallback;
    {
        const ScopedPanelFontFallback scope (&outer);
        CHECK (displayText ("日本（ｖ２）") == "日本(v2)");
        CHECK_FALSE (outer.update (13));
        {
            const ScopedPanelFontFallback nested (&inner);
            CHECK (panelText ("あ") == "あ");
        }
        CHECK (panelText ("日（ｖ２）") == "日(v2)");
    }
    CHECK (panelText ("한") == "한");
    REQUIRE (outer.update (13));
    REQUIRE (inner.update (13));
    for (auto* fallback : { &outer, &inner })
    {
        ImFontAtlas atlas;
        const auto fonts = buildPanelFonts (atlas, daf_resources::dejavusans_ttf,
                                            static_cast<int> (daf_resources::dejavusans_ttf_size), 1, true);
        fallback->reserve (atlas);
        REQUIRE (atlas.Build());
        fallback->rasterise (atlas, *fonts.textEntry);
        CHECK ((fonts.textEntry->FindGlyphNoFallback (0x65e5) != nullptr) == (fallback == &outer));
        CHECK ((fonts.textEntry->FindGlyphNoFallback (0x3042) != nullptr) == (fallback == &inner));
        CHECK (fonts.textEntry->FindGlyphNoFallback (0xd55c) == nullptr);
    }
}
