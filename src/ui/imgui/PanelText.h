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

// The full-width ASCII forms (U+FF01 to U+FF5E) and the ideographic space (U+3000)
// become their ASCII counterparts, which the bundled face has and the full-width forms
// are missing from. For text as drawn, and for a search matched against it; stored
// data, URLs and paths keep their bytes, so a link shows the target it opens. Every
// other byte, invalid UTF-8 included, passes through unchanged.
std::string displayText (std::string_view utf8);
} // namespace duskstudio::imgui
