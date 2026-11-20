#include "MasteringCompView.h"
#include "PanelControls.h"
#include "../../dsp/MultiCompStage.h"
#include "../../dsp/MultibandCompPresets.h"
#include "../../engine/builtin/DafPlugin.h"
#include "../../session/Session.h"

#include <algorithm>
#include <array>
#include <cfloat>
#include <cmath>
#include <cstdint>
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

// Geometry, in design pixels.
constexpr float kSideInset = 8.0f;
constexpr float kTopInset = 8.0f;
constexpr float kHeaderH = 22.0f;
constexpr float kHeaderGap = 8.0f;
constexpr float kComboH = 24.0f;
constexpr float kPresetCaptionW = 52.0f;
constexpr float kPresetComboW = 190.0f;
constexpr float kBottomInset = 8.0f;
constexpr float kSectionGap = 6.0f;
constexpr float kColumnGap = 4.0f;
constexpr float kNameRowH = 16.0f;
constexpr float kToggleRowH = 18.0f;
constexpr float kRowGap = 3.0f;
constexpr float kCaptionH = 11.0f;
constexpr float kValueH = 13.0f;
constexpr float kMinCellH = 40.0f;
constexpr float kMaxCellH = 82.0f;
constexpr float kMinCellW = 52.0f;
constexpr float kMinRadius = 7.0f;
constexpr float kMaxRadius = 26.0f;
constexpr float kMinDisplayH = 60.0f;
constexpr float kLabelRowH = 15.0f;
constexpr float kMinPanelW = 400.0f;
constexpr float kMinPanelH = 410.0f;

constexpr float kFreqMinHz = 20.0f;
constexpr float kFreqMaxHz = 20000.0f;
constexpr float kGrRangeDb = 20.0f;
// A bar edge that moves less than this many pixels has caught up.
constexpr float kBarSettle = 0.25f;
// One meter tick per 30 Hz, the rate the other mastering meters ease at.
constexpr float kMeterInterval = 1.0f / 30.0f;

constexpr unsigned int kPanelFill = 0x20202aff;
constexpr unsigned int kPanelBorder = 0x3a3a46ff;
constexpr unsigned int kHeaderAccent = 0x60d060ff;
constexpr unsigned int kPresetCaption = 0x808088ff;
constexpr unsigned int kPlotFill = 0x0d0d11ff;
constexpr unsigned int kGridLine = 0x2a2a34ff;
constexpr unsigned int kDecadeText = 0x606068ff;
constexpr unsigned int kScaleText = 0x909094ff;
constexpr unsigned int kCaptionText = 0xa0a0a8ff;
constexpr unsigned int kHandleLine = 0xe8e8f0d0;
constexpr unsigned int kHandleIdle = 0x9090a0a0;
constexpr unsigned int kLabelFill = 0x181820f0;
constexpr unsigned int kLabelText = 0xe0e0e6ff;
constexpr unsigned int kGrText = 0xf09060ff;
constexpr unsigned int kBypassLit = 0xe0a040ff;
constexpr unsigned int kSwitchOff = 0x26262eff;
constexpr unsigned int kSwitchText = 0x9a9aa4ff;
constexpr unsigned int kOutText = 0x707078ff;
// The blue the limiter's and Phase's own dials wear.
constexpr unsigned int kStageAccent = 0x5a8ad0ff;

constexpr unsigned int kBandColours[kCompBandCount] = { 0xf07070ff, 0xe0c850ff, 0x70c870ff, 0x5a9ae8ff };
const char* const kBandNames[kCompBandCount] = { "LOW", "LOW-MID", "HIGH-MID", "HIGH" };
const char* const kBandTitles[kCompBandCount] = { "Low", "Low-Mid", "High-Mid", "High" };

constexpr int kKnobsPerBand = 5;
constexpr CompBandField kKnobFields[kKnobsPerBand] = { CompBandField::Threshold, CompBandField::Ratio,
                                                       CompBandField::Attack, CompBandField::Release,
                                                       CompBandField::Makeup };
const char* const kKnobCaptions[kKnobsPerBand] = { "THRESH", "RATIO", "ATTACK", "RELEASE", "MAKEUP" };

constexpr CompParamId kCrossovers[3] = { CompParamId::MbCrossover1, CompParamId::MbCrossover2,
                                         CompParamId::MbCrossover3 };

const char* const kEngageName = "comp_enabled";
const char* const kPresetName = "comp_preset";

const char* const kKnobHelp = "Drag or scroll to adjust; double-click to reset. "
                              "Cmd-click on macOS or Ctrl-click elsewhere to type a value.";

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

struct Rect
{
    float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

    bool empty() const noexcept { return ! (x1 > x0) || ! (y1 > y0); }
    bool overlaps (const Rect& o) const noexcept { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
    bool inside (const Rect& o) const noexcept { return x0 >= o.x0 && y0 >= o.y0 && x1 <= o.x1 && y1 <= o.y1; }
    Rect unite (const Rect& o) const noexcept
    {
        return { std::min (x0, o.x0), std::min (y0, o.y0), std::max (x1, o.x1), std::max (y1, o.y1) };
    }
};

float textWidth (ImFont* font, float size, const char* text)
{
    return font != nullptr ? font->CalcTextSizeA (size, FLT_MAX, 0.0f, text).x : 0.0f;
}

bool sameValue (float a, float b) noexcept
{
    std::uint32_t x = 0, y = 0;
    std::memcpy (&x, &a, sizeof x);
    std::memcpy (&y, &b, sizeof y);
    return x == y;
}

class MasteringCompViewImpl final : public DuskPanelView
{
public:
    MasteringCompViewImpl (MasteringParams& p, const MultiCompStage& s, int& preset)
        : params (p), comp (p.comp), stage (s), presetChoice (preset)
    {
        for (const auto id : compStageParams (CompStage::Multiband))
            controls.push_back ({ id, compParamSpec (id).symbol, {}, false });
        controls.push_back ({ CompParamId::kCount, kEngageName, {}, false });
        controls.push_back ({ CompParamId::kCount, kPresetName, {}, false });
        presetItems.push_back ("Select preset...");
        for (int i = 0; i < mbpresets::kNumPresets; ++i)
            presetItems.push_back (mbpresets::kPresets[(std::size_t) i].name);
        const auto& descs = probe.plugin().params();
        for (int c = 0; c < 3; ++c)
            for (std::size_t i = 0; i < descs.size(); ++i)
                if (descs[i].symbol == compParamSpec (kCrossovers[c]).symbol)
                    probeIndex[(std::size_t) c] = (int) i;
    }

    ImVec2 preferredSize() const override { return ImVec2 (kMinPanelW, kMinPanelH); }
    bool wantsPlate() const override { return false; }
    bool capturesKeyboard() const override { return keyboardActive; }
    bool escapeDismisses() const override { return ! keyboardWasActive; }
    void closeLists() override { listsToClose = true; }

    // A control is reported only while the last frame drew all of it inside the
    // panel, clear of every other control: each Multiband parameter by its symbol,
    // "comp_enabled" the header and "comp_preset" the preset picker, whose point
    // is its centre. A dial's point runs up its face with the
    // fraction, so 0.5 is its centre and a move from there to 1 drags it up; "plot"
    // runs across the band display at the crossover handles' height; "meter_<band>"
    // runs down a playing band's gain-reduction bar, from the display's top to the
    // bar's foot.
    bool controlPointForScenario (const std::string& name, ImVec2& point, float fraction) const override
    {
        if (listPoint (presetList, name, point)) return true;
        const float f = std::clamp (fraction, 0.0f, 1.0f);
        if (name == "plot")
        {
            if (plot.empty()) return false;
            point = { plot.x0 + 0.5f + f * (plot.x1 - plot.x0 - 1.0f), handleRowY };
            return true;
        }
        for (int b = 0; b < kCompBandCount; ++b)
            if (name == "meter_" + std::to_string (b))
            {
                const auto& meter = meters[(std::size_t) b];
                if (! meter.drawn) return false;
                point = { meter.x, plot.y0 + f * (meter.foot - plot.y0) };
                return true;
            }
        for (const auto& control : controls)
            if (name == control.symbol)
            {
                if (! control.clean) return false;
                const auto& r = control.hit;
                const float inset = std::min (0.5f, 0.5f * (r.y1 - r.y0));
                point = { 0.5f * (r.x0 + r.x1), r.y1 - inset - f * (r.y1 - r.y0 - 2.0f * inset) };
                return true;
            }
        return false;
    }

    // "list:<n>": entry n of the open list, while it is open and all of that entry
    // lies inside the panel.
    bool listPoint (const std::vector<ImVec4>& list, const std::string& name, ImVec2& point) const
    {
        const auto entry = formListEntry (name, list.size());
        if (! entry) return false;
        const auto& item = list[*entry];
        const Rect box { item.x, item.y, item.z, item.w };
        if (box.empty() || ! box.inside (viewport)) return false;
        point = { 0.5f * (box.x0 + box.x1), 0.5f * (box.y0 + box.y1) };
        return true;
    }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        keyboardWasActive = keyboardActive;
        if (std::exchange (listsToClose, false))
            formCloseLists();
        presetList.clear();
        const ImVec2 br (origin.x + size.x, origin.y + size.y);
        ctx.dl->AddRectFilled (origin, br, rgba (kPanelFill));
        ctx.dl->AddRect ({ origin.x + 0.5f, origin.y + 0.5f }, { br.x - 0.5f, br.y - 0.5f }, rgba (kPanelBorder), 0.0f,
                         0, ctx.s (1.0f));
        viewport = { origin.x, origin.y, br.x, br.y };
        for (auto& control : controls)
        {
            control.drawn = {};
            control.hit = {};
            control.clean = false;
        }
        plot = {};
        meters = {};

        tickMeters();
        readBands();

        const float left = origin.x + ctx.s (kSideInset);
        const float right = br.x - ctx.s (kSideInset);
        float top = origin.y + ctx.s (kTopInset);
        const float bottom = br.y - ctx.s (kBottomInset);
        if (right - left > ctx.s (120.0f))
        {
            drawHeader (ctx, left, right, top);
            top += ctx.s (kHeaderH + kHeaderGap);
            drawPresetRow (ctx, left, right, top);
            top += ctx.s (kComboH + kSectionGap);
        }
        if (right - left > ctx.s (120.0f) && bottom - top > ctx.s (120.0f))
        {
            const auto layout = arrange (ctx, left, right, top, bottom);
            drawDisplay (ctx, { left, top, right, top + layout.displayH });
            float y = top + layout.displayH + ctx.s (kSectionGap);
            drawBands (ctx, left, right, y, layout);
            y += layout.bandsH + ctx.s (kSectionGap);
            drawStageRow (ctx, left, right, y, layout.cellH);
            drawEditField (ctx);
        }
        else
            editing = -1;
        settleControls();
        keyboardActive = editing >= 0 || ImGui::IsAnyItemActive()
                      || ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
    }

    std::optional<std::uint64_t> liveStamp() override
    {
        for (int b = 0; b < kCompBandCount; ++b)
        {
            const float target = bandTarget (b);
            if (std::abs (target - shownGr[(std::size_t) b]) / kGrRangeDb * barTravel > kBarSettle
                || std::lround (target * 10.0f) != std::lround (shownGr[(std::size_t) b] * 10.0f))
                return std::nullopt;
        }
        StampHash hash;
        for (const auto id : compStageParams (CompStage::Multiband))
            hash.add (comp.get (id));
        hash.add (params.compEnabled.load (std::memory_order_relaxed)).add (editing).add (presetChoice);
        for (const float gr : shownGr)
            hash.add (std::lround (gr * 10.0f));
        return hash.value;
    }

private:
    struct Control
    {
        CompParamId id;
        const char* symbol;
        // The whole control, captions and readouts included.
        Rect drawn;
        bool clean;
        // Where it takes input.
        Rect hit {};
    };

    struct Layout
    {
        float displayH = 0.0f;
        float bandsH = 0.0f;
        float cellH = 0.0f;
        int perRow = 1;
        int knobRows = kKnobsPerBand;
    };

    // As many dials to a row as gives them the largest face: one above another in a
    // tall narrow column, side by side when the column is wide or the panel short.
    Layout arrange (const dw::Context& ctx, float left, float right, float top, float bottom) const
    {
        const float columnW = ((right - left) - 3.0f * ctx.s (kColumnGap)) / (float) kCompBandCount;
        const float fixed = ctx.s (kNameRowH + kRowGap + kToggleRowH + kRowGap) + 2.0f * ctx.s (kSectionGap);
        const float room = (bottom - top) - fixed - ctx.s (kMinDisplayH);
        Layout layout;
        float bestRadius = -1.0f;
        const int widest = std::clamp ((int) (columnW / ctx.s (kMinCellW)), 1, kKnobsPerBand);
        for (int perRow = 1; perRow <= widest; ++perRow)
        {
            const int rows = (kKnobsPerBand + perRow - 1) / perRow;
            float cellH = std::clamp (room / (float) (rows + 1), ctx.s (kMinCellH), ctx.s (kMaxCellH));
            const float radius = knobRadius (ctx, columnW / (float) perRow, cellH);
            // A row no taller than its dial needs; the display takes the rest.
            cellH = std::max (ctx.s (kMinCellH), std::min (cellH, 2.0f * radius + ctx.s (kCaptionH + kValueH + 8.0f)));
            if (radius > bestRadius + 0.5f)
            {
                bestRadius = radius;
                layout.perRow = perRow;
                layout.knobRows = rows;
                layout.cellH = cellH;
            }
        }
        layout.bandsH = ctx.s (kNameRowH + kRowGap + kToggleRowH + kRowGap) + layout.cellH * (float) layout.knobRows;
        layout.displayH = std::max (ctx.s (30.0f), (bottom - top) - fixed - layout.cellH * (float) (layout.knobRows + 1));
        return layout;
    }

    static float knobRadius (const dw::Context& ctx, float cellW, float cellH)
    {
        return std::clamp (std::min ((cellH - ctx.s (kCaptionH + kValueH + 4.0f)) * 0.5f, cellW * 0.30f),
                           ctx.s (kMinRadius), ctx.s (kMaxRadius));
    }

    Control* find (CompParamId id)
    {
        for (auto& control : controls)
            if (control.id == id) return &control;
        return nullptr;
    }

    void noteNamed (const char* name, const Rect& at)
    {
        for (auto& control : controls)
            if (std::strcmp (control.symbol, name) == 0)
            {
                control.drawn = at;
                control.hit = at;
            }
    }

    // ---- the header and the presets ----------------------------------------------------------

    void drawHeader (dw::Context& ctx, float left, float right, float top)
    {
        const bool engaged = params.compEnabled.load (std::memory_order_relaxed);
        const auto pill = dw::modulePill (ctx, "##comp-header", { left, top }, { right, top + ctx.s (kHeaderH) },
                                          "MULTIBAND COMP", engaged, rgba (kHeaderAccent));
        formTooltip ("Click to engage or bypass the multiband compressor.");
        noteNamed (kEngageName, { left, top, right, top + ctx.s (kHeaderH) });
        if (pill.toggled || pill.labelClicked)
            params.compEnabled.store (! engaged, std::memory_order_relaxed);
    }

    // A one-shot apply: the picked name stays showing after a band is changed, and
    // picking it again applies the preset again.
    void drawPresetRow (dw::Context& ctx, float left, float right, float top)
    {
        const float comboW = std::min (ctx.s (kPresetComboW),
                                       (right - left) - ctx.s (kPresetCaptionW + 6.0f));
        if (comboW <= ctx.s (40.0f)) return;
        const float x0 = left + ((right - left) - ctx.s (kPresetCaptionW + 6.0f) - comboW) * 0.5f;
        dw::text (ctx, ctx.fonts->pill, ctx.s (10.0f), { x0, top + (ctx.s (kComboH) - ctx.s (10.0f)) * 0.5f },
                  ctx.s (kPresetCaptionW), rgba (kPresetCaption), "PRESET", dw::Align::right);
        const float comboX = x0 + ctx.s (kPresetCaptionW + 6.0f);
        int picked = std::clamp (presetChoice, 0, (int) presetItems.size() - 1);
        {
            const ScopedFormStyle style (ctx);
            // Tall enough for every preset at once, rather than the eight a list
            // shows before it scrolls.
            ImGui::SetNextWindowSizeConstraints ({ comboW, 0.0f }, { FLT_MAX, FLT_MAX });
            if (formCombo (ctx, "##comp-preset", { comboX, top }, { comboX + comboW, top + ctx.s (kComboH) },
                           presetItems.data(), (int) presetItems.size(), picked, true, &presetList, true)
                && picked > 0 && mbpresets::apply (picked - 1, stage, comp))
                presetChoice = picked;
            formTooltip ("Apply a 3-band mastering compressor preset. "
                         "Maps onto the multiband comp with the high-mid band disabled.");
        }
        noteNamed (kPresetName, { comboX, top, comboX + comboW, top + ctx.s (kComboH) });
    }

    void note (CompParamId id, const Rect& drawn, const Rect& hit)
    {
        if (auto* control = find (id))
        {
            control->drawn = drawn;
            control->hit = hit;
        }
    }

    void settleControls()
    {
        for (auto& control : controls)
        {
            control.clean = ! control.drawn.empty() && control.drawn.inside (viewport);
            for (const auto& other : controls)
                if (&other != &control && ! other.drawn.empty() && control.drawn.overlaps (other.drawn))
                    control.clean = false;
        }
    }

    // ---- the values -------------------------------------------------------------------------

    const std::string& text (CompParamId id, float value)
    {
        auto& entry = texts[(std::size_t) id];
        if (! entry.valid || ! sameValue (entry.value, value))
        {
            entry.text = stage.valueText (id, value);
            entry.value = value;
            entry.valid = true;
        }
        return entry.text;
    }

    bool band (int b, CompBandField field) const
    {
        return comp.get (compBandParam (b, field)) >= 0.5f;
    }

    // Which bands play: the enabled ones, made up to two with the lowest of the
    // others, as Multi-Comp 2 repairs the set it plays.
    void readBands()
    {
        enabledCount = 0;
        for (int b = 0; b < kCompBandCount; ++b)
        {
            enabled[(std::size_t) b] = band (b, CompBandField::Enabled);
            active[(std::size_t) b] = enabled[(std::size_t) b];
            enabledCount += enabled[(std::size_t) b] ? 1 : 0;
        }
        int count = enabledCount;
        for (int b = 0; b < kCompBandCount && count < 2; ++b)
            if (! active[(std::size_t) b])
            {
                active[(std::size_t) b] = true;
                ++count;
            }
        anySolo = false;
        for (int b = 0; b < kCompBandCount; ++b)
            anySolo = anySolo || (active[(std::size_t) b] && band (b, CompBandField::Solo));
        readCrossovers();
    }

    // The crossovers as the plug-in plays them, which keeps each one clear of the one
    // below: its own instance, which never runs, is handed the three settings and
    // asked, as its editor asks.
    void readCrossovers()
    {
        for (int c = 0; c < 3; ++c)
        {
            const float set = comp.get (kCrossovers[c]);
            if (probeIndex[(std::size_t) c] >= 0)
                probe.plugin().setParameterValue ((std::uint32_t) probeIndex[(std::size_t) c], set);
        }
        for (int c = 0; c < 3; ++c)
        {
            const float host = probeIndex[(std::size_t) c] >= 0
                             ? probe.editorReading (probeIndex[(std::size_t) c])
                             : comp.get (kCrossovers[c]);
            auto& cached = played[(std::size_t) c];
            if (! cached.valid || ! sameValue (cached.host, host))
            {
                cached.host = host;
                cached.hz = hzForHost (kCrossovers[c], host);
                cached.valid = true;
            }
        }
    }

    // The host value the plug-in reads a whole number of hertz as.
    float hostAtHz (CompParamId id, int hz) const
    {
        float value = compParamSpec (id).hostMin;
        stage.valueFromText (id, std::to_string (hz), value);
        return value;
    }

    float hostForHz (CompParamId id, float hz) const
    {
        const float clamped = std::clamp (hz, 1.0f, 30000.0f);
        const int whole = (int) std::floor (clamped);
        const float below = hostAtHz (id, whole);
        const float above = hostAtHz (id, whole + 1);
        return below + (clamped - (float) whole) * (above - below);
    }

    // The hertz a host value plays, found through the plug-in's own reading of typed
    // hertz, so the control's taper is the plug-in's to say.
    float hzForHost (CompParamId id, float host) const
    {
        static constexpr int kLowest = 1, kHighest = 30000;
        const float floorValue = hostAtHz (id, kLowest);
        const float ceilingValue = hostAtHz (id, kHighest);
        // The last whole hertz whose host value is at most `limit`.
        const auto lastAtMost = [this, id] (float limit)
        {
            int lo = kLowest, hi = kHighest;
            while (hi - lo > 1)
            {
                const int mid = lo + (hi - lo) / 2;
                (hostAtHz (id, mid) <= limit ? lo : hi) = mid;
            }
            return lo;
        };
        if (! (host > floorValue))
            return (float) lastAtMost (floorValue);
        if (! (host < ceilingValue))
        {
            int lo = kLowest, hi = kHighest;
            while (hi - lo > 1)
            {
                const int mid = lo + (hi - lo) / 2;
                (hostAtHz (id, mid) >= ceilingValue ? hi : lo) = mid;
            }
            return (float) hi;
        }
        const int below = lastAtMost (host);
        const float a = hostAtHz (id, below), b = hostAtHz (id, below + 1);
        return (float) below + (b > a ? (host - a) / (b - a) : 0.0f);
    }

    float bandTarget (int b) const
    {
        if (! params.compEnabled.load (std::memory_order_relaxed)) return 0.0f;
        return std::clamp (stage.bandGainReductionDb (b), -kGrRangeDb * 4.0f, 0.0f);
    }

    void tickMeters()
    {
        meterClock += ImGui::GetIO().DeltaTime;
        if (meterClock < kMeterInterval) return;
        meterClock = 0.0f;
        for (int b = 0; b < kCompBandCount; ++b)
        {
            const float target = bandTarget (b);
            auto& shown = shownGr[(std::size_t) b];
            if (target < shown)
                shown = target;
            else
                shown += (target - shown) * 0.18f;
            if (std::abs (target - shown) < 0.01f)
                shown = target;
        }
    }

    void set (CompParamId id, float value)
    {
        comp.set (id, value);
    }

    // ---- the band display --------------------------------------------------------------------

    float freqToX (float hz) const
    {
        const float frac = std::log10 (std::clamp (hz, kFreqMinHz, kFreqMaxHz) / kFreqMinHz)
                         / std::log10 (kFreqMaxHz / kFreqMinHz);
        return plot.x0 + frac * (plot.x1 - plot.x0);
    }

    float xToFreq (float x) const
    {
        const float width = plot.x1 - plot.x0;
        const float frac = width > 0.0f ? std::clamp ((x - plot.x0) / width, 0.0f, 1.0f) : 0.0f;
        return kFreqMinHz * std::pow (kFreqMaxHz / kFreqMinHz, frac);
    }

    float grToY (float db) const
    {
        return plot.y0 + std::clamp (-db / kGrRangeDb, 0.0f, 1.0f) * (plot.y1 - plot.y0);
    }

    // A crossover is a split the plug-in plays when the band above it plays and
    // some band below it does.
    bool crossoverPlays (int c) const
    {
        if (! active[(std::size_t) c + 1]) return false;
        for (int b = 0; b <= c; ++b)
            if (active[(std::size_t) b]) return true;
        return false;
    }

    void drawDisplay (dw::Context& ctx, Rect area)
    {
        auto& dl = *ctx.dl;
        plot = area;
        dl.AddRectFilled ({ plot.x0, plot.y0 }, { plot.x1, plot.y1 }, rgba (kPlotFill), ctx.s (3.0f));
        barTravel = plot.y1 - plot.y0;

        // Each playing band's region runs from the split below it to the next
        // playing band's split.
        const bool engaged = params.compEnabled.load (std::memory_order_relaxed);
        for (int b = 0; b < kCompBandCount; ++b)
        {
            if (! active[(std::size_t) b]) continue;
            float lowHz = kFreqMinHz, highHz = kFreqMaxHz;
            for (int below = b - 1; below >= 0; --below)
                if (active[(std::size_t) below]) { lowHz = played[(std::size_t) b - 1].hz; break; }
            for (int above = b + 1; above < kCompBandCount; ++above)
                if (active[(std::size_t) above]) { highHz = played[(std::size_t) above - 1].hz; break; }
            const float x0 = freqToX (lowHz), x1 = freqToX (highHz);
            if (x1 - x0 < 1.0f) continue;
            const bool bypassed = band (b, CompBandField::Bypass);
            const bool soloed = band (b, CompBandField::Solo);
            const bool heard = ! anySolo || soloed;
            const ImU32 colour = rgba (kBandColours[b]);
            const float tint = ! engaged || bypassed || ! heard ? 0.05f : 0.14f;
            dl.AddRectFilled ({ x0, plot.y0 }, { x1, plot.y1 }, dw::withAlpha (colour, tint));
            if (soloed)
            {
                dl.AddRect ({ x0 + 1.0f, plot.y0 + 1.0f }, { x1 - 1.0f, plot.y1 - 1.0f },
                            dw::withAlpha (colour, 0.9f), 0.0f, 0, ctx.s (1.5f));
                dw::text (ctx, ctx.fonts->pill, ctx.s (9.0f), { x0, plot.y0 + barTravel * 0.5f + ctx.s (7.0f) },
                          x1 - x0, rgba (0xd4d400ff), "SOLO");
            }
            if (bypassed && x1 - x0 > ctx.s (28.0f))
                dw::text (ctx, ctx.fonts->pill, ctx.s (9.0f), { x0, plot.y0 + barTravel * 0.5f - ctx.s (5.0f) },
                          x1 - x0, dw::withAlpha (rgba (kBypassLit), 0.8f), "BYP");

            const float gr = shownGr[(std::size_t) b];
            const float inset = std::min (ctx.s (3.0f), (x1 - x0) * 0.15f);
            const float barBottom = grToY (gr);
            meters[(std::size_t) b] = { 0.5f * (x0 + x1), barBottom, true };
            if (barBottom - plot.y0 > 0.5f)
                dl.AddRectFilledMultiColor ({ x0 + inset, plot.y0 }, { x1 - inset, barBottom },
                                            dw::withAlpha (colour, 0.55f), dw::withAlpha (colour, 0.55f),
                                            dw::withAlpha (colour, 0.95f), dw::withAlpha (colour, 0.95f));
        }

        for (const float db : { -3.0f, -6.0f, -10.0f, -15.0f })
            dl.AddLine ({ plot.x0, grToY (db) }, { plot.x1, grToY (db) }, rgba (kGridLine), ctx.s (1.0f));
        for (const float hz : { 100.0f, 1000.0f, 10000.0f })
            dl.AddLine ({ freqToX (hz), plot.y0 }, { freqToX (hz), plot.y1 }, rgba (kGridLine), ctx.s (1.0f));

        const float decadeSize = ctx.s (9.0f);
        for (const auto& entry : { std::pair<float, const char*> { 100.0f, "100" },
                                   std::pair<float, const char*> { 1000.0f, "1k" },
                                   std::pair<float, const char*> { 10000.0f, "10k" } })
            dw::text (ctx, ctx.fonts->caption, decadeSize,
                      { freqToX (entry.first) - ctx.s (18.0f), plot.y1 - ctx.s (11.0f) }, ctx.s (36.0f),
                      rgba (kDecadeText), entry.second);

        const float scaleSize = ctx.s (9.0f);
        float lastLabel = -1.0e9f;
        for (const auto& entry : { std::pair<float, const char*> { 0.0f, "0" },
                                   std::pair<float, const char*> { -3.0f, "-3" },
                                   std::pair<float, const char*> { -6.0f, "-6" },
                                   std::pair<float, const char*> { -10.0f, "-10" },
                                   std::pair<float, const char*> { -15.0f, "-15" } })
        {
            const float y = std::max (plot.y0 + ctx.s (1.0f), grToY (entry.first) - scaleSize * 0.5f);
            if (y - lastLabel < scaleSize * 1.4f || y > plot.y1 - ctx.s (40.0f)) continue;
            lastLabel = y;
            dw::text (ctx, ctx.fonts->caption, scaleSize, { plot.x1 - ctx.s (24.0f), y }, ctx.s (21.0f),
                      rgba (kScaleText), entry.second, dw::Align::right);
        }
        if (! engaged)
            dw::text (ctx, ctx.fonts->pill, ctx.s (10.0f), { plot.x0, plot.y0 + ctx.s (5.0f) },
                      plot.x1 - plot.x0, rgba (kOutText), "BYPASS");

        drawCrossovers (ctx);
    }

    void drawCrossovers (dw::Context& ctx)
    {
        auto& dl = *ctx.dl;
        const float labelSize = ctx.s (9.5f);
        // Two rows of labels where the display has room, so the middle split's
        // reads clear of its neighbours'; one row on a display too short for that.
        const float rowA = std::max (plot.y0 + ctx.s (1.0f), plot.y1 - ctx.s (12.0f) - ctx.s (kLabelRowH));
        const float rowB = plot.y1 - plot.y0 >= ctx.s (kMinDisplayH) ? rowA - ctx.s (kLabelRowH + 2.0f) : rowA;
        handleRowY = rowA + ctx.s (kLabelRowH) * 0.5f;

        struct Handle
        {
            float x = 0.0f, width = 0.0f;
            Rect box;
            bool hovered = false, held = false;
        };
        std::array<Handle, 3> handles {};
        for (int c = 0; c < 3; ++c)
        {
            auto& handle = handles[(std::size_t) c];
            handle.x = freqToX (played[(std::size_t) c].hz);
            handle.width = textWidth (ctx.fonts->pill, labelSize, text (kCrossovers[c], played[(std::size_t) c].host).c_str())
                         + ctx.s (10.0f);
            const float labelY = c == 1 ? rowB : rowA;
            const float labelX = std::clamp (handle.x - handle.width * 0.5f, plot.x0 + ctx.s (1.0f),
                                             plot.x1 - ctx.s (1.0f) - handle.width);
            handle.box = { labelX, labelY, labelX + handle.width, labelY + ctx.s (kLabelRowH) };
        }

        // Each split takes presses across the display's height, so close splits'
        // areas overlap, and Dear ImGui gives a press to the first area submitted
        // under the pointer. The split whose label the pointer is on goes first, the
        // label drawn on top where two overlap, then the one whose line is nearest.
        const ImVec2 mouse = ImGui::GetIO().MousePos;
        const auto priority = [&handles, mouse] (int c)
        {
            const auto& handle = handles[(std::size_t) c];
            const auto& b = handle.box;
            if (mouse.x >= b.x0 && mouse.x < b.x1 && mouse.y >= b.y0 && mouse.y < b.y1)
                return -1.0f - (float) c;
            return std::abs (mouse.x - handle.x);
        };
        std::array<int, 3> order { 0, 1, 2 };
        std::stable_sort (order.begin(), order.end(), [&priority] (int a, int b) { return priority (a) < priority (b); });
        for (const int c : order)
        {
            auto& handle = handles[(std::size_t) c];
            const Rect strip { std::min (handle.box.x0, handle.x - ctx.s (5.0f)), plot.y0,
                               std::max (handle.box.x1, handle.x + ctx.s (5.0f)), plot.y1 };
            char hitId[24];
            std::snprintf (hitId, sizeof hitId, "##mb-xover%d", c + 1);
            handle.hovered = dw::hitArea (ctx, hitId, { strip.x0, strip.y0 }, { strip.x1, strip.y1 });
            formTooltip ("Crossover: drag sideways, double-click to reset. "
                         "Cmd-click on macOS or Ctrl-click elsewhere to type a frequency.");
            handle.held = ImGui::IsItemActive();
            handleCrossoverInput (c, handle.x, handle.box);
        }

        for (int c = 0; c < 3; ++c)
        {
            const auto id = kCrossovers[c];
            const auto& handle = handles[(std::size_t) c];
            const float x = handle.x;
            const auto& box = handle.box;
            const bool plays = crossoverPlays (c);
            const ImU32 lineColour = plays ? rgba (kHandleLine) : rgba (kHandleIdle);
            if (plays)
                dl.AddLine ({ x, plot.y0 }, { x, plot.y1 }, lineColour,
                            ctx.s (handle.held || handle.hovered ? 2.0f : 1.4f));
            else
                for (float y = plot.y0; y < plot.y1; y += ctx.s (6.0f))
                    dl.AddLine ({ x, y }, { x, std::min (plot.y1, y + ctx.s (3.0f)) }, lineColour, ctx.s (1.0f));
            dl.AddRectFilled ({ box.x0, box.y0 }, { box.x1, box.y1 }, rgba (kLabelFill), ctx.s (3.0f));
            dl.AddRect ({ box.x0, box.y0 }, { box.x1, box.y1 }, lineColour, ctx.s (3.0f), 0,
                        ctx.s (handle.held ? 1.4f : 0.8f));
            dw::text (ctx, ctx.fonts->pill, labelSize, { box.x0, box.y0 + (ctx.s (kLabelRowH) - labelSize) * 0.5f },
                      handle.width, plays ? rgba (kLabelText) : rgba (kSwitchText),
                      text (id, played[(std::size_t) c].host).c_str());
            note (id, box, box);
            if (editing == (int) id)
                editRect = box;
        }
    }

    void handleCrossoverInput (int c, float x, const Rect& box)
    {
        const auto id = kCrossovers[c];
        auto& io = ImGui::GetIO();
        if (ImGui::IsItemActivated())
        {
            dragOffset = io.MousePos.x - x;
            gestureTypes = io.KeyCtrl || io.KeySuper;
            resetHeld = false;
            if (gestureTypes)
                beginEdit (id, played[(std::size_t) c].host, box);
        }
        if (! ImGui::IsItemActive())
            return;
        if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
        {
            resetHeld = true;
            set (id, compParamSpec (id).hostDefault);
            return;
        }
        if (resetHeld || gestureTypes || ! ImGui::IsMouseDragging (ImGuiMouseButton_Left, 2.0f))
            return;
        const float hz = xToFreq (io.MousePos.x - dragOffset);
        set (id, hostForHz (id, hz));
    }

    // ---- the band columns --------------------------------------------------------------------

    void drawBands (dw::Context& ctx, float left, float right, float top, const Layout& layout)
    {
        const float columnW = ((right - left) - 3.0f * ctx.s (kColumnGap)) / (float) kCompBandCount;
        for (int b = 0; b < kCompBandCount; ++b)
        {
            const float x0 = left + (float) b * (columnW + ctx.s (kColumnGap));
            drawBandColumn (ctx, b, x0, x0 + columnW, top, layout);
        }
    }

    void drawBandColumn (dw::Context& ctx, int b, float x0, float x1, float top, const Layout& layout)
    {
        auto& dl = *ctx.dl;
        const ImU32 colour = rgba (kBandColours[b]);
        const bool on = enabled[(std::size_t) b];
        const bool plays = active[(std::size_t) b];
        const float width = x1 - x0;

        dl.AddRectFilled ({ x0, top }, { x1, top + layout.bandsH }, dw::withAlpha (colour, plays ? 0.05f : 0.02f),
                          ctx.s (3.0f));

        // The name chip, and the band's gain reduction beside it.
        const float grW = std::min (ctx.s (36.0f), width * 0.36f);
        const float chipR = x1 - grW - ctx.s (2.0f);
        dl.AddRectFilled ({ x0, top }, { chipR, top + ctx.s (kNameRowH) },
                          plays ? dw::withAlpha (colour, 0.9f) : dw::withAlpha (colour, 0.3f), ctx.s (3.0f));
        dw::text (ctx, ctx.fonts->pill, ctx.s (10.0f), { x0, top + ctx.s (3.0f) }, chipR - x0,
                  plays ? ctx.theme->textOn : rgba (kSwitchText), kBandNames[b]);
        char gr[16];
        const float shown = shownGr[(std::size_t) b];
        std::snprintf (gr, sizeof gr, "%.1f", (double) (shown > -0.05f ? 0.0f : shown));
        dw::text (ctx, ctx.fonts->pill, ctx.s (9.5f), { chipR, top + ctx.s (3.0f) }, x1 - chipR,
                  rgba (kGrText), gr, dw::Align::right);

        // ON / S / BYP.
        float y = top + ctx.s (kNameRowH + kRowGap);
        const float switchW = (width - 2.0f * ctx.s (2.0f)) / 3.0f;
        const auto toggle = [&] (int slot, CompBandField field, const char* label, ImU32 lit, bool locked,
                                 const char* help)
        {
            const auto id = compBandParam (b, field);
            const bool value = band (b, field);
            const float sx = x0 + (float) slot * (switchW + ctx.s (2.0f));
            const Rect r { sx, y, sx + switchW, y + ctx.s (kToggleRowH) };
            char buttonId[32];
            std::snprintf (buttonId, sizeof buttonId, "##%s", compParamSpec (id).symbol);
            dw::ButtonStyle style;
            style.offFill = rgba (kSwitchOff);
            style.onFill = locked ? dw::withAlpha (lit, 0.55f) : lit;
            style.offText = rgba (kSwitchText);
            style.onText = ctx.theme->textOn;
            style.fontSize = 9.5f;
            const auto clicked = dw::textButton (ctx, buttonId, { r.x0, r.y0 }, { r.x1, r.y1 }, label, value, style).clicked;
            formTooltip (help);
            note (id, r, r);
            return clicked && ! locked;
        };
        char help[96];
        const bool locked = on && enabledCount <= 2;
        if (locked)
            std::snprintf (help, sizeof help, "Multiband requires at least 2 enabled bands");
        else
            std::snprintf (help, sizeof help, "Enable / disable the %s band", kBandTitles[b]);
        if (toggle (0, CompBandField::Enabled, "ON", colour, locked, help))
        {
            // A band switched off drops its solo, as Multi-Comp 2's own face does.
            if (on && band (b, CompBandField::Solo))
                set (compBandParam (b, CompBandField::Solo), 0.0f);
            set (compBandParam (b, CompBandField::Enabled), on ? 0.0f : 1.0f);
        }
        std::snprintf (help, sizeof help, "Solo the %s band", kBandTitles[b]);
        if (toggle (1, CompBandField::Solo, "S", rgba (0xd4d400ff), false, help))
            set (compBandParam (b, CompBandField::Solo), band (b, CompBandField::Solo) ? 0.0f : 1.0f);
        std::snprintf (help, sizeof help, "Bypass the %s band: it passes unprocessed", kBandTitles[b]);
        if (toggle (2, CompBandField::Bypass, "BYP", rgba (kBypassLit), false, help))
            set (compBandParam (b, CompBandField::Bypass), band (b, CompBandField::Bypass) ? 0.0f : 1.0f);

        y += ctx.s (kToggleRowH + kRowGap);
        const float cellW = width / (float) layout.perRow;
        const ImU32 knobColour = plays ? colour : dw::withAlpha (colour, 0.45f);
        for (int k = 0; k < kKnobsPerBand; ++k)
        {
            const int row = k / layout.perRow, column = k % layout.perRow;
            const int inRow = std::min (layout.perRow, kKnobsPerBand - row * layout.perRow);
            // A short last row centres under the full ones.
            const float rowLeft = x0 + (width - cellW * (float) inRow) * 0.5f;
            drawKnob (ctx, compBandParam (b, kKnobFields[k]), kKnobCaptions[k],
                      rowLeft + (float) column * cellW, y + (float) row * layout.cellH, cellW, layout.cellH,
                      knobColour);
        }
    }

    // ---- the stage-wide row ------------------------------------------------------------------

    void drawStageRow (dw::Context& ctx, float left, float right, float top, float cellH)
    {
        const float cellW = std::min (ctx.s (110.0f), (right - left) / 3.0f);
        const float start = left + ((right - left) - 3.0f * cellW) * 0.5f;
        const ImU32 accent = rgba (kStageAccent);
        drawKnob (ctx, CompParamId::MbMix, "MIX", start, top, cellW, cellH, accent);
        drawKnob (ctx, CompParamId::MbOutput, "OUTPUT", start + cellW, top, cellW, cellH, accent);

        const auto id = CompParamId::AutoMakeup;
        const bool on = comp.get (id) >= 0.5f;
        const float bx0 = start + 2.0f * cellW + ctx.s (4.0f), bx1 = start + 3.0f * cellW - ctx.s (4.0f);
        const float buttonH = ctx.s (kToggleRowH + 4.0f);
        const float by0 = top + (cellH - buttonH) * 0.5f;
        dw::ButtonStyle style;
        style.offFill = rgba (kSwitchOff);
        style.onFill = accent;
        style.offText = rgba (kSwitchText);
        style.onText = ctx.theme->textOn;
        style.fontSize = 9.5f;
        if (dw::textButton (ctx, "##auto_makeup", { bx0, by0 }, { bx1, by0 + buttonH }, "AUTO MAKEUP", on, style).clicked)
            set (id, on ? 0.0f : 1.0f);
        formTooltip ("Auto makeup: Multi-Comp 2 makes up the level each band's compression takes off.");
        const Rect r { bx0, by0, bx1, by0 + buttonH };
        note (id, r, r);
    }

    // ---- one dial ----------------------------------------------------------------------------

    void drawKnob (dw::Context& ctx, CompParamId id, const char* caption, float x0, float y0, float cellW,
                   float cellH, ImU32 colour)
    {
        const auto& spec = compParamSpec (id);
        const float value = comp.get (id);
        const float captionSize = ctx.s (9.0f);
        const float valueSize = ctx.s (10.5f);
        const float radius = knobRadius (ctx, cellW, cellH);
        const float centreX = x0 + cellW * 0.5f;
        const float centreY = y0 + ctx.s (kCaptionH + 1.0f) + radius;

        dw::text (ctx, ctx.fonts->caption, captionSize, { x0, y0 + ctx.s (1.0f) }, cellW, rgba (kCaptionText),
                  caption);

        const auto& readout = text (id, value);
        dw::KnobStyle style;
        style.fill = colour;
        style.value = readout.c_str();
        char knobId[32];
        std::snprintf (knobId, sizeof knobId, "##%s", spec.symbol);
        const auto result = dw::knob (ctx, knobId, { centreX, centreY }, radius, value,
                                      dw::Range (spec.hostMin, spec.hostMax), spec.hostDefault, style);
        formTooltip (kKnobHelp);
        auto& io = ImGui::GetIO();
        const float readoutW = std::max (textWidth (ctx.fonts->label, valueSize, readout.c_str()), radius * 2.0f);
        const Rect valueBox { centreX - readoutW * 0.5f - ctx.s (2.0f), centreY + radius + ctx.s (1.0f),
                              centreX + readoutW * 0.5f + ctx.s (2.0f), centreY + radius + ctx.s (kValueH) };
        if (ImGui::IsItemActivated())
        {
            gestureTypes = io.KeyCtrl || io.KeySuper;
            resetHeld = false;
            if (gestureTypes)
                beginEdit (id, value, valueBox);
        }
        // The press that made a double-click stays down for a few frames, and the
        // dial would drag the value back to where that press found it.
        if (result.hovered && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left) && ! gestureTypes)
        {
            resetHeld = true;
            set (id, spec.hostDefault);
        }
        else if (result.changed && ! (ImGui::IsItemActive() && (gestureTypes || resetHeld)))
            set (id, std::clamp (result.value, spec.hostMin, spec.hostMax));

        const float captionW = textWidth (ctx.fonts->caption, captionSize, caption);
        const Rect captionBox { centreX - captionW * 0.5f, y0 + ctx.s (1.0f), centreX + captionW * 0.5f,
                                y0 + ctx.s (1.0f) + captionSize };
        const Rect dial { centreX - radius, centreY - radius, centreX + radius, centreY + radius };
        note (id, captionBox.unite (dial).unite (valueBox), dial);
        if (editing == (int) id)
            editRect = valueBox;
    }

    // ---- typed values ------------------------------------------------------------------------

    void beginEdit (CompParamId id, float value, const Rect& at)
    {
        editing = (int) id;
        editTakesFocus = true;
        editRect = at;
        std::snprintf (editBuffer, sizeof editBuffer, "%s", text (id, value).c_str());
    }

    void drawEditField (dw::Context& ctx)
    {
        if (editing < 0) return;
        const float width = std::max (ctx.s (64.0f), editRect.x1 - editRect.x0);
        const float centre = 0.5f * (editRect.x0 + editRect.x1);
        const float x0 = std::clamp (centre - width * 0.5f, viewport.x0, viewport.x1 - width);
        const float height = ctx.s (18.0f);
        const float y0 = std::clamp (editRect.y0 - ctx.s (2.0f), viewport.y0, viewport.y1 - height);
        const ScopedFormStyle style (ctx);
        const auto result = dw::textField (ctx, "##mb-edit", { x0, y0 }, { x0 + width, y0 + height }, editBuffer,
                                           sizeof editBuffer, editTakesFocus);
        editTakesFocus = false;
        if (result.committed)
        {
            const auto id = (CompParamId) editing;
            float value = 0.0f;
            if (stage.valueFromText (id, editBuffer, value))
                set (id, value);
            editing = -1;
        }
        else if (result.cancelled)
            editing = -1;
    }

    MasteringParams& params;
    CompParams& comp;
    const MultiCompStage& stage;
    int& presetChoice;
    std::vector<const char*> presetItems;
    std::vector<ImVec4> presetList;
    MultiCompStage probe { CompStage::Multiband };
    std::array<int, 3> probeIndex { -1, -1, -1 };

    struct Played
    {
        float host = 0.0f;
        float hz = kFreqMinHz;
        bool valid = false;
    };
    std::array<Played, 3> played {};

    struct Text
    {
        float value = 0.0f;
        std::string text;
        bool valid = false;
    };
    std::array<Text, kCompParamCount> texts {};

    std::vector<Control> controls;
    Rect viewport, plot, editRect;
    float handleRowY = 0.0f;
    std::array<bool, kCompBandCount> enabled {}, active {};
    int enabledCount = 0;
    bool anySolo = false;

    std::array<float, kCompBandCount> shownGr {};
    struct Meter
    {
        float x = 0.0f, foot = 0.0f;
        bool drawn = false;
    };
    std::array<Meter, kCompBandCount> meters {};
    float meterClock = 0.0f;
    float barTravel = 0.0f;

    float dragOffset = 0.0f;
    bool gestureTypes = false;
    bool resetHeld = false;
    int editing = -1;
    bool editTakesFocus = false;
    char editBuffer[32] {};
    bool keyboardActive = false, keyboardWasActive = false;
    bool listsToClose = false;
};
} // namespace

std::unique_ptr<DuskPanelView> makeMasteringCompView (MasteringParams& params, const MultiCompStage& stage,
                                                     int& presetChoice)
{
    return std::make_unique<MasteringCompViewImpl> (params, stage, presetChoice);
}
} // namespace duskstudio::imgui
