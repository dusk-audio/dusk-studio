#include "BuiltinUnitView.h"
#include "DuskTheme.h"
#include "PanelControls.h"
#include "../../engine/builtin/NativeBuiltinSlot.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

constexpr float kOuterInset   = 12.0f;
constexpr float kHeaderH      = 24.0f;
constexpr float kHeaderGap    = 10.0f;
constexpr float kSectionH     = 18.0f;
constexpr float kSectionGap   = 6.0f;
constexpr float kRowH         = 26.0f;
constexpr float kRowGap       = 4.0f;
constexpr float kLabelW       = 104.0f;
constexpr float kLabelGap     = 8.0f;
constexpr float kControlW     = 176.0f;
constexpr float kColumnGap    = 22.0f;
constexpr float kSectionSpace = 12.0f;

constexpr unsigned int kPanelFill   = 0x20202aff;
constexpr unsigned int kPanelBorder = 0x3a3a46ff;
constexpr unsigned int kTitleText   = 0xf0f0f4ff;
constexpr unsigned int kAccent      = 0x9080c0ff;

constexpr int kMaxRowsPerColumn = 13;

// A column is one section's rows plus its heading. Sections are laid out into
// columns in order and a section never straddles two of them, so a heading is
// always above the rows it names.
struct Column
{
    int firstSection = 0;
    int sectionCount = 0;
    int rowCount     = 0;
};

struct Section
{
    const char* name = nullptr;
    int firstParam = 0;
    int count = 0;
};

class BuiltinUnitViewImpl final : public DuskPanelView
{
public:
    BuiltinUnitViewImpl (builtin::NativeBuiltinSlot& s, std::string t,
                         std::function<void (int)> touched, bool inlineInStage)
        : slot (s), title (std::move (t)), onTouched (std::move (touched)),
          embedded (inlineInStage)
    {
        buildLayout();
    }

    ImVec2 preferredSize() const override { return { bodyWidth, bodyHeight }; }

    float dimAlpha() const override { return 0.28f; }
    bool  wantsPlate() const override { return ! embedded; }
    bool  takeDismissRequest() override { return std::exchange (dismissRequested, false); }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override;

private:
    void buildLayout();
    void drawRow (dw::Context& ctx, ImVec2 at, int paramIndex);

    builtin::NativeBuiltinSlot& slot;
    std::string title;
    std::function<void (int)> onTouched;
    bool embedded = false;
    bool dismissRequested = false;

    std::vector<Section> sections;
    std::vector<Column>  columns;
    float bodyWidth = 0.0f;
    float bodyHeight = 0.0f;

    ComboModel combo;
};

void BuiltinUnitViewImpl::buildLayout()
{
    const int count = slot.paramCount();
    for (int i = 0; i < count; ++i)
    {
        const auto* info = slot.paramInfo (i);
        if (info == nullptr) continue;
        const char* name = info->section != nullptr ? info->section : "";
        if (sections.empty() || std::string (sections.back().name) != name)
            sections.push_back ({ name, i, 0 });
        ++sections.back().count;
    }

    // Pack sections into columns without splitting one, so a tall unit grows
    // sideways rather than off the bottom of the window.
    for (int s = 0; s < (int) sections.size(); ++s)
    {
        const int rows = sections[(size_t) s].count + 1;
        if (columns.empty()
            || (columns.back().rowCount + rows > kMaxRowsPerColumn
                && columns.back().sectionCount > 0))
            columns.push_back ({ s, 0, 0 });
        ++columns.back().sectionCount;
        columns.back().rowCount += rows;
    }

    int tallest = 0;
    for (const auto& c : columns) tallest = std::max (tallest, c.rowCount);

    const float columnW = kLabelW + kLabelGap + kControlW;
    bodyWidth = kOuterInset * 2.0f
              + (float) columns.size() * columnW
              + (float) std::max (0, (int) columns.size() - 1) * kColumnGap;
    bodyHeight = kOuterInset * 2.0f + kHeaderH + kHeaderGap
               + (float) tallest * (kRowH + kRowGap)
               + (float) std::max (0, (int) sections.size() - 1) * 0.0f
               + kSectionSpace;
}

void BuiltinUnitViewImpl::drawRow (dw::Context& ctx, ImVec2 at, int paramIndex)
{
    const auto* info = slot.paramInfo (paramIndex);
    if (info == nullptr) return;

    formLabel (ctx, at, kLabelW, kRowH, info->name);

    const ImVec2 tl { at.x + kLabelW + kLabelGap, at.y + 2.0f };
    const ImVec2 br { tl.x + kControlW, at.y + kRowH - 2.0f };

    char id[64];
    std::snprintf (id, sizeof (id), "##bu_%s", info->id);

    float value = slot.getParamValue (paramIndex);

    switch (info->kind)
    {
        case builtin::ParamKind::Toggle:
        {
            bool on = value >= 0.5f;
            if (formCheckbox (ctx, id, tl, kRowH - 4.0f, on ? "On" : "Off", on))
            {
                slot.setParamValue (paramIndex, on ? 1.0f : 0.0f);
                if (onTouched) onTouched (paramIndex);
            }
            break;
        }

        case builtin::ParamKind::Choice:
        {
            combo.clear();
            for (int c = 0; c < info->choiceCount; ++c)
                combo.add (info->choices[c] != nullptr ? info->choices[c] : "");
            const int lowest = (int) std::lround (info->minValue);
            combo.finish (std::clamp ((int) std::lround (value) - lowest,
                                      0, std::max (0, info->choiceCount - 1)));
            if (formCombo (ctx, id, tl, br, combo))
            {
                slot.setParamValue (paramIndex, (float) (combo.selected + lowest));
                if (onTouched) onTouched (paramIndex);
            }
            break;
        }

        case builtin::ParamKind::Continuous:
        default:
        {
            // Two decimals reads wrong on a frequency and right on a ratio, so
            // the precision follows the span rather than the unit.
            const float span = info->maxValue - info->minValue;
            char format[32];
            if (span >= 500.0f)      std::snprintf (format, sizeof (format), "%%.0f %s", info->suffix);
            else if (span >= 10.0f)  std::snprintf (format, sizeof (format), "%%.1f %s", info->suffix);
            else                     std::snprintf (format, sizeof (format), "%%.2f %s", info->suffix);

            const auto result = formSlider (ctx, id, tl, br, value,
                                            info->minValue, info->maxValue, format);
            if (result.changed)
            {
                slot.setParamValue (paramIndex, value);
                if (onTouched) onTouched (paramIndex);
            }
            break;
        }
    }
}

void BuiltinUnitViewImpl::draw (dw::Context& ctx, ImVec2 origin, ImVec2 size)
{
    auto& dl = *ImGui::GetWindowDrawList();
    const ImVec2 br { origin.x + size.x, origin.y + size.y };

    dl.AddRectFilled (origin, br, kPanelFill, 6.0f);
    dl.AddRect (origin, br, kPanelBorder, 6.0f);

    ScopedFormStyle style (ctx);

    const ImVec2 header { origin.x + kOuterInset, origin.y + kOuterInset };
    dl.AddText (header, kTitleText, title.c_str());
    dl.AddLine ({ origin.x + kOuterInset, header.y + kHeaderH - 4.0f },
                { br.x - kOuterInset, header.y + kHeaderH - 4.0f }, kAccent);

    const float columnW = kLabelW + kLabelGap + kControlW;
    float x = origin.x + kOuterInset;

    for (const auto& column : columns)
    {
        float y = header.y + kHeaderH + kHeaderGap;
        for (int s = column.firstSection; s < column.firstSection + column.sectionCount; ++s)
        {
            const auto& section = sections[(size_t) s];
            formHeading (ctx, { x, y }, columnW, kRowH, section.name);
            y += kRowH + kRowGap;

            for (int p = section.firstParam; p < section.firstParam + section.count; ++p)
            {
                drawRow (ctx, { x, y }, p);
                y += kRowH + kRowGap;
            }
        }
        x += columnW + kColumnGap;
    }
}
} // namespace

std::unique_ptr<DuskPanelView> makeBuiltinUnitView (
    builtin::NativeBuiltinSlot& slot,
    std::string title,
    std::function<void (int)> onParameterTouched,
    bool inlineInStage)
{
    return std::make_unique<BuiltinUnitViewImpl> (slot, std::move (title),
                                                  std::move (onParameterTouched),
                                                  inlineInStage);
}
} // namespace duskstudio::imgui
