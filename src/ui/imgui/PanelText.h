#pragma once

#include <DuskWidgets.hpp>

#include <string>
#include <string_view>

namespace duskstudio::imgui
{
// The panel window's faces from one TTF. The text entry face also bakes the Arabic
// presentation forms panelText joins letters with. With `replacementMark` it bakes
// U+FFFD too, which Dear ImGui then draws for a character the face lacks in place of
// '?'. A view showing text from outside the app asks for it; other panels keep '?'.
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

// displayText in display order: Arabic joined and each line reordered for right-to-left
// scripts, with glyph requests for the panel currently drawing. Search and persisted
// data use displayText or the original text instead.
std::string panelText (std::string_view utf8);

// panelText cut to `width` at `size` in `font`. The cut is made in typing order before
// reordering, so a right-to-left name keeps its beginning, and ends with `ellipsis`.
// Without an ellipsis, text that needs no reordering comes back whole, for the caller's
// clip rectangle to cut as before.
std::string panelText (std::string_view utf8, ImFont* font, float size, float width,
                       std::string_view ellipsis = {});

// panelText for text a panel wraps: broken into lines that fit `wrapWidth` in typing
// order, then each line reordered. Text that needs no reordering comes back unbroken.
std::string panelTextWrapped (std::string_view utf8, ImFont* font, float size, float wrapWidth);

// panelTextWrapped in the current font, at the width ImGui::TextWrapped would wrap to
// from the cursor.
std::string panelTextWrapped (std::string_view utf8);

// The characters a view drew in its names over one frame, how many of them the face
// had no glyph for, and the names as drawn, one per line, for a scenario to read.
struct GlyphTally
{
    int drawn = 0;
    int missing = 0;
    std::string text;

    void add (ImFont* font, std::string_view utf8);
};
} // namespace duskstudio::imgui
