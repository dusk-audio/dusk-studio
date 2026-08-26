#include "PanelControls.h"

#include <algorithm>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// The settings chrome, in the framework's colour order. The field colours are the ones
// JUCE's dark scheme painted these controls with, written out here so the port lands on
// the same picture rather than on a redesign; the rule and the heading are
// AudioSettingsPanel::paint()'s own, and the selection is the one the native startup
// panel already highlights a row with.
constexpr unsigned int kFieldFill = 0x263238ff;
constexpr unsigned int kFieldOutline = 0x8e989bff;
constexpr unsigned int kSelection = 0x305a82ff;
constexpr unsigned int kRule = 0x2a2a32ff;
constexpr unsigned int kHeadingText = 0xe0e0e6ff;

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

ImVec4 asVec4 (ImU32 colour)
{
    return ImGui::ColorConvertU32ToFloat4 (colour);
}

ImFont* formFont (const dw::Context& ctx)
{
    return ctx.fonts != nullptr && ctx.fonts->band != nullptr ? ctx.fonts->band
                                                             : ImGui::GetFont();
}

// Dear ImGui derives a control's height from the font plus its frame padding, while a
// panel row states the height it wants. This turns one into the other.
struct ScopedRowHeight
{
    ScopedRowHeight (const dw::Context& ctx, float height)
    {
        const float fontSize = formFont (ctx)->FontSize;
        ImGui::PushStyleVar (ImGuiStyleVar_FramePadding,
                             ImVec2 (ctx.s (6.0f),
                                     std::max (0.0f, (height - fontSize) * 0.5f)));
    }
    ~ScopedRowHeight() { ImGui::PopStyleVar(); }
};
} // namespace

ScopedFormStyle::ScopedFormStyle (const dw::Context& ctx)
{
    const auto& theme = *ctx.theme;
    const auto colour = [this] (ImGuiCol which, ImU32 value)
    {
        ImGui::PushStyleColor (which, asVec4 (value));
        ++colours;
    };
    const auto var = [this] (ImGuiStyleVar which, float value)
    {
        ImGui::PushStyleVar (which, value);
        ++vars;
    };

    const ImU32 field = rgba (kFieldFill);
    colour (ImGuiCol_Text, theme.textBright);
    colour (ImGuiCol_TextDisabled, theme.textBypassed);
    colour (ImGuiCol_FrameBg, field);
    colour (ImGuiCol_FrameBgHovered, dw::brighter (field, 0.25f));
    colour (ImGuiCol_FrameBgActive, dw::brighter (field, 0.4f));
    colour (ImGuiCol_Border, rgba (kFieldOutline));
    colour (ImGuiCol_BorderShadow, IM_COL32 (0, 0, 0, 0));
    colour (ImGuiCol_Button, field);
    colour (ImGuiCol_ButtonHovered, dw::brighter (field, 0.25f));
    colour (ImGuiCol_ButtonActive, dw::brighter (field, 0.4f));
    colour (ImGuiCol_Header, rgba (kSelection));
    colour (ImGuiCol_HeaderHovered, dw::brighter (rgba (kSelection), 0.2f));
    colour (ImGuiCol_HeaderActive, rgba (kSelection));
    colour (ImGuiCol_PopupBg, dw::darker (field, 0.2f));
    colour (ImGuiCol_CheckMark, theme.textBright);
    colour (ImGuiCol_SliderGrab, theme.capMid);
    colour (ImGuiCol_SliderGrabActive, theme.capTop);
    colour (ImGuiCol_ScrollbarBg, IM_COL32 (0, 0, 0, 0));
    colour (ImGuiCol_ScrollbarGrab, theme.pillDivider);
    colour (ImGuiCol_ScrollbarGrabHovered, dw::brighter (theme.pillDivider, 0.25f));
    colour (ImGuiCol_ScrollbarGrabActive, dw::brighter (theme.pillDivider, 0.4f));

    var (ImGuiStyleVar_FrameRounding, ctx.s (3.0f));
    var (ImGuiStyleVar_FrameBorderSize, 1.0f);
    var (ImGuiStyleVar_PopupRounding, ctx.s (3.0f));
    var (ImGuiStyleVar_PopupBorderSize, 1.0f);
    var (ImGuiStyleVar_GrabRounding, ctx.s (2.0f));
    var (ImGuiStyleVar_GrabMinSize, ctx.s (10.0f));
    var (ImGuiStyleVar_ScrollbarSize, ctx.s (10.0f));
    var (ImGuiStyleVar_ScrollbarRounding, ctx.s (4.0f));
    var (ImGuiStyleVar_ChildBorderSize, 0.0f);

    ImGui::PushFont (formFont (ctx));
}

ScopedFormStyle::~ScopedFormStyle()
{
    ImGui::PopFont();
    ImGui::PopStyleVar (vars);
    ImGui::PopStyleColor (colours);
}

void formLabel (const dw::Context& ctx, ImVec2 at, float width, float height,
                const char* text, dw::Align align)
{
    ImFont* const font = formFont (ctx);
    dw::text (ctx, font, font->FontSize,
              ImVec2 (at.x, at.y + (height - font->FontSize) * 0.5f), width,
              ctx.theme->textValue, text, align);
}

void formHeading (const dw::Context& ctx, ImVec2 at, float width, float height,
                  const char* text)
{
    ImFont* const font = ctx.fonts->title != nullptr ? ctx.fonts->title : formFont (ctx);
    dw::text (ctx, font, font->FontSize,
              ImVec2 (at.x, at.y + (height - font->FontSize) * 0.5f), width,
              rgba (kHeadingText), text, dw::Align::left);
}

void formRule (const dw::Context& ctx, ImVec2 at, float width)
{
    ctx.dl->AddLine (at, ImVec2 (at.x + width, at.y), rgba (kRule), ctx.s (1.0f));
}

bool formCombo (dw::Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                const char* const* items, int count, int& selected, bool enabled)
{
    ++ctx.widgets;
    const ScopedRowHeight rowHeight (ctx, br.y - tl.y);
    ImGui::BeginDisabled (! enabled);
    ImGui::SetCursorScreenPos (tl);
    ImGui::SetNextItemWidth (std::max (1.0f, br.x - tl.x));

    const char* const preview = selected >= 0 && selected < count ? items[selected] : "";
    bool picked = false;
    if (ImGui::BeginCombo (id, preview))
    {
        for (int i = 0; i < count; ++i)
        {
            const bool isSelected = i == selected;
            ImGui::PushID (i);
            if (ImGui::Selectable (items[i], isSelected) && i != selected)
            {
                selected = i;
                picked = true;
            }
            if (isSelected)
                ImGui::SetItemDefaultFocus();
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::EndDisabled();
    return picked;
}

void ComboModel::clear()
{
    labels.clear();
    items.clear();
    selected = -1;
}

void ComboModel::add (std::string label)
{
    labels.push_back (std::move (label));
}

void ComboModel::finish (int selectedIndex)
{
    items.clear();
    items.reserve (labels.size());
    for (const auto& entry : labels)
        items.push_back (entry.c_str());
    selected = selectedIndex;
}

const std::string& ComboModel::label (int index) const
{
    static const std::string empty;
    if (index < 0 || index >= static_cast<int> (labels.size()))
        return empty;
    return labels[static_cast<std::size_t> (index)];
}

bool formCombo (dw::Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                ComboModel& model, bool enabled)
{
    return formCombo (ctx, id, tl, br, model.items.data(), model.count(), model.selected,
                      enabled);
}

bool formButton (dw::Context& ctx, const char* id, ImVec2 tl, ImVec2 br, const char* label)
{
    ImFont* const font = formFont (ctx);
    dw::ButtonStyle style;
    style.offFill = rgba (kFieldFill);
    style.onFill = dw::brighter (rgba (kFieldFill), 0.25f);
    style.offText = ctx.theme->textBright;
    style.onText = ctx.theme->textBright;
    style.font = font;
    style.fontSize = font->FontSize;
    style.rounding = ctx.s (3.0f);
    return dw::textButton (ctx, id, tl, br, label, false, style).clicked;
}

bool formCheckbox (dw::Context& ctx, const char* id, ImVec2 at, float height,
                   const char* label, bool& value)
{
    ++ctx.widgets;
    // The tick box is square and sized from the font, so the row height only decides
    // where it sits rather than how big it is.
    ImFont* const font = formFont (ctx);
    const float boxPad = ctx.s (3.0f);
    const float boxHeight = font->FontSize + boxPad * 2.0f;
    ImGui::PushStyleVar (ImGuiStyleVar_FramePadding, ImVec2 (boxPad, boxPad));
    ImGui::SetCursorScreenPos (ImVec2 (at.x, at.y + (height - boxHeight) * 0.5f));
    ImGui::PushID (id);
    const bool toggled = ImGui::Checkbox (label, &value);
    ImGui::PopID();
    ImGui::PopStyleVar();
    return toggled;
}

FormSliderResult formSlider (dw::Context& ctx, const char* id, ImVec2 tl, ImVec2 br,
                             float& value, float minimum, float maximum, const char* format)
{
    ++ctx.widgets;
    const ScopedRowHeight rowHeight (ctx, br.y - tl.y);
    ImGui::SetCursorScreenPos (tl);
    ImGui::SetNextItemWidth (std::max (1.0f, br.x - tl.x));

    FormSliderResult result;
    result.changed = ImGui::SliderFloat (id, &value, minimum, maximum, format,
                                         ImGuiSliderFlags_AlwaysClamp);
    result.released = ImGui::IsItemDeactivatedAfterEdit();
    return result;
}

void formTooltip (const char* text)
{
    if (! ImGui::IsItemHovered (ImGuiHoveredFlags_ForTooltip))
        return;
    // The wrap position belongs to the window the text is submitted into, so it has to
    // be pushed inside the tooltip rather than around a SetTooltip call - which opens
    // and closes a window of its own and leaves the prose on one unreadable line.
    if (! ImGui::BeginTooltip())
        return;
    ImGui::PushTextWrapPos (ImGui::GetFontSize() * 24.0f);
    ImGui::TextUnformatted (text);
    ImGui::PopTextWrapPos();
    ImGui::EndTooltip();
}
} // namespace duskstudio::imgui
