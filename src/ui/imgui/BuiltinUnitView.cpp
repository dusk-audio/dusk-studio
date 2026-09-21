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

// The palette constants above are RGBA; Dear ImGui packs ABGR.
ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

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
    std::vector<int> params;
};

class BuiltinUnitViewImpl final : public DuskPanelView
{
public:
    BuiltinUnitViewImpl (builtin::NativeBuiltinSlot& s, std::string t,
                         std::function<void (int)> touched)
        : slot (s), title (std::move (t)), onTouched (std::move (touched))
    {
        controlBounds.resize ((size_t) slot.paramCount());
        buildLayout();
    }

    bool controlPointForScenario (const std::string& control, ImVec2& point, float position) const override
    {
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* info = slot.paramInfo (i);
            if (info == nullptr || control != info->id) continue;
            const auto& bounds = controlBounds[(size_t) i];
            if (bounds.second.x <= bounds.first.x) return false;
            point = { bounds.first.x + std::clamp (position, 0.0f, 1.0f) * (bounds.second.x - bounds.first.x),
                      (bounds.first.y + bounds.second.y) * 0.5f };
            return true;
        }
        return false;
    }
    ImVec2 preferredSize() const override { return { bodyWidth, bodyHeight }; }

    float dimAlpha() const override { return 0.28f; }
    bool  takeDismissRequest() override { return std::exchange (dismissRequested, false); }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override;

private:
    void buildLayout();
    void drawRow (dw::Context& ctx, ImVec2 at, int paramIndex);

    builtin::NativeBuiltinSlot& slot;
    std::string title;
    std::function<void (int)> onTouched;
    bool dismissRequested = false;

    std::vector<Section> sections;
    std::vector<Column>  columns;
    float bodyWidth = 0.0f;
    float bodyHeight = 0.0f;

    std::vector<std::pair<ImVec2, ImVec2>> controlBounds;
    ComboModel combo;
};

void BuiltinUnitViewImpl::buildLayout()
{
    const int count = slot.paramCount();
    for (int i = 0; i < count; ++i)
    {
        const auto* info = slot.paramInfo (i);
        if (info == nullptr || info->hidden) continue;
        const char* name = info->section != nullptr ? info->section : "";
        if (sections.empty() || std::string (sections.back().name) != name)
            sections.push_back ({ name, {} });
        sections.back().params.push_back (i);
    }

    // Pack sections into columns without splitting one, so a tall unit grows
    // sideways rather than off the bottom of the window.
    for (int s = 0; s < (int) sections.size(); ++s)
    {
        const int rows = (int) sections[(size_t) s].params.size() + 1;
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
               + kSectionSpace;
}

void BuiltinUnitViewImpl::drawRow (dw::Context& ctx, ImVec2 at, int paramIndex)
{
    const auto* info = slot.paramInfo (paramIndex);
    if (info == nullptr) return;

    formLabel (ctx, at, ctx.s (kLabelW), ctx.s (kRowH), info->name);

    const ImVec2 tl { at.x + ctx.s (kLabelW + kLabelGap), at.y + ctx.s (2.0f) };
    const ImVec2 br { tl.x + ctx.s (kControlW), at.y + ctx.s (kRowH - 2.0f) };

    controlBounds[(size_t) paramIndex] = { tl, br };

    char id[64];
    std::snprintf (id, sizeof (id), "##bu_%s", info->id);

    float value = slot.getParamValue (paramIndex);

    switch (info->kind)
    {
        case builtin::ParamKind::Toggle:
        {
            bool on = value >= 0.5f;
            if (formCheckbox (ctx, id, tl, ctx.s (kRowH - 4.0f), on ? "On" : "Off", on))
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
            // The suffix is literal text inside a printf format, so a percent
            // sign in it has to be escaped or it eats the following character.
            char suffix[16] = {};
            for (int in = 0, out = 0; info->suffix[in] != '\0' && out < 14; ++in)
            {
                suffix[out++] = info->suffix[in];
                if (info->suffix[in] == '%') suffix[out++] = '%';
            }

            char format[32];
            if (span >= 500.0f)      std::snprintf (format, sizeof (format), "%%.0f %s", suffix);
            else if (span >= 10.0f)  std::snprintf (format, sizeof (format), "%%.1f %s", suffix);
            else                     std::snprintf (format, sizeof (format), "%%.2f %s", suffix);

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
    auto& dl = *ctx.dl;
    const ImVec2 br { origin.x + size.x, origin.y + size.y };

    dl.AddRectFilled (origin, br, rgba (kPanelFill), ctx.s (6.0f));
    dl.AddRect (origin, br, rgba (kPanelBorder), ctx.s (6.0f), 0, ctx.s (1.0f));

    ScopedFormStyle style (ctx);

    const ImVec2 header { origin.x + ctx.s (kOuterInset), origin.y + ctx.s (kOuterInset) };
    dw::text (ctx, ctx.fonts->title, ctx.s (13.0f), header, br.x - header.x,
              rgba (kTitleText), title.c_str(), dw::Align::left);
    dl.AddLine ({ header.x, header.y + ctx.s (kHeaderH - 4.0f) },
                { br.x - ctx.s (kOuterInset), header.y + ctx.s (kHeaderH - 4.0f) },
                rgba (kAccent), ctx.s (1.0f));

    const float columnW = ctx.s (kLabelW + kLabelGap + kControlW);
    const float rowPitch = ctx.s (kRowH + kRowGap);
    float x = header.x;

    for (const auto& column : columns)
    {
        float y = header.y + ctx.s (kHeaderH + kHeaderGap);
        for (int s = column.firstSection; s < column.firstSection + column.sectionCount; ++s)
        {
            const auto& section = sections[(size_t) s];
            formHeading (ctx, { x, y }, columnW, ctx.s (kRowH), section.name);
            y += rowPitch;

            for (const int p : section.params)
            {
                drawRow (ctx, { x, y }, p);
                y += rowPitch;
            }
        }
        x += columnW + ctx.s (kColumnGap);
    }
}
} // namespace

std::unique_ptr<DuskPanelView> makeBuiltinUnitView (
    builtin::NativeBuiltinSlot& slot,
    std::string title,
    std::function<void (int)> onParameterTouched)
{
    return std::make_unique<BuiltinUnitViewImpl> (slot, std::move (title),
                                                  std::move (onParameterTouched));
}
} // namespace duskstudio::imgui
