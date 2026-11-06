#pragma once

#include <DuskWidgets.hpp>

#include <string>
#include <string_view>

namespace duskstudio::imgui
{
// The panel window's faces from one TTF. With `replacementMark`, the text entry face
// also bakes U+FFFD, which Dear ImGui then draws for a character the face lacks in
// place of '?'. A view showing text from outside the app asks for it; other panels
// keep the atlas they had.
DuskWidgets::Fonts buildPanelFonts (ImFontAtlas& atlas, const void* ttfData, int ttfDataSize,
                                    float scale, bool replacementMark);

// The face for a name a user typed or a device reported: track, region, take, session
// and device names. The console faces bake Latin-1 alone; the text entry face bakes the
// scripts a name may use and is the face the name was typed in. A face of its own at
// each size a panel draws names would double the atlas, so names draw from this one at
// those sizes. The band face when there is no text entry face.
ImFont* nameFace (const DuskWidgets::Fonts& fonts);

// The full-width ASCII forms (U+FF01 to U+FF5E) and the ideographic space (U+3000)
// become their ASCII counterparts, which the bundled face has and the full-width forms
// are missing from. For text as drawn, and for a search matched against it; stored
// data, URLs and paths keep their bytes, so a link shows the target it opens. Every
// other byte, invalid UTF-8 included, passes through unchanged.
std::string displayText (std::string_view utf8);

// Display normalization plus glyph requests for the panel currently drawing.
// Search and persisted data use displayText or the original text instead.
std::string panelText (std::string_view utf8);

// The characters a view drew in its names over one frame, and how many of them the
// face had no glyph for, for a scenario to read.
struct GlyphTally
{
    int drawn = 0;
    int missing = 0;

    void add (ImFont* font, std::string_view utf8);
};
} // namespace duskstudio::imgui
