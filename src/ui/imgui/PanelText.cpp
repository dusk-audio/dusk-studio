#include "PanelText.h"
#include "PanelFontFallback.h"
#include "../../foundation/BidiText.h"

#include <DearImGui/imgui_internal.h>

#include <cfloat>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
const ImWchar* arabicFormRanges()
{
    static const std::vector<ImWchar> ranges = []
    {
        std::vector<ImWchar> pairs;
        for (const auto c : dusk::text::arabicPresentationForms())
        {
            if (! pairs.empty() && pairs.back() + 1 == static_cast<ImWchar> (c))
                pairs.back() = static_cast<ImWchar> (c);
            else
                pairs.insert (pairs.end(), { static_cast<ImWchar> (c), static_cast<ImWchar> (c) });
        }
        pairs.push_back (0);
        return pairs;
    }();
    return ranges.data();
}

float widthOf (ImFont* font, float size, std::string_view text)
{
    if (text.empty()) return 0.0f;
    return font->CalcTextSizeA (size, FLT_MAX, 0.0f, text.data(), text.data() + text.size()).x;
}
} // namespace

DuskWidgets::Fonts buildPanelFonts (ImFontAtlas& atlas, const void* ttfData, int ttfDataSize,
                                    float scale, bool replacementMark)
{
    auto fonts = DuskWidgets::buildFonts (atlas, ttfData, ttfDataSize, scale);

    // Dear ImGui counts a face's merged sources as one run directly after its own, so
    // they can join the text entry face only while that face is still the last one
    // added. Otherwise the face keeps '?' and draws Arabic unjoined.
    if (fonts.textEntry == nullptr || atlas.Sources.empty() || atlas.Sources.back().DstFont != fonts.textEntry)
        return fonts;

    const ImFontConfig face = atlas.Sources.back();
    const auto merge = [&] (const ImWchar* ranges)
    {
        ImFontConfig config;
        config.MergeMode = true;
        config.DstFont = fonts.textEntry;
        config.FontDataOwnedByAtlas = false;
        config.OversampleH = face.OversampleH;
        config.OversampleV = face.OversampleV;
        config.PixelSnapH = face.PixelSnapH;
        config.RasterizerDensity = face.RasterizerDensity;
        atlas.AddFontFromMemoryTTF (const_cast<void*> (ttfData), ttfDataSize, face.SizePixels, &config, ranges);
    };
    merge (arabicFormRanges());
    if (replacementMark)
    {
        static const ImWchar kReplacementMark[] = { 0xfffd, 0xfffd, 0 };
        merge (kReplacementMark);
    }
    return fonts;
}

ImFont* nameFace (const DuskWidgets::Fonts& fonts)
{
    return fonts.textEntry != nullptr ? fonts.textEntry : fonts.band;
}

std::string panelText (std::string_view utf8)
{
    auto text = displayText (utf8);
    if (dusk::text::needsDisplayOrder (text)) text = dusk::text::displayOrder (text);
    requestPanelGlyphs (text);
    return text;
}

std::string panelText (std::string_view utf8, ImFont* font, float size, float width, std::string_view ellipsis)
{
    auto typed = displayText (utf8);
    const bool reorder = dusk::text::needsDisplayOrder (typed);
    if (reorder) typed = dusk::text::shapeArabic (typed);
    auto text = reorder ? dusk::text::visualOrder (typed) : typed;
    // The fit is measured on the text as drawn. The same advances summed in typing
    // order can come out a bit wider, which would cut a name at the exact width a
    // caller measured from panelText's own output.
    if (font != nullptr && (reorder || ! ellipsis.empty()) && widthOf (font, size, text) > width)
    {
        const char* kept = typed.c_str();
        const float room = width - widthOf (font, size, ellipsis);
        if (room > 0.0f) font->CalcTextSizeA (size, room, 0.0f, typed.c_str(), typed.c_str() + typed.size(), &kept);
        typed.resize (static_cast<std::size_t> (kept - typed.c_str()));
        typed += ellipsis;
        text = reorder ? dusk::text::visualOrder (typed) : typed;
    }
    requestPanelGlyphs (text);
    return text;
}

std::string panelTextWrapped (std::string_view utf8, ImFont* font, float size, float wrapWidth)
{
    auto text = displayText (utf8);
    if (font == nullptr || wrapWidth <= 0.0f || ! dusk::text::needsDisplayOrder (text))
    {
        requestPanelGlyphs (text);
        return text;
    }
    text = dusk::text::shapeArabic (text);
    // The same breaks Dear ImGui's own wrapping makes, found in typing order so each
    // line holds the words that come next in reading order.
    std::vector<dusk::text::LineRange> lines;
    const char* const begin = text.c_str();
    const char* const end = begin + text.size();
    const float scale = size / font->FontSize;
    for (const char* paragraph = begin;;)
    {
        const char* paragraphEnd = paragraph;
        while (paragraphEnd < end && *paragraphEnd != '\n') ++paragraphEnd;
        const char* line = paragraph;
        do
        {
            const char* lineEnd = font->CalcWordWrapPositionA (scale, line, paragraphEnd, wrapWidth);
            while (lineEnd < paragraphEnd && (static_cast<unsigned char> (*lineEnd) & 0xc0) == 0x80) ++lineEnd;
            lines.push_back ({ static_cast<std::size_t> (line - begin), static_cast<std::size_t> (lineEnd - begin) });
            line = lineEnd;
            while (line < paragraphEnd && (*line == ' ' || *line == '\t')) ++line;
        } while (line < paragraphEnd);
        if (paragraphEnd == end) break;
        paragraph = paragraphEnd + 1;
    }
    text = dusk::text::visualOrder (text, lines);
    requestPanelGlyphs (text);
    return text;
}

std::string panelTextWrapped (std::string_view utf8)
{
    return panelTextWrapped (utf8, ImGui::GetFont(), ImGui::GetFontSize(),
                             ImGui::CalcWrapWidthForPos (ImGui::GetCursorScreenPos(), 0.0f));
}

std::string displayText (std::string_view utf8)
{
    std::string shown;
    shown.reserve (utf8.size());
    for (std::size_t i = 0; i < utf8.size(); ++i)
    {
        const auto lead = static_cast<unsigned char> (utf8[i]);
        if ((lead == 0xe3 || lead == 0xef) && i + 2 < utf8.size())
        {
            const auto second = static_cast<unsigned char> (utf8[i + 1]);
            const auto third = static_cast<unsigned char> (utf8[i + 2]);
            if ((second & 0xc0) == 0x80 && (third & 0xc0) == 0x80)
            {
                const unsigned codepoint = ((lead & 0x0fu) << 12) | ((second & 0x3fu) << 6) | (third & 0x3fu);
                if (codepoint >= 0xff01 && codepoint <= 0xff5e)
                {
                    shown += static_cast<char> (codepoint - 0xfee0);
                    i += 2;
                    continue;
                }
                if (codepoint == 0x3000)
                {
                    shown += ' ';
                    i += 2;
                    continue;
                }
            }
        }
        shown += utf8[i];
    }
    return shown;
}

void GlyphTally::add (ImFont* font, std::string_view utf8)
{
    if (! text.empty()) text += '\n';
    text.append (utf8);
    const char* at = utf8.data();
    const char* const end = at + utf8.size();
    while (at < end)
    {
        unsigned int codepoint = 0;
        at += ImTextCharFromUtf8 (&codepoint, at, end);
        ++drawn;
        if (font == nullptr || font->FindGlyphNoFallback (static_cast<ImWchar> (codepoint)) == nullptr)
            ++missing;
    }
}
} // namespace duskstudio::imgui
