#include "BuiltinLaneView.h"
#include "DuskTheme.h"
#include "PanelControls.h"
#include "../../engine/builtin/NativeBuiltinSlot.h"

#include <algorithm>
#include <cctype>
#include <cfloat>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// Design pixels.
constexpr float kInset            = 10.0f;
constexpr float kGroupGap         = 10.0f;
constexpr float kGroupPad         = 10.0f;
constexpr float kGroupCaptionH    = 22.0f;
constexpr float kGroupRounding    = 4.0f;
constexpr float kRowGap           = 10.0f;
constexpr float kCellPad          = 8.0f;
constexpr float kCellMinW         = 70.0f;
constexpr float kKnobCaptionH     = 18.0f;
constexpr float kKnobValueH       = 20.0f;
constexpr float kSelectorCaptionH = 18.0f;
constexpr float kBankH            = 24.0f;
constexpr float kBankButtonMinW   = 44.0f;
constexpr float kBankButtonMaxW   = 72.0f;
constexpr float kBankLabelPad     = 12.0f;
constexpr float kComboH           = 24.0f;
constexpr float kComboMinW        = 140.0f;
constexpr float kComboMaxW        = 200.0f;
constexpr float kComboArrowW      = 30.0f;
constexpr float kToggleH          = 24.0f;
constexpr float kToggleMinW       = 72.0f;
constexpr float kToggleGap        = 8.0f;
constexpr float kToggleLabelPad   = 16.0f;
constexpr float kMinRadius        = 14.0f;
constexpr float kMaxRadius        = 44.0f;
// A group grows with the lane, but only so far: a lone knob in a box stretched across
// a wide lane, or a row of knobs in a box three times their height, reads as a gap
// rather than a section. Past that the faceplate is centred in the lane.
constexpr float kMaxGroupGrowth   = 2.0f;
constexpr float kMaxRowGrowth     = 1.5f;

// Text sizes, each the design size of the face it is drawn with, so no glyph is scaled
// off the size it was baked at.
constexpr float kCaptionSize = 12.0f;  // band
constexpr float kValueSize   = 14.0f;  // valueLarge
constexpr float kGroupSize   = 10.5f;  // pill
constexpr float kBankSize    = 10.5f;  // pill
constexpr float kToggleSize  = 10.5f;  // pill
constexpr float kComboSize   = 12.0f;  // band, which the form controls draw with

constexpr unsigned int kPanelFill     = 0x20202aff;
constexpr unsigned int kPanelBorder   = 0x3a3a46ff;
constexpr unsigned int kGroupFill     = 0x18181fff;
constexpr unsigned int kGroupBorder   = 0x30303cff;
constexpr unsigned int kCaptionText   = 0xa0a0a8ff;
constexpr unsigned int kValueText     = 0xe0e0e0ff;
constexpr unsigned int kToggleOnText  = 0x121214ff;
constexpr unsigned int kDefaultAccent = 0x9080c0ff;

// The palette constants above are RGBA; Dear ImGui packs ABGR.
ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

bool same (float a, float b) { return ! (a < b) && ! (b < a); }

enum class Control { knob, stepped, bank, dropdown, toggle };

struct GroupSpec
{
    const char* caption;
    const char* params[6];
};

struct UnitSpec
{
    const char* unitId;
    unsigned int accent;
    GroupSpec groups[4];
};

// The units an aux lane's picker offers, grouped the way the hardware they stand in
// for is. Parameter ids rather than indices, so a unit that reorders its table keeps
// its layout; a parameter the table does not name still lands, in its own section.
const UnitSpec kUnitSpecs[] = {
    { "dusk.builtin.utility", 0xa0a8b8ff,
      { { "Level", { "gain_db", "polarity" } },
        { "Image", { "width", "mono" } } } },
    { "dusk.builtin.reverb", 0x9080c0ff,
      { { "Tank", { "algorithm", "decay", "size", "predelay" } },
        { "Tone", { "damping", "width", "lo_cut", "hi_cut" } },
        { "Mix", { "mix" } } } },
    { "dusk.builtin.delay", 0x5a8ad0ff,
      { { "Heads", { "mode", "repeat_rate", "intensity" } },
        { "Tape", { "input", "wow_flutter", "tape_age" } },
        { "Tone", { "bass", "treble" } },
        { "Mix", { "echo", "reverb", "dry" } } } },
    { "dusk.builtin.tape", 0xd09060ff,
      { { "Machine", { "machine", "speed", "type", "eq_standard", "signal_path" } },
        { "Level", { "input", "calibration", "bias", "output" } },
        { "Tone", { "hpf", "lpf" } },
        { "Transport", { "wow", "flutter", "noise", "auto_cal", "auto_comp" } } } },
};

constexpr int kMaxKnobColumns = 4;

struct Item
{
    int param = -1;
    Control kind = Control::knob;
};

struct Group
{
    std::string caption;
    std::vector<Item> selectors;   // a bank or a dropdown per row
    std::vector<Item> knobs;       // knobs and rotary switches, in rows of cells
    std::vector<Item> toggles;     // one row of latching buttons
    int columns = 1;

    // Text-driven widths in design pixels, measured against the faces that draw them.
    float captionW = 0.0f;
    float knobTextW = 0.0f;
    std::vector<float> selectorW;
    std::vector<float> toggleW;
    float togglesW = 0.0f;

    bool empty() const { return selectors.empty() && knobs.empty() && toggles.empty(); }
    int knobRows() const
    {
        return knobs.empty() ? 0 : (static_cast<int> (knobs.size()) + columns - 1) / columns;
    }
};

std::string upperCase (const char* text)
{
    std::string out (text != nullptr ? text : "");
    for (auto& c : out)
        c = static_cast<char> (std::toupper (static_cast<unsigned char> (c)));
    return out;
}

// A switch whose positions are numbers - the tape echo's twelve head modes - reads as
// the rotary switch it is on the hardware. Named positions become a bank of buttons
// when there are few enough to label, and a dropdown otherwise.
Control controlFor (const builtin::ParamInfo& info)
{
    switch (info.kind)
    {
        case builtin::ParamKind::Toggle:
            return Control::toggle;

        case builtin::ParamKind::Choice:
        {
            if (info.choices == nullptr || info.choiceCount < 1)
                return Control::knob;
            bool numbered = true;
            for (int c = 0; c < info.choiceCount && numbered; ++c)
            {
                const char* label = info.choices[c];
                numbered = label != nullptr && *label != 0;
                for (; numbered && label != nullptr && *label != 0; ++label)
                    numbered = std::isdigit (static_cast<unsigned char> (*label)) != 0;
            }
            if (numbered)
                return Control::stepped;
            return info.choiceCount <= 4 ? Control::bank : Control::dropdown;
        }

        case builtin::ParamKind::Continuous:
        default:
            return Control::knob;
    }
}

void addItem (Group& group, int index, const builtin::ParamInfo& info)
{
    const Item item { index, controlFor (info) };
    switch (item.kind)
    {
        case Control::bank:
        case Control::dropdown: group.selectors.push_back (item); break;
        case Control::toggle:   group.toggles.push_back (item); break;
        case Control::knob:
        case Control::stepped:  group.knobs.push_back (item); break;
    }
}

std::vector<Group> buildGroups (const builtin::NativeBuiltinSlot& slot, unsigned int& accent)
{
    std::vector<Group> groups;
    const int count = slot.paramCount();
    std::vector<bool> placed (static_cast<std::size_t> (std::max (0, count)), false);

    const auto indexOf = [&slot, count] (const char* id)
    {
        for (int i = 0; i < count; ++i)
            if (const auto* info = slot.paramInfo (i); info != nullptr && std::strcmp (info->id, id) == 0)
                return i;
        return -1;
    };

    accent = kDefaultAccent;
    for (const auto& unit : kUnitSpecs)
    {
        if (slot.getPluginId() != unit.unitId)
            continue;
        accent = unit.accent;
        for (const auto& spec : unit.groups)
        {
            if (spec.caption == nullptr)
                break;
            Group group;
            group.caption = upperCase (spec.caption);
            for (const char* id : spec.params)
            {
                if (id == nullptr)
                    break;
                const int index = indexOf (id);
                if (index < 0 || placed[static_cast<std::size_t> (index)])
                    continue;
                placed[static_cast<std::size_t> (index)] = true;
                addItem (group, index, *slot.paramInfo (index));
            }
            if (! group.empty())
                groups.push_back (std::move (group));
        }
    }

    const std::size_t firstFallback = groups.size();
    for (int i = 0; i < count; ++i)
    {
        const auto* info = slot.paramInfo (i);
        if (placed[static_cast<std::size_t> (i)] || info == nullptr)
            continue;
        auto caption = upperCase (info->section);
        if (groups.size() == firstFallback || groups.back().caption != caption)
        {
            groups.emplace_back();
            groups.back().caption = std::move (caption);
        }
        addItem (groups.back(), i, *info);
    }

    for (auto& group : groups)
        group.columns = std::clamp (static_cast<int> (group.knobs.size()), 1, kMaxKnobColumns);
    return groups;
}

void formatValue (const builtin::ParamInfo& info, float value, char* out, std::size_t size)
{
    if (info.kind == builtin::ParamKind::Choice && info.choices != nullptr && info.choiceCount > 0)
    {
        const int index = std::clamp (static_cast<int> (std::lround (value - info.minValue)), 0,
                                      info.choiceCount - 1);
        std::snprintf (out, size, "%s", info.choices[index] != nullptr ? info.choices[index] : "");
        return;
    }

    const char* const suffix = info.suffix != nullptr ? info.suffix : "";
    if (std::strcmp (suffix, "Hz") == 0 && value >= 1000.0f)
    {
        char hz[16];
        dw::formatFrequency (hz, sizeof hz, value);
        std::snprintf (out, size, "%sHz", hz);
        return;
    }

    // Two decimals reads wrong on a frequency and right on a ratio, so the precision
    // follows the span rather than the unit.
    const float span = info.maxValue - info.minValue;
    const int decimals = span >= 500.0f ? 0 : (span >= 10.0f ? 1 : 2);
    if (*suffix != 0)
        std::snprintf (out, size, "%.*f %s", decimals, static_cast<double> (value), suffix);
    else
        std::snprintf (out, size, "%.*f", decimals, static_cast<double> (value));
}

// A range whose top is twenty times its bottom or more - a cutoff, a decay time - sweeps
// geometrically, the way the hardware pot is tapered; the rest are linear.
dw::Range knobRange (const builtin::ParamInfo& info)
{
    if (info.minValue > 0.0f && info.maxValue >= info.minValue * 20.0f)
        return dw::Range::withMidPoint (info.minValue, info.maxValue,
                                        std::sqrt (info.minValue * info.maxValue));
    return dw::Range (info.minValue, info.maxValue);
}

ImFont* face (ImFont* font) { return font != nullptr ? font : ImGui::GetFont(); }

float textWidth (ImFont* font, float designSize, const char* text)
{
    return face (font)->CalcTextSizeA (designSize, FLT_MAX, 0.0f, text != nullptr ? text : "").x;
}

float cellWidth (const Group& group, float radius)
{
    return std::max ({ kCellMinW, radius * 2.0f + kCellPad * 2.0f,
                       group.knobTextW + kCellPad * 2.0f });
}

float knobRowHeight (float radius) { return kKnobCaptionH + radius * 2.0f + kKnobValueH; }

float selectorRowHeight (Control kind)
{
    return kSelectorCaptionH + (kind == Control::bank ? kBankH : kComboH);
}

// A group with knobs stacks in three bands, and each carries the gap that separates it
// from the knobs, so they add up to contentHeight.
float selectorsHeight (const Group& group)
{
    float height = 0.0f;
    for (const auto& item : group.selectors)
        height += selectorRowHeight (item.kind) + kRowGap;
    return height;
}

float knobsHeight (const Group& group, float radius)
{
    const int rows = group.knobRows();
    return rows == 0 ? 0.0f
                     : static_cast<float> (rows) * knobRowHeight (radius)
                           + static_cast<float> (rows - 1) * kRowGap;
}

float togglesHeight (const Group& group) { return group.toggles.empty() ? 0.0f : kRowGap + kToggleH; }

float contentHeight (const Group& group, float radius)
{
    float height = static_cast<float> (group.knobRows()) * knobRowHeight (radius);
    int rows = group.knobRows();
    for (const auto& item : group.selectors)
    {
        height += selectorRowHeight (item.kind);
        ++rows;
    }
    if (! group.toggles.empty())
    {
        height += kToggleH;
        ++rows;
    }
    return height + static_cast<float> (std::max (0, rows - 1)) * kRowGap;
}

// The knob rows of the groups sharing a row of the lane sit on one line, so a group
// with a selector above its knobs does not push them out of step with its neighbours.
// The groups with no knobs centre what they have instead.
struct RowBands
{
    float above = 0.0f;   // selectors over the shared knob line
    float knobs = 0.0f;
    float below = 0.0f;   // toggles under it
    float height = 0.0f;
};

RowBands rowBands (const std::vector<const Group*>& row, float radius)
{
    RowBands bands;
    float loose = 0.0f;
    for (const auto* group : row)
    {
        if (group->knobs.empty())
        {
            loose = std::max (loose, contentHeight (*group, radius));
            continue;
        }
        bands.above = std::max (bands.above, selectorsHeight (*group));
        bands.knobs = std::max (bands.knobs, knobsHeight (*group, radius));
        bands.below = std::max (bands.below, togglesHeight (*group));
    }
    bands.height = std::max (loose, bands.above + bands.knobs + bands.below);
    return bands;
}

float contentWidth (const Group& group, float radius)
{
    float width = std::max (group.captionW, group.togglesW);
    for (const float w : group.selectorW)
        width = std::max (width, w);
    if (! group.knobs.empty())
        width = std::max (width, static_cast<float> (group.columns) * cellWidth (group, radius));
    return width;
}

float groupWidth (const Group& group, float radius)
{
    return contentWidth (group, radius) + kGroupPad * 2.0f;
}

float boxHeight (const RowBands& bands) { return kGroupCaptionH + bands.height + kGroupPad; }

// Bit i of a split mask starts a new row of groups after group i.
std::vector<std::vector<const Group*>> rowsOf (const std::vector<Group>& groups, int mask)
{
    std::vector<std::vector<const Group*>> rows (1);
    for (std::size_t g = 0; g < groups.size(); ++g)
    {
        rows.back().push_back (&groups[g]);
        if (g + 1 < groups.size() && (mask & (1 << g)) != 0)
            rows.emplace_back();
    }
    return rows;
}

int rowCountOf (int mask)
{
    int rows = 1;
    for (; mask != 0; mask &= mask - 1)
        ++rows;
    return rows;
}

// The ImGui widgets and the kit's styles take design sizes and scale them by the
// context, so a layout that had to shrink to fit hands them the shrunk scale.
class ScopedShrink final
{
public:
    ScopedShrink (dw::Context& c, float shrink) : ctx (c), saved (c.scale) { ctx.scale *= shrink; }
    ~ScopedShrink() { ctx.scale = saved; }

    ScopedShrink (const ScopedShrink&) = delete;
    ScopedShrink& operator= (const ScopedShrink&) = delete;

private:
    dw::Context& ctx;
    const float saved;
};

class BuiltinLaneViewImpl final : public BuiltinLaneView
{
public:
    BuiltinLaneViewImpl (builtin::NativeBuiltinSlot& s, std::function<void (int)> touched)
        : slot (s), onTouched (std::move (touched))
    {
        groups = buildGroups (slot, accentHex);
    }

    // The lane sizes an inline view; this only reaches a modal plate, which this view
    // never takes.
    ImVec2 preferredSize() const override { return { 480.0f, 240.0f }; }
    bool wantsPlate() const override { return false; }

    const std::vector<Placement>& placements() const override { return placed; }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        placed.clear();
        const ImVec2 br (origin.x + size.x, origin.y + size.y);
        ctx.dl->AddRectFilled (origin, br, rgba (kPanelFill));
        ctx.dl->AddRect (ImVec2 (origin.x + 0.5f, origin.y + 0.5f),
                         ImVec2 (br.x - 0.5f, br.y - 0.5f), rgba (kPanelBorder), 0.0f, 0,
                         ctx.s (1.0f));
        if (groups.empty() || size.x < 1.0f || size.y < 1.0f)
            return;

        if (! same (size.x, laidOutSize.x) || ! same (size.y, laidOutSize.y)
            || ! same (ctx.scale, laidOutScale) || ctx.fonts != laidOutFonts)
            layOut (ctx, size);

        const float unit = ctx.scale * shrink;
        const ScopedShrink shrunk (ctx, shrink);
        for (const auto& box : boxes)
            drawGroup (ctx, box, ImVec2 (origin.x + box.x * unit, origin.y + box.y * unit),
                       box.w * unit, box.h * unit);
    }

private:
    struct Box
    {
        const Group* group = nullptr;
        RowBands bands;
        float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;   // design pixels, shrink applied
    };

    void measure (const dw::Context& ctx)
    {
        const auto* fonts = ctx.fonts;
        ImFont* const band = fonts != nullptr ? fonts->band : nullptr;
        ImFont* const value = fonts != nullptr ? fonts->valueLarge : nullptr;
        ImFont* const pill = fonts != nullptr ? fonts->pill : nullptr;

        char readout[32];
        for (auto& group : groups)
        {
            group.captionW = textWidth (pill, kGroupSize, group.caption.c_str());

            group.knobTextW = 0.0f;
            for (const auto& item : group.knobs)
            {
                const auto* found = slot.paramInfo (item.param);
                if (found == nullptr)
                    continue;
                const auto& info = *found;
                group.knobTextW = std::max (group.knobTextW, textWidth (band, kCaptionSize, info.name));
                const float samples[] = { info.minValue, info.maxValue, info.defaultValue };
                for (const float sample : samples)
                {
                    formatValue (info, sample, readout, sizeof readout);
                    group.knobTextW = std::max (group.knobTextW, textWidth (value, kValueSize, readout));
                }
                for (int c = 0; item.kind == Control::stepped && c < info.choiceCount; ++c)
                    group.knobTextW = std::max (group.knobTextW,
                                                textWidth (value, kValueSize, info.choices[c]));
            }

            group.selectorW.clear();
            for (const auto& item : group.selectors)
            {
                const auto* found = slot.paramInfo (item.param);
                if (found == nullptr)
                {
                    group.selectorW.push_back (kComboMinW);
                    continue;
                }
                const auto& info = *found;
                float widest = 0.0f;
                for (int c = 0; c < info.choiceCount; ++c)
                    widest = std::max (widest, item.kind == Control::bank
                                                   ? textWidth (pill, kBankSize, info.choices[c])
                                                   : textWidth (band, kComboSize, info.choices[c]));
                const float count = static_cast<float> (info.choiceCount);
                const float control = item.kind == Control::bank
                                    ? count * std::max (kBankButtonMinW, widest + kBankLabelPad)
                                    : std::max (kComboMinW, widest + kComboArrowW);
                group.selectorW.push_back (std::max (control, textWidth (band, kCaptionSize, info.name)));
            }

            group.toggleW.clear();
            group.togglesW = 0.0f;
            for (const auto& item : group.toggles)
            {
                const auto* info = slot.paramInfo (item.param);
                const float w = std::max (kToggleMinW,
                                          textWidth (pill, kToggleSize, info != nullptr ? info->name : "")
                                              + kToggleLabelPad);
                group.toggleW.push_back (w);
                group.togglesW += w;
            }
            group.togglesW += static_cast<float> (std::max (0, static_cast<int> (group.toggles.size()) - 1))
                            * kToggleGap;
        }
    }

    // Largest knob radius that fits, and at it the fewest rows of groups. When even the
    // smallest radius overflows, the arrangement that needs the least shrinking wins and
    // everything - text included - is scaled down by that much.
    void layOut (const dw::Context& ctx, ImVec2 size)
    {
        laidOutSize = size;
        laidOutScale = ctx.scale;
        laidOutFonts = ctx.fonts;
        measure (ctx);

        const float width = size.x / ctx.scale;
        const float height = size.y / ctx.scale;
        const int groupCount = static_cast<int> (groups.size());
        const int masks = 1 << (groupCount - 1);

        const auto need = [this] (int mask, float r)
        {
            ImVec2 total (0.0f, 0.0f);
            for (const auto& row : rowsOf (groups, mask))
            {
                float rowW = static_cast<float> (row.size() - 1) * kGroupGap;
                for (const auto* group : row)
                    rowW += groupWidth (*group, r);
                total.x = std::max (total.x, rowW);
                total.y += boxHeight (rowBands (row, r)) + (total.y > 0.0f ? kGroupGap : 0.0f);
            }
            return ImVec2 (total.x + kInset * 2.0f, total.y + kInset * 2.0f);
        };

        int chosen = -1;
        radius = kMinRadius;
        shrink = 1.0f;
        for (float r = kMaxRadius; r >= kMinRadius && chosen < 0; r -= 1.0f)
        {
            int fewest = INT_MAX;
            float narrowest = FLT_MAX;
            for (int mask = 0; mask < masks; ++mask)
            {
                const auto needed = need (mask, r);
                if (needed.x > width || needed.y > height)
                    continue;
                const int rows = rowCountOf (mask);
                if (rows < fewest || (rows == fewest && needed.x < narrowest))
                {
                    fewest = rows;
                    narrowest = needed.x;
                    chosen = mask;
                    radius = r;
                }
            }
        }
        if (chosen < 0)
        {
            float best = 0.0f;
            for (int mask = 0; mask < masks; ++mask)
            {
                const auto needed = need (mask, kMinRadius);
                const float fit = std::min (width / needed.x, height / needed.y);
                if (fit > best)
                {
                    best = fit;
                    chosen = mask;
                }
            }
            shrink = std::clamp (best, 0.05f, 1.0f);
        }

        place (chosen, width / shrink, height / shrink);
    }

    // The rows share out whatever height the lane has beyond their own, and each row
    // shares out its spare width among its groups in proportion to their size.
    void place (int mask, float width, float height)
    {
        boxes.clear();
        const auto rows = rowsOf (groups, mask);

        std::vector<RowBands> bands;
        float naturalH = 0.0f;
        for (const auto& row : rows)
        {
            bands.push_back (rowBands (row, radius));
            naturalH += boxHeight (bands.back());
        }
        const float rowCount = static_cast<float> (rows.size());
        const float gapsH = (rowCount - 1.0f) * kGroupGap;
        const float spareH = std::max (0.0f, height - kInset * 2.0f - naturalH - gapsH);

        std::vector<float> rowHeights;
        float usedH = gapsH;
        for (const auto& band : bands)
        {
            const float natural = boxHeight (band);
            rowHeights.push_back (std::min (natural * kMaxRowGrowth, natural + spareH / rowCount));
            usedH += rowHeights.back();
        }

        float y = kInset + std::max (0.0f, (height - kInset * 2.0f - usedH) * 0.5f);
        for (std::size_t r = 0; r < rows.size(); ++r)
        {
            const float rowH = rowHeights[r];

            float naturalW = 0.0f;
            for (const auto* group : rows[r])
                naturalW += groupWidth (*group, radius);
            const float gaps = static_cast<float> (rows[r].size() - 1) * kGroupGap;
            const float spareW = std::max (0.0f, width - kInset * 2.0f - naturalW - gaps);

            std::vector<float> widths;
            float usedW = gaps;
            for (const auto* group : rows[r])
            {
                const float w = groupWidth (*group, radius);
                widths.push_back (std::min (w * kMaxGroupGrowth, w + spareW * w / naturalW));
                usedW += widths.back();
            }

            float x = kInset + std::max (0.0f, (width - kInset * 2.0f - usedW) * 0.5f);
            for (std::size_t i = 0; i < rows[r].size(); ++i)
            {
                boxes.push_back ({ rows[r][i], bands[r], x, y, widths[i], rowH });
                x += widths[i] + kGroupGap;
            }
            y += rowH + kGroupGap;
        }
    }

    void drawGroup (dw::Context& ctx, const Box& box, ImVec2 tl, float width, float height)
    {
        const Group& group = *box.group;
        const ImU32 accent = rgba (accentHex);
        const ImVec2 br (tl.x + width, tl.y + height);
        ctx.dl->AddRectFilled (tl, br, rgba (kGroupFill), ctx.s (kGroupRounding));
        ctx.dl->AddRect (tl, br, rgba (kGroupBorder), ctx.s (kGroupRounding), 0, ctx.s (1.0f));

        const float pad = ctx.s (kGroupPad);
        const float innerW = width - pad * 2.0f;
        dw::text (ctx, ctx.fonts->pill, ctx.s (kGroupSize), ImVec2 (tl.x + pad, tl.y + ctx.s (6.0f)),
                  innerW, accent, group.caption.c_str(), dw::Align::left);
        ctx.dl->AddLine (ImVec2 (tl.x + pad, tl.y + ctx.s (kGroupCaptionH - 3.0f)),
                         ImVec2 (br.x - pad, tl.y + ctx.s (kGroupCaptionH - 3.0f)),
                         dw::withAlpha (accent, 0.35f), ctx.s (1.0f));

        // The row's bands sit as one block in the middle of whatever height the row gave
        // the group.
        const float areaTop = tl.y + ctx.s (kGroupCaptionH);
        const float areaH = br.y - pad - areaTop;
        const float left = tl.x + pad;
        const float blockTop = areaTop + std::max (0.0f, (areaH - ctx.s (box.bands.height)) * 0.5f);
        float y = group.knobs.empty()
                ? areaTop + std::max (0.0f, (areaH - ctx.s (contentHeight (group, radius))) * 0.5f)
                : blockTop + ctx.s (box.bands.above - selectorsHeight (group));

        for (std::size_t i = 0; i < group.selectors.size(); ++i)
        {
            const auto& item = group.selectors[i];
            const float natural = ctx.s (group.selectorW[i]);
            const auto* info = slot.paramInfo (item.param);
            const float cap = item.kind == Control::bank && info != nullptr
                            ? ctx.s (kBankButtonMaxW * static_cast<float> (info->choiceCount))
                            : ctx.s (kComboMaxW);
            const float w = std::min (innerW, std::max (natural, cap));
            drawSelector (ctx, item, ImVec2 (left + (innerW - w) * 0.5f, y), w);
            y += ctx.s (selectorRowHeight (item.kind) + kRowGap);
        }

        const float cellW = innerW / static_cast<float> (group.columns);
        for (int row = 0; row < group.knobRows(); ++row)
        {
            const int first = row * group.columns;
            const int inRow = std::min (group.columns, static_cast<int> (group.knobs.size()) - first);
            const float rowLeft = left + static_cast<float> (group.columns - inRow) * cellW * 0.5f;
            for (int k = 0; k < inRow; ++k)
                drawKnob (ctx, group.knobs[static_cast<std::size_t> (first + k)],
                          ImVec2 (rowLeft + static_cast<float> (k) * cellW, y), cellW);
            y += ctx.s (knobRowHeight (radius) + kRowGap);
        }

        if (! group.toggles.empty())
        {
            float x = left + (innerW - ctx.s (group.togglesW)) * 0.5f;
            for (std::size_t i = 0; i < group.toggles.size(); ++i)
            {
                const float w = ctx.s (group.toggleW[i]);
                drawToggle (ctx, group.toggles[i], ImVec2 (x, y), ImVec2 (x + w, y + ctx.s (kToggleH)));
                x += w + ctx.s (kToggleGap);
            }
        }
    }

    void touched (int index)
    {
        if (onTouched)
            onTouched (index);
    }

    void drawKnob (dw::Context& ctx, const Item& item, ImVec2 tl, float cellW)
    {
        const auto* info = slot.paramInfo (item.param);
        if (info == nullptr)
            return;

        const float value = slot.getParamValue (item.param);
        char readout[32];
        formatValue (*info, value, readout, sizeof readout);

        dw::text (ctx, ctx.fonts->band, ctx.s (kCaptionSize), tl, cellW, rgba (kCaptionText),
                  info->name);

        const bool stepped = item.kind == Control::stepped;
        dw::KnobStyle style;
        style.fill = rgba (accentHex);
        if (stepped)
            style.wheelStep = 1.0f / static_cast<float> (std::max (1, info->choiceCount - 1));

        char id[64];
        std::snprintf (id, sizeof id, "##lane_%s", info->id);
        const float r = ctx.s (radius);
        const ImVec2 centre (tl.x + cellW * 0.5f, tl.y + ctx.s (kKnobCaptionH) + r);
        const auto result = dw::knob (ctx, id, centre, r, value,
                                      stepped ? dw::Range (info->minValue, info->maxValue)
                                              : knobRange (*info),
                                      info->defaultValue, style);

        const float valueTop = centre.y + r + ctx.s (3.0f);
        dw::text (ctx, ctx.fonts->valueLarge, ctx.s (kValueSize), ImVec2 (tl.x, valueTop), cellW,
                  rgba (kValueText), readout);

        if (result.changed)
        {
            float next = std::clamp (result.value, info->minValue, info->maxValue);
            if (stepped)
                next = std::round (next);
            if (! same (next, value))
            {
                slot.setParamValue (item.param, next);
                touched (item.param);
            }
        }
        if (result.dragging && ctx.drag != nullptr)
        {
            formatValue (*info, slot.getParamValue (item.param), ctx.drag->bubbleText,
                         sizeof ctx.drag->bubbleText);
            ctx.drag->bubbleAt = ImVec2 (centre.x + r, centre.y);
        }

        placed.push_back ({ item.param, tl,
                            ImVec2 (tl.x + cellW, tl.y + ctx.s (knobRowHeight (radius))) });
    }

    void drawSelector (dw::Context& ctx, const Item& item, ImVec2 tl, float width)
    {
        const auto* info = slot.paramInfo (item.param);
        if (info == nullptr)
            return;

        dw::text (ctx, ctx.fonts->band, ctx.s (kCaptionSize), tl, width, rgba (kCaptionText),
                  info->name);

        const int lowest = static_cast<int> (std::lround (info->minValue));
        const int selected = std::clamp (static_cast<int> (std::lround (slot.getParamValue (item.param)))
                                             - lowest,
                                         0, std::max (0, info->choiceCount - 1));
        const ImVec2 controlTl (tl.x, tl.y + ctx.s (kSelectorCaptionH));

        char id[64];
        std::snprintf (id, sizeof id, "##lane_%s", info->id);
        int picked = -1;
        ImVec2 controlBr;
        if (item.kind == Control::bank)
        {
            controlBr = ImVec2 (tl.x + width, controlTl.y + ctx.s (kBankH));
            dw::ButtonBankStyle style;
            style.orientation = dw::BankOrientation::horizontal;
            style.labelSide = dw::BankLabelSide::inside;
            style.tab = rgba (accentHex);
            style.font = ctx.fonts->pill;
            style.fontSize = kBankSize;
            const auto result = dw::buttonBank (ctx, id, controlTl, controlBr, info->choices,
                                                info->choiceCount, selected, style);
            if (result.changed)
                picked = result.clicked;
        }
        else
        {
            controlBr = ImVec2 (tl.x + width, controlTl.y + ctx.s (kComboH));
            combo.clear();
            for (int c = 0; c < info->choiceCount; ++c)
                combo.add (info->choices[c] != nullptr ? info->choices[c] : "");
            combo.finish (selected);
            const ScopedFormStyle style (ctx);
            if (formCombo (ctx, id, controlTl, controlBr, combo))
                picked = combo.selected;
        }

        if (picked >= 0 && picked != selected)
        {
            slot.setParamValue (item.param, static_cast<float> (picked + lowest));
            touched (item.param);
        }
        placed.push_back ({ item.param, tl, controlBr });
    }

    void drawToggle (dw::Context& ctx, const Item& item, ImVec2 tl, ImVec2 br)
    {
        const auto* info = slot.paramInfo (item.param);
        if (info == nullptr)
            return;

        const bool on = slot.getParamValue (item.param) >= 0.5f;
        dw::ButtonStyle style;
        style.onFill = rgba (accentHex);
        style.onText = rgba (kToggleOnText);
        style.fontSize = kToggleSize;

        char id[64];
        std::snprintf (id, sizeof id, "##lane_%s", info->id);
        if (dw::textButton (ctx, id, tl, br, info->name, on, style).clicked)
        {
            slot.setParamValue (item.param, on ? 0.0f : 1.0f);
            touched (item.param);
        }
        placed.push_back ({ item.param, tl, br });
    }

    builtin::NativeBuiltinSlot& slot;
    std::function<void (int)> onTouched;
    unsigned int accentHex = kDefaultAccent;
    std::vector<Group> groups;

    ImVec2 laidOutSize {};
    float laidOutScale = 0.0f;
    const dw::Fonts* laidOutFonts = nullptr;
    float radius = kMinRadius;
    float shrink = 1.0f;
    std::vector<Box> boxes;

    std::vector<Placement> placed;
    ComboModel combo;
};
} // namespace

std::unique_ptr<BuiltinLaneView> makeBuiltinLaneView (
    builtin::NativeBuiltinSlot& slot,
    std::function<void (int paramIndex)> onParameterTouched)
{
    return std::make_unique<BuiltinLaneViewImpl> (slot, std::move (onParameterTouched));
}
} // namespace duskstudio::imgui
