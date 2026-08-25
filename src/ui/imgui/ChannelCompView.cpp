#include "ChannelCompView.h"
#include "DuskTheme.h"
#include "../../engine/CompModeMap.h"
#include "../../session/Session.h"

#include <algorithm>
#include <cstdio>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// The JUCE editor's resized() geometry, in design pixels. Every constant below is
// the one it laid out with, so the port lands on the same picture.
constexpr float kOuterInset = 12.0f;
constexpr float kHeaderH = 24.0f;
constexpr float kModeRowH = 24.0f;
constexpr float kKneeRowH = 20.0f;
constexpr float kRowLabelH = 14.0f;
constexpr float kHandleW = 14.0f;
constexpr float kMeterW = 28.0f;
constexpr float kMeterGap = 4.0f;
constexpr float kMeterStripW = kHandleW + kMeterGap + kMeterW * 2.0f + kMeterGap;
constexpr float kMeterTopPad = 16.0f;
constexpr float kMeterBottomPad = 14.0f;
constexpr float kColGap = 12.0f;
constexpr float kPanelW = 380.0f;

// refreshLabelsForMode's uniform height: the grid is the same in every mode so a
// mode swap never resizes the panel.
constexpr float kPanelH = (kHeaderH + 8.0f + kModeRowH + 12.0f)
                        + (56.0f + 18.0f + 4.0f) + 6.0f + (56.0f + 18.0f + 4.0f)
                        + 16.0f + 24.0f;

constexpr float kInMeterMinDb = -60.0f;
constexpr float kInMeterMaxDb = 0.0f;
constexpr float kGrMeterMaxDb = 20.0f;

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

float clampf (float lo, float hi, float value)
{
    return std::clamp (value, lo, std::max (lo, hi));
}

dw::Range rangeWithMid (comp::Domain domain, float midPoint)
{
    return dw::Range::withMidPoint (domain.lo, domain.hi, midPoint);
}

// The FET ratio knob sweeps continuously but the unit has a five-position switch, so
// the sweep picks a rung and the rung reads back as its nominal ratio.
int fetRatioIndexFor (float knobValue)
{
    if (knobValue >= 18.0f) return 4;
    if (knobValue >= 14.0f) return 3;
    if (knobValue >= 10.0f) return 2;
    if (knobValue >= 6.0f)  return 1;
    return 0;
}

float fetRatioDisplayFor (int index)
{
    static const float display[] = { 4.0f, 8.0f, 12.0f, 20.0f, 20.0f };
    return display[std::clamp (index, 0, comp::kFetRatioMaxIndex)];
}

class ChannelCompView final : public DuskPanelView
{
public:
    explicit ChannelCompView (Track& trackRef) : track (trackRef) {}

    ImVec2 preferredSize() const override { return ImVec2 (kPanelW, kPanelH); }

    // A processing editor: the DAW behind stays readable so the strip meters can be
    // watched while auditioning.
    float dimAlpha() const override { return 0.28f; }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        auto& strip = track.strip;
        const float scale = ctx.scale;
        auto s = [scale] (float v) { return v * scale; };

        ctx.dl->AddRectFilled (origin, ImVec2 (origin.x + size.x, origin.y + size.y),
                               rgba (0x181820ff));
        ctx.dl->AddRect (ImVec2 (origin.x + 0.5f, origin.y + 0.5f),
                         ImVec2 (origin.x + size.x - 0.5f, origin.y + size.y - 0.5f),
                         rgba (0x2a2a30ff), 0.0f, 0, scale);

        advanceMeters();

        const float left = origin.x + s (kOuterInset);
        const float right = origin.x + size.x - s (kOuterInset);
        const float bottom = origin.y + size.y - s (kOuterInset);
        const float innerW = right - left;
        float y = origin.y + s (kOuterInset);

        const int mode = std::clamp (strip.compMode.load (std::memory_order_relaxed), 0, 2);

        drawHeader (ctx, strip, ImVec2 (left, y), innerW);
        y += s (kHeaderH) + s (8.0f);
        drawModeRow (ctx, strip, ImVec2 (left, y), innerW, mode);
        y += s (kModeRowH) + s (6.0f);

        if (mode == 2)
        {
            drawKneeRow (ctx, strip, ImVec2 (left, y), innerW);
            y += s (kKneeRowH) + s (6.0f);
        }
        else
        {
            y += s (6.0f);
        }

        drawMeterStrip (ctx, strip, ImVec2 (left, y), bottom);
        drawRotaries (ctx, strip, ImVec2 (left + s (kMeterStripW) + s (kColGap), y),
                      right - (left + s (kMeterStripW) + s (kColGap)), bottom - y, mode);
    }

private:
    void advanceMeters()
    {
        // The JUCE editor ran these at 30 Hz with 0.18 and 0.10; the window draws at
        // 60, so a frame is half a tick and the per-frame coefficient is
        // 1 - sqrt(1 - a) rather than a/2. Halving it would decay the meters faster
        // than the strip's, which is the one thing the two must agree on.
        const float gr = track.meterGrDb.load (std::memory_order_relaxed);
        if (gr < displayedGrDb) displayedGrDb = gr;
        else                    displayedGrDb += (gr - displayedGrDb) * 0.0945f;

        const float in = track.meterInputDb.load (std::memory_order_relaxed);
        if (in > displayedInputDb) displayedInputDb = in;
        else                       displayedInputDb += (in - displayedInputDb) * 0.0513f;
    }

    void drawHeader (dw::Context& ctx, ChannelStripParams& strip, ImVec2 at, float width)
    {
        const auto& pal = consolePalette();
        const float scale = ctx.scale;
        const ImVec2 tl (at.x + width - scale * 60.0f + scale, at.y + scale);
        const ImVec2 br (at.x + width - scale, at.y + scale * kHeaderH - scale);

        dw::ButtonStyle style;
        style.offFill = rgba (0x202024ff);
        style.onFill = pal.compGold;
        style.offText = pal.compGold;
        style.onText = rgba (0x121214ff);
        style.fontSize = 10.0f * scale;

        const bool on = strip.compEnabled.load (std::memory_order_relaxed);
        if (dw::textButton (ctx, "##comp-on", tl, br, "ON", on, style).clicked)
            strip.compEnabled.store (! on, std::memory_order_relaxed);
    }

    void drawModeRow (dw::Context& ctx, ChannelStripParams& strip, ImVec2 at, float width,
                      int mode)
    {
        static const char* const labels[] = { "Opto", "FET", "VCA" };
        static const char* const ids[] = { "##mode-opto", "##mode-fet", "##mode-vca" };

        const auto& pal = consolePalette();
        const float scale = ctx.scale;
        const float colW = width / 3.0f;

        dw::ButtonStyle style;
        style.offFill = rgba (0x181820ff);
        style.onFill = pal.compGold;
        style.offText = pal.compGold;
        style.onText = rgba (0x121214ff);
        style.fontSize = 10.0f * scale;

        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 tl (at.x + colW * static_cast<float> (i) + scale * 2.0f, at.y);
            const ImVec2 br (at.x + colW * static_cast<float> (i + 1) - scale * 2.0f,
                             at.y + scale * kModeRowH);
            if (dw::textButton (ctx, ids[i], tl, br, labels[i], mode == i, style).clicked
                && mode != i)
                strip.compMode.store (i, std::memory_order_relaxed);
        }
    }

    void drawKneeRow (dw::Context& ctx, ChannelStripParams& strip, ImVec2 at, float width)
    {
        const auto& pal = consolePalette();
        const float scale = ctx.scale;
        const float colW = width * 0.5f;

        dw::ButtonStyle style;
        style.offFill = rgba (0x181820ff);
        style.onFill = pal.compGold;
        style.offText = pal.compGold;
        style.onText = rgba (0x121214ff);
        style.fontSize = 10.0f * scale;

        const bool soft = strip.compVcaOverEasy.load (std::memory_order_relaxed);
        if (dw::textButton (ctx, "##vca-knee",
                            ImVec2 (at.x + scale * 4.0f, at.y),
                            ImVec2 (at.x + colW - scale * 4.0f, at.y + scale * kKneeRowH),
                            "Soft Knee", soft, style).clicked)
            strip.compVcaOverEasy.store (! soft, std::memory_order_relaxed);

        const bool classic = strip.compVcaDetectorClassic.load (std::memory_order_relaxed);
        if (dw::textButton (ctx, "##vca-detector",
                            ImVec2 (at.x + colW + scale * 4.0f, at.y),
                            ImVec2 (at.x + width - scale * 4.0f, at.y + scale * kKneeRowH),
                            "Classic", classic, style).clicked)
            strip.compVcaDetectorClassic.store (! classic, std::memory_order_relaxed);
    }

    void drawVerticalMeter (dw::Context& ctx, ImVec2 tl, ImVec2 br, float db,
                            float minDb, float maxDb, ImU32 fillTop, ImU32 fillBottom,
                            const char* caption, const char* valueText)
    {
        const float scale = ctx.scale;

        ctx.dl->AddRectFilled (tl, br, rgba (0x0c0c0eff), scale * 2.0f);
        ctx.dl->AddRect (tl, br, rgba (0x2a2a2eff), scale * 2.0f, 0, scale * 0.8f);

        const float frac = (clampf (minDb, maxDb, db) - minDb) / (maxDb - minDb);
        if (frac > 0.001f)
        {
            const float fillH = (br.y - tl.y - scale * 4.0f) * frac;
            const ImVec2 fillTl (tl.x + scale * 2.0f, br.y - scale * 2.0f - fillH);
            const ImVec2 fillBr (br.x - scale * 2.0f, br.y - scale * 2.0f);
            ctx.dl->AddRectFilledMultiColor (fillTl, fillBr, fillTop, fillTop,
                                             fillBottom, fillBottom);
        }

        dw::text (ctx, ctx.fonts->pill, scale * 10.0f,
                  ImVec2 (tl.x - scale * 8.0f, tl.y - scale * 14.0f),
                  br.x - tl.x + scale * 16.0f, rgba (0xa0a0a0ff), caption);
        dw::text (ctx, ctx.fonts->band, scale * 12.0f,
                  ImVec2 (tl.x - scale * 8.0f, br.y + scale * 1.0f),
                  br.x - tl.x + scale * 16.0f, rgba (0xa0a0a0ff), valueText);
    }

    void drawMeterStrip (dw::Context& ctx, ChannelStripParams& strip, ImVec2 at, float bottom)
    {
        const auto& pal = consolePalette();
        const float scale = ctx.scale;
        const float top = at.y + scale * kMeterTopPad;
        const float base = bottom - scale * kMeterBottomPad;
        if (base <= top)
            return;

        const float handleX = at.x;
        const float inX = handleX + scale * (kHandleW + kMeterGap);
        const float grX = inX + scale * (kMeterW + kMeterGap);
        const ImVec2 inTl (inX, top), inBr (inX + scale * kMeterW, base);
        const ImVec2 grTl (grX, top), grBr (grX + scale * kMeterW, base);

        char buffer[32];
        std::snprintf (buffer, sizeof buffer, displayedInputDb <= -99.0f ? "-inf" : "%.1f",
                       static_cast<double> (displayedInputDb));
        drawVerticalMeter (ctx, inTl, inBr, displayedInputDb, kInMeterMinDb, kInMeterMaxDb,
                           rgba (0xd05a5aff), rgba (0x60c060ff), "IN", buffer);

        std::snprintf (buffer, sizeof buffer, "%.1f", static_cast<double> (displayedGrDb));
        drawVerticalMeter (ctx, grTl, grBr, -displayedGrDb, 0.0f, kGrMeterMaxDb,
                           dw::brighter (pal.hfRed, 0.1f), dw::brighter (pal.compGold, 0.2f),
                           "GR", buffer);

        // The handle column and the IN bar are one drag target, the way the inline
        // strip's meter is: a press anywhere on either sets the threshold.
        const ImVec2 dragTl (handleX - scale * 2.0f, top - scale * 2.0f);
        const ImVec2 dragBr (inBr.x + scale * 2.0f, base + scale * 2.0f);
        const bool hovered = dw::hitArea (ctx, "##comp-threshold", dragTl, dragBr);

        if (ImGui::IsItemActive())
            applyThresholdAt (strip, ImGui::GetIO().MousePos.y, inTl.y, inBr.y);
        else if (hovered && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            comp::resetTrackCompThreshold (strip);

        drawThresholdHandle (ctx, strip, inTl, inBr, handleX);
    }

    void applyThresholdAt (ChannelStripParams& strip, float pointerY, float barTop,
                           float barBottom)
    {
        const float travel = std::max (1.0f, (barBottom - barTop) - 4.0f);
        const float frac = clampf (0.0f, 1.0f, (barBottom - 2.0f - pointerY) / travel);
        comp::applyTrackCompThresholdDb (
            strip, comp::fracToValue (comp::thresholdDomainFor (strip), frac));
        // Touching the threshold engages the comp, the same rule the inline strip's
        // meter drag follows: nobody sets a threshold meaning to leave it bypassed.
        strip.compEnabled.store (true, std::memory_order_relaxed);
    }

    void drawThresholdHandle (dw::Context& ctx, const ChannelStripParams& strip,
                              ImVec2 inTl, ImVec2 inBr, float handleX)
    {
        const auto& pal = consolePalette();
        const float scale = ctx.scale;
        const float frac = comp::valueToFrac (comp::thresholdDomainFor (strip),
                                              comp::trackCompThresholdDb (strip));
        const float y = inBr.y - scale * 2.0f - frac * ((inBr.y - inTl.y) - scale * 4.0f);

        const float halfH = scale * 9.0f;
        const float tipX = handleX + scale * (kHandleW + 2.0f);
        const bool engaged = strip.compEnabled.load (std::memory_order_relaxed);
        const ImU32 fill = engaged ? dw::brighter (pal.compGold, 0.30f) : rgba (0x909098ff);

        ctx.dl->AddTriangleFilled (ImVec2 (handleX, y - halfH + scale),
                                   ImVec2 (handleX, y + halfH + scale),
                                   ImVec2 (tipX + scale, y + scale),
                                   IM_COL32 (0, 0, 0, 115));
        ctx.dl->AddTriangleFilled (ImVec2 (handleX, y - halfH), ImVec2 (handleX, y + halfH),
                                   ImVec2 (tipX, y), fill);
        ctx.dl->AddTriangle (ImVec2 (handleX, y - halfH), ImVec2 (handleX, y + halfH),
                             ImVec2 (tipX, y), rgba (0x0a0a0aff), scale);
        ctx.dl->AddLine (ImVec2 (inTl.x - scale, y), ImVec2 (inBr.x + scale, y),
                         dw::withAlpha (rgba (0xf8c878ff), engaged ? 0.75f : 0.40f),
                         scale * 1.2f);
    }

    void drawRotaries (dw::Context& ctx, ChannelStripParams& strip, ImVec2 at, float width,
                       float height, int mode)
    {
        const float scale = ctx.scale;
        const float rowH = (height - scale * kRowLabelH * 2.0f - scale * 12.0f) * 0.5f;
        if (rowH <= 0.0f || width <= 0.0f)
            return;

        if (mode == 0)
        {
            // Opto's ratio and timing are the optical model's, so only the makeup
            // knob has anything behind it and it takes the column's centre.
            const float cellH = scale * kRowLabelH + rowH;
            const float cellW = std::min (width, scale * 120.0f);
            drawMakeup (ctx, strip, ImVec2 (at.x + (width - cellW) * 0.5f,
                                            at.y + (height - cellH) * 0.5f),
                        cellW, rowH);
            return;
        }

        const float colW = width * 0.5f;
        const float topY = at.y;
        const float bottomY = at.y + scale * kRowLabelH + rowH + scale * 12.0f;

        drawRatio (ctx, strip, ImVec2 (at.x, topY), colW, rowH, mode);
        drawTiming (ctx, strip, ImVec2 (at.x + colW, topY), colW, rowH, true, mode);
        drawTiming (ctx, strip, ImVec2 (at.x, bottomY), colW, rowH, false, mode);
        drawMakeup (ctx, strip, ImVec2 (at.x + colW, bottomY), colW, rowH);
    }

    // Every cell is a caption over a knob over its readout, the way the JUCE grid laid
    // a Label over a rotary whose text box sat underneath it. The caption and the
    // readout are drawn here rather than left to the kit's KnobStyle, which has one
    // size and one colour for each and neither is the one this panel prints.
    dw::KnobResult knobCell (dw::Context& ctx, const char* id, ImVec2 at, float width,
                             float height, const char* caption, const char* valueText,
                             float value, const dw::Range& range, float defaultValue)
    {
        const float scale = ctx.scale;
        dw::text (ctx, ctx.fonts->band, scale * 11.0f, at, width,
                  consolePalette().compLabel, caption);

        dw::KnobStyle style;
        style.fill = consolePalette().compGold;
        style.outline = rgba (0x404048ff);

        const float readoutH = scale * 18.0f;
        const float knobTop = at.y + scale * kRowLabelH;
        const float knobH = std::max (scale * 12.0f, height - readoutH);
        const float radius = std::max (scale * 6.0f,
                                       std::min (width * 0.5f - scale * 4.0f,
                                                 knobH * 0.5f - scale * 2.0f));
        const ImVec2 centre (at.x + width * 0.5f, knobTop + knobH * 0.5f);
        const auto result = dw::knob (ctx, id, centre, radius, value, range, defaultValue,
                                      style);

        dw::text (ctx, ctx.fonts->valueLarge, scale * 14.0f,
                  ImVec2 (at.x, centre.y + radius + scale * 3.0f), width,
                  rgba (0xe0e0e0ff), valueText);

        if (result.dragging)
        {
            std::snprintf (ctx.drag->bubbleText, sizeof ctx.drag->bubbleText, "%s", valueText);
            ctx.drag->bubbleAt = ImVec2 (centre.x + radius, centre.y);
        }
        return result;
    }

    void drawRatio (dw::Context& ctx, ChannelStripParams& strip, ImVec2 at, float width,
                    float height, int mode)
    {
        const auto domain = comp::ratioKnobDomainFor (strip);
        const float value = mode == 1
            ? fetRatioDisplayFor (strip.compFetRatio.load (std::memory_order_relaxed))
            : strip.compVcaRatio.load (std::memory_order_relaxed);

        char buffer[32];
        std::snprintf (buffer, sizeof buffer, "%.1f:1", static_cast<double> (value));

        const auto result = knobCell (ctx, "##comp-ratio", at, width, height, "RATIO", buffer,
                                      value, rangeWithMid (domain, mode == 1 ? 4.0f : 8.0f),
                                      mode == 1 ? 4.0f : comp::kVcaRatioDefault);
        if (! result.changed)
            return;

        if (mode == 1)
            strip.compFetRatio.store (fetRatioIndexFor (result.value), std::memory_order_relaxed);
        else
            strip.compVcaRatio.store (clampf (comp::kVcaRatioMin, comp::kVcaRatioMax, result.value),
                                      std::memory_order_relaxed);
    }

    void drawTiming (dw::Context& ctx, ChannelStripParams& strip, ImVec2 at, float width,
                     float height, bool attack, int mode)
    {
        const auto domain = attack ? comp::attackDomainFor (strip) : comp::releaseDomainFor (strip);
        const float value = attack ? comp::trackCompAttackMs (strip)
                                   : comp::trackCompReleaseMs (strip);
        const float midPoint = attack ? (mode == 1 ? 0.8f : 3.0f) : 200.0f;
        const float defaultValue = attack
            ? (mode == 1 ? comp::kFetAttackDefaultMs : comp::kVcaAttackDefaultMs)
            : (mode == 1 ? comp::kFetReleaseDefaultMs : comp::kVcaReleaseDefaultMs);

        char buffer[32];
        std::snprintf (buffer, sizeof buffer, attack ? "%.1f ms" : "%.0f ms",
                       static_cast<double> (value));

        const auto result = knobCell (ctx, attack ? "##comp-attack" : "##comp-release", at,
                                      width, height, attack ? "ATTACK" : "RELEASE", buffer,
                                      value, rangeWithMid (domain, midPoint), defaultValue);
        if (! result.changed)
            return;

        if (attack) comp::applyTrackCompAttackMs (strip, result.value);
        else        comp::applyTrackCompReleaseMs (strip, result.value);
    }

    void drawMakeup (dw::Context& ctx, ChannelStripParams& strip, ImVec2 at, float width,
                     float height)
    {
        const auto domain = comp::makeupDomainFor (strip);
        const float value = comp::trackCompMakeupDb (strip);

        char buffer[32];
        std::snprintf (buffer, sizeof buffer, "%.1f dB", static_cast<double> (value));

        const auto result = knobCell (ctx, "##comp-makeup", at, width, height, "MAKEUP", buffer,
                                      value, dw::Range (domain.lo, domain.hi), 0.0f);
        if (result.changed)
            comp::applyTrackCompMakeupDb (strip, result.value);
    }

    Track& track;
    float displayedInputDb = -100.0f;
    float displayedGrDb = 0.0f;
};
} // namespace

std::unique_ptr<DuskPanelView> makeChannelCompView (Track& track)
{
    return std::unique_ptr<DuskPanelView> (new ChannelCompView (track));
}
} // namespace duskstudio::imgui
