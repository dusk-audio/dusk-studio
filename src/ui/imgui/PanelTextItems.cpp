#include "PanelTextItems.h"
#include "PanelFontFallback.h"
#include "PanelText.h"

#include <DearImGui/imgui_internal.h>

namespace duskstudio::imgui
{
bool textSelectable (const char* id, std::string_view text, bool selected)
{
    requestPanelGlyphs (text);
    const bool pressed = ImGui::Selectable (id, selected);
    const auto selectable = GImGui->LastItemData;
    ImGui::SameLine (0.0f, 0.0f);
    ImGui::TextUnformatted (text.data(), text.data() + text.size());
    GImGui->LastItemData = selectable;
    return pressed;
}

bool beginTextCombo (const char* id)
{
    return ImGui::BeginCombo (id, nullptr, ImGuiComboFlags_CustomPreview);
}

void comboPreviewText (std::string_view text)
{
    requestPanelGlyphs (text);
    if (! ImGui::BeginComboPreview()) return;
    const auto combo = GImGui->LastItemData;
    ImGui::TextUnformatted (text.data(), text.data() + text.size());
    ImGui::EndComboPreview();
    GImGui->LastItemData = combo;
}

void comboPreviewName (std::string_view utf8)
{
    if (! ImGui::BeginComboPreview()) return;
    const auto combo = GImGui->LastItemData;
    const auto text = panelText (utf8, ImGui::GetFont(), ImGui::GetFontSize(),
                                 GImGui->ComboPreviewData.PreviewRect.Max.x - ImGui::GetCursorScreenPos().x);
    ImGui::TextUnformatted (text.data(), text.data() + text.size());
    ImGui::EndComboPreview();
    GImGui->LastItemData = combo;
}
} // namespace duskstudio::imgui
