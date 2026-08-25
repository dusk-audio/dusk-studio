#include "ChannelStripView.h"
#include "StripTheme.h"

#include <DearImGui/imgui_internal.h>

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace duskspike
{
namespace
{

constexpr float kRotaryStart = -2.35619449f;
constexpr float kRotaryEnd   =  2.35619449f;

ImU32 col (std::uint32_t argb, float alpha = 1.0f)
{
    const int a = static_cast<int> (((argb >> 24) & 0xff) * alpha);
    return IM_COL32 ((argb >> 16) & 0xff, (argb >> 8) & 0xff, argb & 0xff, a);
}

// JUCE's Colour::brighter / ::darker, so the knob dome and the cap satin land on the same
// shades the LookAndFeel produces rather than something merely similar.
std::uint32_t brighter (std::uint32_t argb, float amount)
{
    const float k = 1.0f / (1.0f + amount);
    auto ch = [k] (int c) { return 255 - static_cast<int> ((255 - c) * k); };
    return (argb & 0xff000000u)
         | (static_cast<std::uint32_t> (ch ((argb >> 16) & 0xff)) << 16)
         | (static_cast<std::uint32_t> (ch ((argb >> 8) & 0xff)) << 8)
         |  static_cast<std::uint32_t> (ch (argb & 0xff));
}

std::uint32_t darker (std::uint32_t argb, float amount)
{
    const float k = 1.0f / (1.0f + amount);
    auto ch = [k] (int c) { return static_cast<int> (c * k); };
    return (argb & 0xff000000u)
         | (static_cast<std::uint32_t> (ch ((argb >> 16) & 0xff)) << 16)
         | (static_cast<std::uint32_t> (ch ((argb >> 8) & 0xff)) << 8)
         |  static_cast<std::uint32_t> (ch (argb & 0xff));
}

std::uint32_t lerpArgb (std::uint32_t a, std::uint32_t b, float t)
{
    auto ch = [a, b, t] (int shift)
    {
        const float x = static_cast<float> ((a >> shift) & 0xff);
        const float y = static_cast<float> ((b >> shift) & 0xff);
        return static_cast<std::uint32_t> (x + (y - x) * t) & 0xffu;
    };
    return 0xff000000u | (ch (16) << 16) | (ch (8) << 8) | ch (0);
}

enum class Align { left, centre, right };

void text (ImDrawList& dl, ImFont* font, float size, ImVec2 at, float width,
           std::uint32_t colour, const char* str, Align align = Align::centre)
{
    if (font == nullptr || str == nullptr || *str == 0)
        return;

    const ImVec2 extent = font->CalcTextSizeA (size, FLT_MAX, 0.0f, str);
    float x = at.x;
    if (align == Align::centre) x = at.x + (width - extent.x) * 0.5f;
    else if (align == Align::right) x = at.x + width - extent.x;

    dl.AddText (font, size, ImVec2 (std::round (x), std::round (at.y)), col (colour), str);
}

void panel (ImDrawList& dl, ImVec2 tl, ImVec2 br, std::uint32_t fill,
            std::uint32_t border, float borderAlpha, float radius, float thickness)
{
    dl.AddRectFilled (tl, br, col (fill), radius);
    dl.AddRect (tl, br, col (border, borderAlpha), radius, 0, thickness);
}

// The dome from DuskStudioLookAndFeel::drawSslKnob. ImDrawList has no radial gradient, so
// the body is a stack of concentric circles marching toward the highlight point; twelve
// steps is where the banding stops being visible at these radii.
void drawSslKnob (ImDrawList& dl, ImVec2 centre, float radius, float norm,
                  std::uint32_t fill)
{
    const float r = radius - 2.0f;
    if (r <= 1.0f)
        return;

    const float bodyR = r * 0.94f;
    const float angle = kRotaryStart + norm * (kRotaryEnd - kRotaryStart);

    for (int i = 0; i < 11; ++i)
    {
        const float t = kRotaryStart + (kRotaryEnd - kRotaryStart) * (i / 10.0f);
        const float dotR = r * 0.965f;
        const ImVec2 p (centre.x + std::sin (t) * dotR, centre.y - std::cos (t) * dotR);
        dl.AddCircleFilled (p, std::max (0.9f, r * 0.06f), col (theme::kKnobDot), 8);
    }

    dl.AddCircleFilled (ImVec2 (centre.x, centre.y + bodyR * 0.25f), bodyR + 3.0f,
                        IM_COL32 (0, 0, 0, 40), 24);

    const std::uint32_t hi = brighter (fill, 0.20f);
    const std::uint32_t lo = darker (fill, 0.40f);
    const ImVec2 hiCentre (centre.x - bodyR * 0.45f, centre.y - bodyR * 0.50f);

    constexpr int kSteps = 12;
    for (int i = 0; i < kSteps; ++i)
    {
        const float t = i / static_cast<float> (kSteps - 1);
        const float ringR = bodyR * (1.0f - t * 0.92f);
        const ImVec2 c (centre.x + (hiCentre.x - centre.x) * t,
                        centre.y + (hiCentre.y - centre.y) * t);
        dl.AddCircleFilled (c, ringR, col (lerpArgb (lo, hi, t)), 32);
    }

    dl.AddEllipseFilled (ImVec2 (centre.x, centre.y - bodyR * 0.60f),
                         ImVec2 (bodyR * 0.70f, bodyR * 0.27f),
                         IM_COL32 (255, 255, 255, 0x24), 0.0f, 20);

    dl.AddCircle (centre, bodyR, col (theme::kKnobRim), 32, 1.0f);

    const ImVec2 dir (std::sin (angle), -std::cos (angle));
    dl.AddLine (ImVec2 (centre.x + dir.x * bodyR * 0.82f, centre.y + dir.y * bodyR * 0.82f),
                ImVec2 (centre.x + dir.x * bodyR, centre.y + dir.y * bodyR),
                IM_COL32 (0x0c, 0x0c, 0x0e, 255), std::max (2.4f, r * 0.13f));
    dl.AddLine (ImVec2 (centre.x + dir.x * bodyR * 0.16f, centre.y + dir.y * bodyR * 0.16f),
                ImVec2 (centre.x + dir.x * bodyR * 0.76f, centre.y + dir.y * bodyR * 0.76f),
                IM_COL32 (255, 255, 255, 255), std::max (2.0f, r * 0.10f));
}

void formatFreq (char* out, std::size_t n, float hz)
{
    if (hz >= 1000.0f)
    {
        const float k = hz / 1000.0f;
        if (k >= 10.0f) std::snprintf (out, n, "%.0fk", k);
        else            std::snprintf (out, n, "%.1fk", k);
    }
    else
    {
        std::snprintf (out, n, "%.0f", hz);
    }
}

void formatGain (char* out, std::size_t n, float db)
{
    if (std::fabs (db - std::round (db)) < 0.05f) std::snprintf (out, n, "%+.0f", db);
    else                                          std::snprintf (out, n, "%+.1f", db);
}

} // namespace

// --------------------------------------------------------------------------------------

namespace
{

struct Ctx
{
    ImDrawList* dl = nullptr;
    float scale = 1.0f;
    const StripFonts* fonts = nullptr;
    std::string* activeDrag = nullptr;
    float* dragStartValue = nullptr;
    int widgets = 0;

    float s (float v) const noexcept { return v * scale; }
};

bool hitArea (const char* id, ImVec2 tl, ImVec2 br)
{
    ImGui::SetCursorScreenPos (tl);
    ImGui::InvisibleButton (id, ImVec2 (std::max (1.0f, br.x - tl.x),
                                        std::max (1.0f, br.y - tl.y)));
    return ImGui::IsItemHovered();
}

// One knob: hit area, vertical drag with a shift-fine modifier, wheel, double-click reset,
// the dome, its caption and its value readout. Returns true while it owns the mouse.
bool knob (Ctx& ctx, const char* id, ImVec2 centre, float radius,
           std::atomic<float>& value, const Range& range, float defaultValue,
           std::uint32_t fill, const char* caption, const char* valueText,
           std::uint32_t outline = theme::kKnobOutline)
{
    ++ctx.widgets;

    const ImVec2 tl (centre.x - radius, centre.y - radius);
    const ImVec2 br (centre.x + radius, centre.y + radius);
    hitArea (id, tl, br);

    float v = value.load (std::memory_order_relaxed);
    bool owned = false;

    if (ImGui::IsItemActive() && ImGui::IsMouseDragging (ImGuiMouseButton_Left, 0.0f))
    {
        if (*ctx.activeDrag != id)
        {
            *ctx.activeDrag = id;
            *ctx.dragStartValue = v;
        }

        const float travel = ctx.s (140.0f);
        const float fine = ImGui::GetIO().KeyShift ? 0.25f : 1.0f;
        const float dy = ImGui::GetMouseDragDelta (ImGuiMouseButton_Left, 0.0f).y;
        const float n = range.toNorm (*ctx.dragStartValue) - (dy / travel) * fine;
        v = range.fromNorm (n);
        value.store (v, std::memory_order_relaxed);
        owned = true;
    }
    else if (*ctx.activeDrag == id && ! ImGui::IsItemActive())
    {
        ctx.activeDrag->clear();
    }

    if (ImGui::IsItemHovered() && ImGui::GetIO().MouseWheel != 0.0f)
    {
        const float n = range.toNorm (v) + ImGui::GetIO().MouseWheel * 0.02f;
        value.store (range.fromNorm (n), std::memory_order_relaxed);
    }

    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
        value.store (defaultValue, std::memory_order_relaxed);

    drawSslKnob (*ctx.dl, centre, radius, range.toNorm (value.load (std::memory_order_relaxed)), fill);

    if (outline != theme::kKnobOutline)
        ctx.dl->AddCircle (centre, radius - 1.0f, col (outline), 32, ctx.s (1.4f));

    if (caption != nullptr)
        text (*ctx.dl, ctx.fonts->label, ctx.s (9.0f),
              ImVec2 (centre.x - ctx.s (24.0f), centre.y - radius - ctx.s (11.0f)),
              ctx.s (48.0f), theme::kTextDim, caption);

    if (valueText != nullptr)
        text (*ctx.dl, ctx.fonts->label, ctx.s (10.5f),
              ImVec2 (centre.x - ctx.s (28.0f), centre.y + radius + ctx.s (1.0f)),
              ctx.s (56.0f), theme::kTextValue, valueText);

    return owned;
}

bool textButton (Ctx& ctx, const char* id, ImVec2 tl, ImVec2 br, const char* label,
                 bool on, std::uint32_t offFill, std::uint32_t onFill,
                 std::uint32_t offText, std::uint32_t onText, float fontSize)
{
    ++ctx.widgets;
    const bool hovered = hitArea (id, tl, br);
    const bool clicked = ImGui::IsItemHovered() && ImGui::IsMouseReleased (ImGuiMouseButton_Left);

    ctx.dl->AddRectFilled (tl, br, col (on ? onFill : offFill), ctx.s (2.0f));
    if (hovered)
        ctx.dl->AddRectFilled (tl, br, IM_COL32 (255, 255, 255, ImGui::IsItemActive() ? 30 : 16),
                               ctx.s (2.0f));
    ctx.dl->AddRect (tl, br, col (theme::kKnobOutline, 0.55f), ctx.s (2.0f), 0, ctx.s (0.8f));

    const float h = br.y - tl.y;
    text (*ctx.dl, ctx.fonts->pill, ctx.s (fontSize),
          ImVec2 (tl.x, tl.y + (h - ctx.s (fontSize)) * 0.5f - ctx.s (1.0f)),
          br.x - tl.x, on ? onText : offText, label);

    return clicked;
}

// The EQ / COMP / AUX header pill: an LED hitbox on the left fifth toggles the section,
// the label hitbox opens its menu. Only the toggle half is wired in the spike.
bool splitModuleButton (Ctx& ctx, const char* id, ImVec2 tl, ImVec2 br,
                        const char* label, std::atomic<bool>& engaged,
                        std::uint32_t accent)
{
    ++ctx.widgets;
    const bool on = engaged.load (std::memory_order_relaxed);
    const float w = br.x - tl.x;
    const float h = br.y - tl.y;

    const bool hovered = hitArea (id, tl, br);
    bool toggled = false;
    if (hovered && ImGui::IsMouseReleased (ImGuiMouseButton_Left))
    {
        engaged.store (! on, std::memory_order_relaxed);
        toggled = true;
    }

    ctx.dl->AddRectFilled (tl, br, col (theme::kShellFill, 0.92f), ctx.s (4.0f));
    ctx.dl->AddRect (tl, br, col (theme::kShellBorder), ctx.s (4.0f), 0, ctx.s (0.9f));
    if (hovered)
        ctx.dl->AddRectFilled (tl, br, IM_COL32 (255, 255, 255, ImGui::IsItemActive() ? 46 : 23),
                               ctx.s (4.0f));

    const float dividerX = tl.x + w * 0.20f;
    ctx.dl->AddLine (ImVec2 (dividerX, tl.y + ctx.s (2.0f)), ImVec2 (dividerX, br.y - ctx.s (2.0f)),
                     col (theme::kShellDivider, 0.55f), ctx.s (1.0f));

    const float ledR = std::min (std::max (std::min (w - ctx.s (8.0f), h - ctx.s (6.0f)),
                                           ctx.s (5.0f)), ctx.s (9.0f)) * 0.5f;
    const ImVec2 ledC (tl.x + w * 0.10f, tl.y + h * 0.5f);
    ctx.dl->AddCircleFilled (ledC, ledR + ctx.s (1.0f), col (theme::kLedRingDark), 16);
    ctx.dl->AddCircleFilled (ledC, ledR, col (on ? accent : theme::kLedOffFill), 16);
    if (on)
        ctx.dl->AddCircleFilled (ImVec2 (ledC.x, ledC.y - ledR * 0.3f), ledR * 0.45f,
                                 col (brighter (accent, 0.8f), 0.55f), 12);

    text (*ctx.dl, ctx.fonts->pill, ctx.s (10.5f),
          ImVec2 (dividerX, tl.y + (h - ctx.s (10.5f)) * 0.5f - ctx.s (1.0f)),
          br.x - dividerX, on ? theme::kTextBright : theme::kLabelBypassed, label);

    return toggled;
}

std::uint32_t meterColourForDb (float db)
{
    if (db >= 5.0f)  return theme::kLedRed;
    if (db >= -5.0f) return theme::kLedYellow;
    return theme::kLedGreen;
}

// Segmented LED bar with hard zone boundaries at -5 and +5 dB, mapped through the fader's
// skewed range so 0 dB lines up with the fader's 0 tick.
void drawMeter (Ctx& ctx, ImVec2 tl, ImVec2 br, float displayedDb, float peakDb)
{
    const auto& range = StripParams::faderRange();
    const float h = br.y - tl.y;

    auto yForDb = [&] (float db)
    {
        return br.y - h * range.toNorm (std::clamp (db, -90.0f, 6.0f));
    };

    ctx.dl->AddRectFilled (tl, br, col (theme::kMeterBack), ctx.s (1.5f));

    const float fillTop = yForDb (displayedDb);
    if (fillTop < br.y - 0.5f)
    {
        const std::uint32_t tip = meterColourForDb (displayedDb);
        ctx.dl->AddRectFilled (ImVec2 (tl.x - ctx.s (1.5f), fillTop - ctx.s (1.5f)),
                               ImVec2 (br.x + ctx.s (1.5f), br.y + ctx.s (1.5f)),
                               col (tip, 0.20f), ctx.s (2.0f));

        const float yellowTop = yForDb (5.0f);
        const float greenTop  = yForDb (-5.0f);

        auto band = [&] (float top, float bottom, std::uint32_t c)
        {
            const float a = std::max (top, fillTop);
            if (a < bottom - 0.25f)
                ctx.dl->AddRectFilled (ImVec2 (tl.x, a), ImVec2 (br.x, bottom), col (c));
        };

        band (tl.y, yellowTop, theme::kLedRed);
        band (yellowTop, greenTop, theme::kLedYellow);
        band (greenTop, br.y, theme::kLedGreen);
    }

    if (peakDb > -99.0f)
    {
        const float py = yForDb (peakDb);
        ctx.dl->AddRectFilled (ImVec2 (tl.x, py - ctx.s (0.7f)),
                               ImVec2 (br.x, py + ctx.s (0.7f)),
                               col (peakDb >= 5.0f ? theme::kPeakTickHot : theme::kPeakTick));
    }

    const int segments = std::clamp (static_cast<int> (h / ctx.s (3.5f)), 8, 30);
    for (int i = 1; i < segments; ++i)
    {
        const float y = tl.y + h * (i / static_cast<float> (segments));
        ctx.dl->AddRectFilled (ImVec2 (tl.x + ctx.s (1.0f), y),
                               ImVec2 (br.x - ctx.s (1.0f), y + ctx.s (0.8f)),
                               col (theme::kMeterSegment));
    }

    ctx.dl->AddRect (tl, br, col (theme::kMeterBorder), ctx.s (1.5f), 0, ctx.s (0.5f));
}

// The slim GR column beside the fader: 24 one-dB segments filling downward, plus the
// draggable threshold triangle that stands in for the missing threshold knob.
void drawGrStrip (Ctx& ctx, ImVec2 tl, ImVec2 br, float grDb,
                  std::atomic<float>& thresholdDb, const char* id)
{
    ++ctx.widgets;
    constexpr float kFloor = -24.0f;
    const float h = br.y - tl.y;
    const float barR = br.x - ctx.s (8.0f);

    ctx.dl->AddRectFilled (tl, ImVec2 (barR, br.y), col (theme::kGrBack), ctx.s (2.0f));

    const float frac = std::clamp (-grDb / -kFloor, 0.0f, 1.0f);
    if (frac > 0.001f)
    {
        const float bottom = tl.y + h * frac;
        for (int i = 0; i < 24; ++i)
        {
            const float a = tl.y + h * (i / 24.0f);
            const float b = tl.y + h * ((i + 1) / 24.0f) - ctx.s (0.6f);
            if (a >= bottom)
                break;
            ctx.dl->AddRectFilled (ImVec2 (tl.x + ctx.s (1.0f), a),
                                   ImVec2 (barR - ctx.s (1.0f), std::min (b, bottom)),
                                   col (lerpArgb (brighter (theme::kCompGold, 0.25f),
                                                  brighter (theme::kHfRed, 0.10f), i / 23.0f)));
        }
    }

    ctx.dl->AddRect (tl, ImVec2 (barR, br.y), col (theme::kMeterBorder), ctx.s (2.0f), 0, ctx.s (0.5f));

    const auto& range = StripParams::faderRange();
    const float thr = thresholdDb.load (std::memory_order_relaxed);
    const float ty = br.y - h * range.toNorm (std::clamp (thr, -90.0f, 6.0f));

    hitArea (id, ImVec2 (barR, ty - ctx.s (7.0f)), ImVec2 (br.x, ty + ctx.s (7.0f)));
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging (ImGuiMouseButton_Left, 0.0f))
    {
        const float n = std::clamp ((br.y - ImGui::GetIO().MousePos.y) / h, 0.0f, 1.0f);
        thresholdDb.store (std::clamp (range.fromNorm (n), -60.0f, 0.0f), std::memory_order_relaxed);
    }

    const float tw = ctx.s (10.0f);
    ctx.dl->AddTriangleFilled (ImVec2 (br.x, ty - tw * 0.6f), ImVec2 (br.x, ty + tw * 0.6f),
                               ImVec2 (barR + ctx.s (1.0f), ty),
                               col (ImGui::IsItemHovered() ? brighter (theme::kCompGold, 0.4f)
                                                           : theme::kCompGold));
}

// The vertical fader: track, asymmetric ticks, the brushed cap, and the dB scale the strip
// paints in the left gutter.
void drawFader (Ctx& ctx, ImVec2 tl, ImVec2 br, float labelLeft,
                std::atomic<float>& faderDb, const char* id)
{
    ++ctx.widgets;
    const auto& range = StripParams::faderRange();
    const float pad = ctx.s (layout::kFaderTrackPad);
    const float trackTop = tl.y + pad;
    const float trackBottom = br.y - pad;
    const float trackH = trackBottom - trackTop;
    const float cx = (tl.x + br.x) * 0.5f;

    hitArea (id, tl, br);
    if (ImGui::IsItemActive() && ImGui::IsMouseDown (ImGuiMouseButton_Left))
    {
        const float n = std::clamp ((trackBottom - ImGui::GetIO().MousePos.y) / trackH, 0.0f, 1.0f);
        faderDb.store (range.fromNorm (n), std::memory_order_relaxed);
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
        faderDb.store (0.0f, std::memory_order_relaxed);

    const float db = faderDb.load (std::memory_order_relaxed);
    const float pos = trackBottom - trackH * range.toNorm (db);

    const float trackW = std::min (ctx.s (4.0f), (br.x - tl.x) * 0.18f);
    ctx.dl->AddRectFilled (ImVec2 (cx - trackW * 0.5f, trackTop),
                           ImVec2 (cx + trackW * 0.5f, trackBottom),
                           col (theme::kTrackFill), trackW * 0.5f);
    ctx.dl->AddRectFilledMultiColor (ImVec2 (cx - trackW * 0.5f, trackTop),
                                     ImVec2 (cx + trackW * 0.5f, trackTop + ctx.s (6.0f)),
                                     IM_COL32 (0, 0, 0, 128), IM_COL32 (0, 0, 0, 128),
                                     IM_COL32 (0, 0, 0, 0), IM_COL32 (0, 0, 0, 0));
    ctx.dl->AddRect (ImVec2 (cx - trackW * 0.5f, trackTop),
                     ImVec2 (cx + trackW * 0.5f, trackBottom),
                     col (theme::kStripBorder), trackW * 0.5f, 0, ctx.s (0.6f));

    for (const auto& tick : kFaderTicks)
    {
        const float y = trackBottom - trackH * range.toNorm (tick.db);
        const bool isZero = tick.db == 0.0f;
        const float leftOver = isZero ? ctx.s (4.0f) : ctx.s (3.0f);
        const float rightOver = isZero ? ctx.s (16.0f) : ctx.s (12.0f);

        ctx.dl->AddLine (ImVec2 (cx - trackW * 0.5f - leftOver, y),
                         ImVec2 (cx + trackW * 0.5f + rightOver, y),
                         IM_COL32 (255, 255, 255, isZero ? 0x90 : 0x40),
                         isZero ? ctx.s (1.2f) : ctx.s (0.7f));

        const bool isInf = tick.db <= -90.0f;
        const float size = ctx.s (isInf ? 16.0f : 10.5f);
        text (*ctx.dl, ctx.fonts->band, size,
              ImVec2 (labelLeft, y - size * (isInf ? 0.75f : 0.55f)),
              cx - ctx.s (layout::kFaderCapW) * 0.5f - labelLeft - ctx.s (3.0f),
              isZero ? theme::kTextBright : 0xffb8b8c0, tick.label, Align::right);
    }

    const float capW = std::min (br.x - tl.x - ctx.s (6.0f), ctx.s (layout::kFaderCapW));
    const float capH = ctx.s (layout::kFaderCapH);
    const ImVec2 capTL (cx - capW * 0.5f, pos - capH * 0.5f);
    const ImVec2 capBR (cx + capW * 0.5f, pos + capH * 0.5f);

    ctx.dl->AddRectFilled (ImVec2 (capTL.x - ctx.s (2.0f), capTL.y + ctx.s (2.0f)),
                           ImVec2 (capBR.x + ctx.s (2.0f), capBR.y + ctx.s (4.0f)),
                           IM_COL32 (0, 0, 0, 140), ctx.s (4.0f));

    const float bands[] = { 0.0f, 0.30f, 0.50f, 0.70f, 1.0f };
    const std::uint32_t stops[] = { theme::kCapTop, 0xffcfc8b8, theme::kCapMid, 0xffcfc8b8, theme::kCapBottom };
    for (int i = 0; i < 4; ++i)
    {
        ctx.dl->AddRectFilledMultiColor (ImVec2 (capTL.x, capTL.y + capH * bands[i]),
                                         ImVec2 (capBR.x, capTL.y + capH * bands[i + 1]),
                                         col (stops[i]), col (stops[i]),
                                         col (stops[i + 1]), col (stops[i + 1]));
    }

    ctx.dl->AddLine (ImVec2 (capTL.x, capTL.y + ctx.s (1.0f)), ImVec2 (capBR.x, capTL.y + ctx.s (1.0f)),
                     IM_COL32 (255, 255, 255, 0x70), ctx.s (1.0f));
    ctx.dl->AddLine (ImVec2 (capTL.x, capBR.y - ctx.s (2.0f)), ImVec2 (capBR.x, capBR.y - ctx.s (2.0f)),
                     IM_COL32 (0, 0, 0, 0x40), ctx.s (1.0f));
    ctx.dl->AddRect (capTL, capBR, col (theme::kCapRim), ctx.s (2.5f), 0, ctx.s (1.0f));

    for (int i = -1; i <= 1; ++i)
    {
        const float gy = pos + ctx.s (4.0f) * static_cast<float> (i);
        ctx.dl->AddRectFilled (ImVec2 (capTL.x + ctx.s (3.0f), gy),
                               ImVec2 (capBR.x - ctx.s (3.0f), gy + ctx.s (1.6f)),
                               col (theme::kCapGroove));
        ctx.dl->AddLine (ImVec2 (capTL.x + ctx.s (3.0f), gy + ctx.s (2.4f)),
                         ImVec2 (capBR.x - ctx.s (3.0f), gy + ctx.s (2.4f)),
                         IM_COL32 (255, 255, 255, 0x35), ctx.s (0.9f));
    }
}

} // namespace

// --------------------------------------------------------------------------------------

StripFrameResult ChannelStripView::draw (ImDrawList& dl, ImVec2 origin, float width,
                                         float height, float scale, StripParams& params,
                                         const StripFonts& fonts)
{
    StripFrameResult result;

    Ctx ctx;
    ctx.dl = &dl;
    ctx.scale = scale;
    ctx.fonts = &fonts;
    ctx.activeDrag = &activeDrag;
    ctx.dragStartValue = &dragStartValue;

    auto s = [scale] (float v) { return v * scale; };
    char buf[32];

    panel (dl, origin, ImVec2 (origin.x + width, origin.y + height),
           theme::kStripFill, theme::kStripBorder, 1.0f, s (4.0f), s (1.0f));

    dl.AddRectFilled (ImVec2 (origin.x + s (1.5f), origin.y + s (1.5f)),
                      ImVec2 (origin.x + width - s (1.5f), origin.y + s (5.5f)),
                      col (theme::kTrackColour, 0.85f), s (2.0f));

    const float left = origin.x + s (layout::kOuterInset);
    const float right = origin.x + width - s (layout::kOuterInset);
    const float innerW = right - left;
    float y = origin.y + s (layout::kOuterInset) + s (layout::kTopPad);

    // Name: double-click swaps the label for a text field, the one place the strip needs
    // real keyboard text entry.
    {
        const ImVec2 tl (left, y), br (right, y + s (layout::kNameH));
        if (editingName)
        {
            ImGui::SetCursorScreenPos (ImVec2 (tl.x, tl.y));
            ImGui::PushItemWidth (br.x - tl.x);
            ImGui::PushStyleColor (ImGuiCol_FrameBg, ImGui::ColorConvertU32ToFloat4 (col (theme::kButtonOff)));
            if (ImGui::InputText ("##name", nameBuffer, sizeof nameBuffer,
                                  ImGuiInputTextFlags_EnterReturnsTrue | ImGuiInputTextFlags_AutoSelectAll))
            {
                params.name = nameBuffer;
                editingName = false;
            }
            if (ImGui::IsItemDeactivated())
            {
                params.name = nameBuffer;
                editingName = false;
            }
            ImGui::SetKeyboardFocusHere (-1);
            ImGui::PopStyleColor();
            ImGui::PopItemWidth();
        }
        else
        {
            hitArea ("##nameLabel", tl, br);
            if (ImGui::IsItemHovered() && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                std::snprintf (nameBuffer, sizeof nameBuffer, "%s", params.name.c_str());
                editingName = true;
            }
            text (dl, fonts.name, s (13.0f), ImVec2 (tl.x, tl.y + s (3.0f)), innerW,
                  theme::kTextBright, params.name.c_str());
        }
        y = br.y + s (2.0f);
    }

    if (textButton (ctx, "##io", ImVec2 (left, y), ImVec2 (right, y + s (layout::kIoButtonH)),
                    "MONO  IN 1", false, theme::kButtonPanel, theme::kButtonPanel,
                    theme::kIoText, theme::kIoText, 10.0f))
        result.openIoModal = true;
    y += s (layout::kIoButtonH) + s (3.0f);

    {
        const float w3 = innerW / 3.0f;
        const float h = s (layout::kRowH);
        struct { const char* id; const char* label; std::atomic<bool>* flag;
                 std::uint32_t on; std::uint32_t offText; std::uint32_t onText; } row[] = {
            { "##in",    "IN",    &params.inputMonitor, theme::kPanCyan, 0xff708090, theme::kOnText },
            { "##arm",   "ARM",   &params.recordArmed,  theme::kArmOn,   theme::kArmOffText, theme::kTextBright },
            { "##print", "PRINT", &params.printEffects, theme::kPrintOn, 0xff8a7060, theme::kOnText },
        };
        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 tl (left + w3 * i + s (1.0f), y + s (1.0f));
            const ImVec2 br (left + w3 * (i + 1) - s (1.0f), y + h - s (1.0f));
            const bool on = row[i].flag->load (std::memory_order_relaxed);
            if (textButton (ctx, row[i].id, tl, br, row[i].label, on, theme::kButtonOff,
                            row[i].on, row[i].offText, row[i].onText, 9.5f))
                row[i].flag->store (! on, std::memory_order_relaxed);
        }
        y += h;
    }

    {
        const ImVec2 tl (left + s (3.0f), y), br (right - s (3.0f), y + s (layout::kRowH));
        textButton (ctx, "##insert", tl, br, "Baroness", false, theme::kButtonPanel,
                    theme::kButtonPanel, theme::kInsertText, theme::kInsertText, 10.0f);
        if (ImGui::IsItemHovered() && ImGui::IsMouseReleased (ImGuiMouseButton_Right))
        {
            result.openInsertMenu = true;
            result.insertMenuAt = ImGui::GetIO().MousePos;
        }
        dl.AddCircleFilled (ImVec2 (tl.x + s (8.0f), (tl.y + br.y) * 0.5f), s (4.0f),
                            col (0xff60d060), 16);
        y = br.y + s (2.0f);
    }

    // EQ
    {
        const float h = s (layout::kEqPanelH);
        panel (dl, ImVec2 (left, y), ImVec2 (right, y + h), theme::kEqFill,
               theme::kEqAccent, 0.40f, s (3.0f), s (0.8f));

        const float px = left + s (3.0f);
        const float pw = innerW - s (6.0f);
        float py = y;

        const ImVec2 chipTL (px + pw - s (28.0f), py + s (1.0f));
        const ImVec2 chipBR (px + pw - s (1.0f), py + s (layout::kEqHeaderH) - s (1.0f));
        const bool black = params.eqBlackMode.load (std::memory_order_relaxed);
        if (textButton (ctx, "##eqtype", chipTL, chipBR, black ? "G" : "E", black,
                        0xff5a3a20, theme::kButtonOff, theme::kTextBright, theme::kTextBright, 10.0f))
            params.eqBlackMode.store (! black, std::memory_order_relaxed);

        splitModuleButton (ctx, "##eqhdr", ImVec2 (px, py),
                           ImVec2 (chipTL.x - s (2.0f), py + s (layout::kEqHeaderH)),
                           "EQ", params.eqEnabled, theme::kLfGreen);
        py += s (layout::kEqHeaderH);

        const float halfW = pw * 0.5f;
        text (dl, fonts.band, s (12.0f), ImVec2 (px, py), halfW, theme::kTextDim, "HPF");
        text (dl, fonts.band, s (12.0f), ImVec2 (px + halfW, py), halfW, theme::kTextDim, "LPF");
        py += s (layout::kFilterLabelH);

        const float kr = s (layout::kKnobSize) * 0.5f;
        formatFreq (buf, sizeof buf, params.hpfFreq.load (std::memory_order_relaxed));
        knob (ctx, "##hpf", ImVec2 (px + halfW * 0.5f, py + kr), kr, params.hpfFreq,
              StripParams::hpfRange(), 20.0f, theme::kFilterWhite, nullptr, buf);
        formatFreq (buf, sizeof buf, params.lpfFreq.load (std::memory_order_relaxed));
        knob (ctx, "##lpf", ImVec2 (px + halfW * 1.5f, py + kr), kr, params.lpfFreq,
              StripParams::lpfRange(), 20000.0f, theme::kFilterWhite, nullptr, buf);
        py += s (layout::kKnobBlockH);

        dl.AddLine (ImVec2 (px, py + s (2.5f)), ImVec2 (px + pw, py + s (2.5f)),
                    col (theme::kEqAccent, 0.18f), s (1.0f));
        py += s (layout::kFilterBandGap);

        const float colW = (pw - s (layout::kRowLabelW)) / 3.0f;
        const char* heads[] = { "GAIN", "FREQ", "Q" };
        for (int i = 0; i < 3; ++i)
            text (dl, fonts.small, s (8.0f), ImVec2 (px + s (layout::kRowLabelW) + colW * i, py),
                  colW, theme::kTextDim, heads[i]);
        py += s (layout::kColumnHeaderH);

        struct Band { const char* label; std::atomic<float>* gain; std::atomic<float>* freq;
                      std::atomic<float>* q; const Range* freqRange; float freqDefault;
                      std::uint32_t colour; };
        const Band bands[] = {
            { "HF", &params.hfGainDb, &params.hfFreq, nullptr, &StripParams::hfFreqRange(), 8000.0f, theme::kHfRed },
            { "HM", &params.hmGainDb, &params.hmFreq, &params.hmQ, &StripParams::hmFreqRange(), 2000.0f, theme::kHmGreen },
            { "LM", &params.lmGainDb, &params.lmFreq, &params.lmQ, &StripParams::lmFreqRange(), 600.0f, theme::kLmBlue },
            { "LF", &params.lfGainDb, &params.lfFreq, nullptr, &StripParams::lfFreqRange(), 100.0f, theme::kLfGraphite },
        };

        for (int b = 0; b < 4; ++b)
        {
            const float rowY = py + (s (layout::kEqBandRowH) + s (layout::kEqBandGap)) * b;
            text (dl, fonts.band, s (12.0f), ImVec2 (px, rowY + s (7.0f)),
                  s (layout::kRowLabelW), bands[b].colour, bands[b].label);

            const float cy = rowY + kr;
            char id[24];

            formatGain (buf, sizeof buf, bands[b].gain->load (std::memory_order_relaxed));
            std::snprintf (id, sizeof id, "##g%d", b);
            knob (ctx, id, ImVec2 (px + s (layout::kRowLabelW) + colW * 0.5f, cy), kr,
                  *bands[b].gain, StripParams::gainRange(), 0.0f, bands[b].colour, nullptr, buf);

            formatFreq (buf, sizeof buf, bands[b].freq->load (std::memory_order_relaxed));
            std::snprintf (id, sizeof id, "##f%d", b);
            knob (ctx, id, ImVec2 (px + s (layout::kRowLabelW) + colW * 1.5f, cy), kr,
                  *bands[b].freq, *bands[b].freqRange, bands[b].freqDefault, bands[b].colour, nullptr, buf);

            if (bands[b].q != nullptr)
            {
                std::snprintf (buf, sizeof buf, "%.2f", bands[b].q->load (std::memory_order_relaxed));
                std::snprintf (id, sizeof id, "##q%d", b);
                knob (ctx, id, ImVec2 (px + s (layout::kRowLabelW) + colW * 2.5f, cy), kr,
                      *bands[b].q, StripParams::qRange(), 0.7f, bands[b].colour, nullptr, buf);
            }
        }

        y += h + s (3.0f);
    }

    // COMP
    {
        const float h = s (layout::kCompPanelH);
        panel (dl, ImVec2 (left, y), ImVec2 (right, y + h), theme::kCompFill,
               theme::kCompGold, 0.40f, s (3.0f), s (0.8f));

        const float px = left + s (3.0f);
        const float pw = innerW - s (6.0f);
        splitModuleButton (ctx, "##comphdr", ImVec2 (px, y + s (2.0f)),
                           ImVec2 (px + pw, y + s (18.0f)), "COMP",
                           params.compEnabled, theme::kCompGold);

        const float bodyY = y + s (20.0f);
        const float colW = pw / 4.0f;
        const float kr = s (layout::kCompKnobSize) * 0.5f;

        struct Cell { const char* id; const char* label; std::atomic<float>* value;
                      const Range* range; float def; const char* fmt; };
        const Cell cells[] = {
            { "##rat", "RAT", &params.compFetRatioIndex, nullptr, 0.0f, nullptr },
            { "##atk", "ATK", &params.compFetAttack,  &StripParams::atkRange(), 0.2f,   "%.2f" },
            { "##rel", "REL", &params.compFetRelease, &StripParams::relRange(), 400.0f, "%.0f" },
            { "##mak", "MAK", &params.compFetOutput,  &StripParams::makRange(), 0.0f,   "%+.1f" },
        };
        static const Range ratioRange { 0.0f, 4.0f, 1.0f };
        static const char* kRatios[] = { "4:1", "8:1", "12:1", "20:1", "All" };

        for (int i = 0; i < 4; ++i)
        {
            const float cx = px + colW * (i + 0.5f);
            text (dl, fonts.label, s (9.0f), ImVec2 (cx - colW * 0.5f, bodyY), colW,
                  0xffb07050, cells[i].label);

            if (cells[i].range == nullptr)
            {
                const int idx = std::clamp (static_cast<int> (std::lround (
                    cells[i].value->load (std::memory_order_relaxed))), 0, 4);
                knob (ctx, cells[i].id, ImVec2 (cx, bodyY + s (layout::kCompKnobLabelH) + kr), kr,
                      *cells[i].value, ratioRange, 0.0f, theme::kCompGold, nullptr, kRatios[idx]);
            }
            else
            {
                std::snprintf (buf, sizeof buf, cells[i].fmt,
                               cells[i].value->load (std::memory_order_relaxed));
                knob (ctx, cells[i].id, ImVec2 (cx, bodyY + s (layout::kCompKnobLabelH) + kr), kr,
                      *cells[i].value, *cells[i].range, cells[i].def, theme::kCompGold, nullptr, buf);
            }
        }

        y += h + s (6.0f);
    }

    // AUX sends
    {
        const float h = s (layout::kAuxPanelH);
        panel (dl, ImVec2 (left, y), ImVec2 (right, y + h), theme::kSendFill,
               theme::kSendPurple, 0.40f, s (3.0f), s (0.8f));

        const float px = left + s (3.0f);
        const float pw = innerW - s (6.0f);
        const float colW = pw / 4.0f;
        const float kr = s (layout::kAuxKnobSize) * 0.5f;
        const char* names[] = { "REV", "DLY", "CUE", "FX" };

        for (int i = 0; i < 4; ++i)
        {
            const float cx = px + colW * (i + 0.5f);
            text (dl, fonts.small, s (8.5f), ImVec2 (cx - colW * 0.5f, y + s (3.0f)), colW,
                  theme::kSendPurple, names[i]);

            const float knobY = y + s (3.0f + 11.0f + 1.0f) + (i % 2 == 0 ? 0.0f : s (layout::kAuxStaggerY));
            const float db = params.auxSendDb[i].load (std::memory_order_relaxed);
            if (db <= StripParams::kAuxSendOffDb + 0.5f) std::snprintf (buf, sizeof buf, "-");
            else                                         std::snprintf (buf, sizeof buf, "%.0f", db);

            char id[16];
            std::snprintf (id, sizeof id, "##aux%d", i);
            knob (ctx, id, ImVec2 (cx, knobY + kr), kr, params.auxSendDb[i],
                  StripParams::auxRange(), StripParams::kAuxSendOffDb, theme::kAuxFill[i],
                  nullptr, buf,
                  params.auxSendPreFader[i].load (std::memory_order_relaxed)
                      ? theme::kAuxPreOutline : theme::kKnobOutline);
        }

        y += h + s (3.0f);
    }

    // Fader column: pan on top, then bus buttons / fader / meter / GR side by side.
    {
        const float bottomBlock = s (layout::kPeakLabelH + 2.0f + layout::kAutoH + 4.0f + layout::kMsRowH);
        const float colBottom = origin.y + height - s (layout::kOuterInset) - bottomBlock;
        const float cx = (left + right) * 0.5f;

        const float panTop = y;
        text (dl, fonts.pill, s (10.5f), ImVec2 (cx - s (28.0f), panTop), s (56.0f),
              theme::kTextDim, "PAN");
        const float pan = params.pan.load (std::memory_order_relaxed);
        if (std::fabs (pan) < 0.005f)      std::snprintf (buf, sizeof buf, "C");
        else if (pan < 0.0f)               std::snprintf (buf, sizeof buf, "L%.0f", -pan * 100.0f);
        else                               std::snprintf (buf, sizeof buf, "R%.0f", pan * 100.0f);
        knob (ctx, "##pan", ImVec2 (cx, panTop + s (layout::kPanLabelH) + s (13.0f)),
              s (layout::kPanKnobSize) * 0.5f, params.pan, StripParams::panRange(), 0.0f,
              theme::kPanRed, nullptr, buf);

        const float faderTop = panTop + s (layout::kPanLabelH + layout::kPanBlockH + layout::kPanFaderGap);
        const float faderW = std::clamp (innerW - s (74.0f), s (22.0f), s (40.0f));

        drawFader (ctx, ImVec2 (cx - faderW * 0.5f, faderTop),
                   ImVec2 (cx + faderW * 0.5f, colBottom),
                   left + s (layout::kBusColumnW) + s (6.0f), params.faderDb, "##fader");

        // Meters share the fader's top and bottom trim so 0 dB is on the same scan line.
        const float meterTop = faderTop + s (layout::kFaderTrackPad);
        const float meterBottom = colBottom - s (layout::kFaderTrackPad);
        const float meterX = cx + faderW * 0.5f + s (1.0f);

        inputMeter.tick (params.meterInputDb.load (std::memory_order_relaxed),
                         ImGui::GetIO().Framerate > 1.0f ? ImGui::GetIO().Framerate / 30.0f : 2.0f);
        drawMeter (ctx, ImVec2 (meterX, meterTop),
                   ImVec2 (meterX + s (layout::kMeterWidth), meterBottom),
                   inputMeter.displayed, inputMeter.peakHold);

        const float grX = meterX + s (layout::kMeterWidth) + s (1.0f);
        const float gr = params.meterGrDb.load (std::memory_order_relaxed);
        if (gr < displayedGrDb) displayedGrDb = gr;
        else                    displayedGrDb += (gr - displayedGrDb) * 0.18f;
        drawGrStrip (ctx, ImVec2 (grX, meterTop), ImVec2 (grX + s (layout::kGrLedW), meterBottom),
                     displayedGrDb, params.compFetThresholdDb, "##thr");

        // Bus buttons track the fader's 0 dB and -inf marks, as in the shipping layout.
        {
            const auto& range = StripParams::faderRange();
            const float trackTop = faderTop + s (layout::kFaderTrackPad);
            const float trackH = (colBottom - s (layout::kFaderTrackPad)) - trackTop;
            const float zeroY = trackTop + trackH * (1.0f - range.toNorm (0.0f));
            const float offY  = trackTop + trackH;
            const float bh = s (layout::kBusButtonH);
            const float span = std::max (4.0f * bh, offY - zeroY);
            const int step = static_cast<int> (std::clamp (std::lround ((span - bh) / 3.0f * 0.85),
                                                           static_cast<long> (bh),
                                                           static_cast<long> (s (48.0f))));
            const float stackTop = zeroY + std::max (0.0f, (span - (bh + step * 3)) * 0.5f);
            const std::uint32_t busColours[] = { 0xff5a8ad0, 0xffe0c050, 0xff60c060, 0xffc06888 };

            for (int i = 0; i < 4; ++i)
            {
                const ImVec2 tl (left + s (3.0f), stackTop + step * i);
                const ImVec2 br (tl.x + s (layout::kBusColumnW), tl.y + bh);
                char id[16];
                std::snprintf (id, sizeof id, "##bus%d", i);
                std::snprintf (buf, sizeof buf, "%d", i + 1);
                const bool on = params.busAssign[i].load (std::memory_order_relaxed);
                if (textButton (ctx, id, tl, br, buf, on, theme::kButtonOff, busColours[i],
                                brighter (busColours[i], 0.15f), theme::kOnText, 10.0f))
                    params.busAssign[i].store (! on, std::memory_order_relaxed);
            }
        }

        y = colBottom;
    }

    // Readouts, automation mode, mute / solo / phase.
    {
        const float db = params.faderDb.load (std::memory_order_relaxed);
        if (db <= -89.95f) std::snprintf (buf, sizeof buf, "\xe2\x88\x9e");
        else               std::snprintf (buf, sizeof buf, "%.1f", db);
        text (dl, fonts.monoBig, s (14.0f), ImVec2 (left, y + s (2.0f)), innerW * 0.55f,
              theme::kTextValue, buf, Align::centre);

        const float peak = inputMeter.peakHold;
        std::snprintf (buf, sizeof buf, "%.1f", peak);
        const std::uint32_t peakCol = peak >= -3.0f ? 0xffff5050
                                    : (peak >= -12.0f ? 0xffe0c050 : theme::kTextValue);
        text (dl, fonts.mono, s (11.0f), ImVec2 (left + innerW * 0.55f, y + s (4.0f)),
              innerW * 0.45f, peakCol, buf, Align::right);
        y += s (layout::kPeakLabelH + 2.0f);

        textButton (ctx, "##auto", ImVec2 (left + s (1.0f), y),
                    ImVec2 (right - s (1.0f), y + s (layout::kAutoH)), "AUTO: READ", true,
                    theme::kButtonOff, 0xff20603a, 0xff707074, 0xffd0e8d0, 9.5f);
        y += s (layout::kAutoH + 4.0f);

        const float w3 = innerW / 3.0f;
        struct { const char* id; const char* label; std::atomic<bool>* flag; std::uint32_t on; } msp[] = {
            { "##mute",  "M", &params.mute,        theme::kMuteOn },
            { "##solo",  "S", &params.solo,        theme::kSoloOn },
            { "##phase", "\xc3\x98", &params.phaseInvert, theme::kPhaseOn },
        };
        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 tl (left + w3 * i + s (1.0f), y + s (1.0f));
            const ImVec2 br (left + w3 * (i + 1) - s (1.0f), y + s (layout::kMsRowH) - s (1.0f));
            const bool on = msp[i].flag->load (std::memory_order_relaxed);
            if (textButton (ctx, msp[i].id, tl, br, msp[i].label, on, theme::kButtonOff,
                            msp[i].on, theme::kTextDim, theme::kOnText, 11.0f))
                msp[i].flag->store (! on, std::memory_order_relaxed);
        }
    }

    lastWidgetCount = ctx.widgets;
    return result;
}

} // namespace duskspike
