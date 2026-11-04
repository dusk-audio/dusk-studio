#pragma once

#include <DuskWidgets.hpp>

#include <string_view>

namespace duskstudio::imgui
{
// Dear ImGui draws a widget label only up to "##". These give the widget a fixed ID and
// draw text from outside the app, such as names and paths, on its own and in full.

// A selectable showing `text`. It stays the last item, so the item queries and tooltip
// that follow see the selectable, as they would after ImGui::Selectable.
bool textSelectable (const char* id, std::string_view text, bool selected);

// A combo whose preview comboPreviewText draws.
bool beginTextCombo (const char* id);

// The preview of the combo beginTextCombo began, which stays the last item. Call it
// after that combo's EndCombo, or straight after beginTextCombo when it returned false.
void comboPreviewText (std::string_view text);
} // namespace duskstudio::imgui
