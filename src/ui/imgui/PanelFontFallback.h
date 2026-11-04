#pragma once

#include "CjkFont.h"

#include <DearImGui/imgui.h>

#include <set>
#include <string_view>

namespace duskstudio::imgui
{
class PanelFontFallback
{
public:
    explicit PanelFontFallback (std::filesystem::path fontFile = {});
    void request (std::string_view text);
    bool update (float pixels);
    void reserve (ImFontAtlas& atlas);
    void rasterise (ImFontAtlas& atlas, ImFont& font);

private:
    std::filesystem::path file;
    std::unique_ptr<CjkFont> source;
    std::set<unsigned int> pending, attempted;
    std::vector<CjkGlyph> glyphs;
    std::vector<int> rectangles;
    std::size_t alphaBytes = 0;
    float size = 0;
};

class ScopedPanelFontFallback
{
public:
    explicit ScopedPanelFontFallback (PanelFontFallback* fallback);
    ~ScopedPanelFontFallback();
    ScopedPanelFontFallback (const ScopedPanelFontFallback&) = delete;
    ScopedPanelFontFallback& operator= (const ScopedPanelFontFallback&) = delete;
private:
    PanelFontFallback* previous;
};

void requestPanelGlyphs (std::string_view text);
} // namespace duskstudio::imgui
