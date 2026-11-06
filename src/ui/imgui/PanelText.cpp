#include "PanelText.h"
#include "PanelFontFallback.h"

#include <DearImGui/imgui_internal.h>

namespace duskstudio::imgui
{
DuskWidgets::Fonts buildPanelFonts (ImFontAtlas& atlas, const void* ttfData, int ttfDataSize,
                                    float scale, bool replacementMark)
{
    auto fonts = DuskWidgets::buildFonts (atlas, ttfData, ttfDataSize, scale);

    // Dear ImGui counts a face's merged sources as one run directly after its own, so
    // the mark can join the text entry face only while that face is still the last one
    // added. Otherwise the face keeps '?'.
    if (! replacementMark || fonts.textEntry == nullptr || atlas.Sources.empty()
        || atlas.Sources.back().DstFont != fonts.textEntry)
        return fonts;

    static const ImWchar kReplacementMark[] = { 0xfffd, 0xfffd, 0 };
    const ImFontConfig& face = atlas.Sources.back();
    const float size = face.SizePixels;
    ImFontConfig mark;
    mark.MergeMode = true;
    mark.DstFont = fonts.textEntry;
    mark.FontDataOwnedByAtlas = false;
    mark.OversampleH = face.OversampleH;
    mark.OversampleV = face.OversampleV;
    mark.PixelSnapH = face.PixelSnapH;
    mark.RasterizerDensity = face.RasterizerDensity;
    atlas.AddFontFromMemoryTTF (const_cast<void*> (ttfData), ttfDataSize, size, &mark, kReplacementMark);
    return fonts;
}

ImFont* nameFace (const DuskWidgets::Fonts& fonts)
{
    return fonts.textEntry != nullptr ? fonts.textEntry : fonts.band;
}

std::string panelText (std::string_view utf8)
{
    auto text = displayText (utf8);
    requestPanelGlyphs (text);
    return text;
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
