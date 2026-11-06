#include "PanelFontFallback.h"

#include <DearImGui/imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
thread_local PanelFontFallback* activeFallback = nullptr;
constexpr std::size_t kMaxGlyphs = 4096;
constexpr std::size_t kMaxAlphaBytes = 4 * 1024 * 1024;
}

PanelFontFallback::PanelFontFallback (std::filesystem::path fontFile) : file (std::move (fontFile)) {}

void PanelFontFallback::request (std::string_view text)
{
    if (text.empty()) return;
    const char* at = text.data();
    const char* const end = at + text.size();
    while (at < end && pending.size() < kMaxGlyphs)
    {
        unsigned int codepoint = 0;
        at += ImTextCharFromUtf8 (&codepoint, at, end);
        if (isCjkCodepoint (codepoint)) pending.insert (codepoint);
    }
}

bool PanelFontFallback::update (float pixels)
{
    if (! std::isfinite (pixels) || pixels < 1 || pixels > 128) return false;
    bool changed = false;
    const bool resized = std::abs (pixels - size) > 0.01f;
    const auto additions = static_cast<std::size_t> (std::count_if (pending.begin(), pending.end(),
        [this] (auto c) { return attempted.count (c) == 0; }));
    if (resized || attempted.size() + additions > kMaxGlyphs)
    {
        // Keep the characters visible in the latest frame when an old browsing
        // session fills the cache. A scale change also needs fresh bitmaps.
        if (resized)
            for (auto c : attempted)
                if (pending.size() < kMaxGlyphs) pending.insert (c);
        changed = ! glyphs.empty();
        glyphs.clear();
        attempted.clear();
        alphaBytes = 0;
        size = pixels;
    }
    for (auto codepoint : pending)
    {
        if (! attempted.insert (codepoint).second) continue;
        if (source == nullptr) source = std::make_unique<CjkFont> (file);
        CjkGlyph glyph;
        if (! source->rasterize (codepoint, pixels, glyph)
            || glyph.alpha.size() > kMaxAlphaBytes - alphaBytes) continue;
        alphaBytes += glyph.alpha.size();
        glyphs.push_back (std::move (glyph));
        changed = true;
    }
    pending.clear();
    return changed;
}

void PanelFontFallback::reserve (ImFontAtlas& atlas)
{
    rectangles.clear();
    for (const auto& glyph : glyphs)
        rectangles.push_back (atlas.AddCustomRectRegular (std::max (1, glyph.width), std::max (1, glyph.height)));
}

void PanelFontFallback::rasterise (ImFontAtlas& atlas, ImFont& font)
{
    for (std::size_t i = 0; i < glyphs.size(); ++i)
    {
        const auto& glyph = glyphs[i];
        const auto* rect = atlas.GetCustomRectByIndex (rectangles[i]);
        if (! rect->IsPacked() || font.FindGlyphNoFallback (static_cast<ImWchar> (glyph.codepoint)) != nullptr) continue;
        for (int y = 0; y < glyph.height; ++y)
            for (int x = 0; x < glyph.width; ++x)
            {
                const auto alpha = glyph.alpha[static_cast<std::size_t> (y * glyph.width + x)];
                const auto index = static_cast<std::size_t> ((rect->Y + y) * atlas.TexWidth + rect->X + x);
                if (atlas.TexPixelsAlpha8 != nullptr) atlas.TexPixelsAlpha8[index] = alpha;
                if (atlas.TexPixelsRGBA32 != nullptr) atlas.TexPixelsRGBA32[index] = IM_COL32 (255, 255, 255, alpha);
            }
        ImVec2 uv0, uv1;
        atlas.CalcCustomRectUV (rect, &uv0, &uv1);
        const float left = static_cast<float> (glyph.left);
        const float top = std::round (font.Ascent) + static_cast<float> (glyph.top);
        font.AddGlyph (nullptr, static_cast<ImWchar> (glyph.codepoint), left, top,
                       left + static_cast<float> (glyph.width), top + static_cast<float> (glyph.height),
                       uv0.x, uv0.y, uv0.x + static_cast<float> (glyph.width) / static_cast<float> (atlas.TexWidth),
                       uv0.y + static_cast<float> (glyph.height) / static_cast<float> (atlas.TexHeight), glyph.advance);
    }
    font.BuildLookupTable();
}

ScopedPanelFontFallback::ScopedPanelFontFallback (PanelFontFallback* fallback)
    : previous (std::exchange (activeFallback, fallback)) {}
ScopedPanelFontFallback::~ScopedPanelFontFallback() { activeFallback = previous; }

void requestPanelGlyphs (std::string_view text)
{
    if (activeFallback != nullptr) activeFallback->request (text);
}
} // namespace duskstudio::imgui
