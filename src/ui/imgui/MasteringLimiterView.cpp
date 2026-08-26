#include "MasteringLimiterView.h"
#include "DuskTheme.h"
#include "PanelControls.h"
#include "../../dsp/BrickwallLimiter.h"
#include "../../session/Session.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// MasteringLimiterEditor's geometry and palette, in design pixels.
constexpr float kOuterInset = 8.0f;
constexpr float kHeaderH = 22.0f;
constexpr float kHeaderGap = 8.0f;
constexpr float kRightColW = 110.0f;
constexpr float kRightColGap = 8.0f;
constexpr float kCaptionH = 14.0f;
constexpr float kComboH = 24.0f;
constexpr float kKnobBlockH = 70.0f;
constexpr float kToggleH = 22.0f;
constexpr float kLufsBoxH = 80.0f;
constexpr float kMeterTopPad = 22.0f;
constexpr float kMeterBottomPad = 18.0f;
constexpr float kMeterGap = 8.0f;
constexpr float kAttenW = 18.0f;
constexpr float kHandlePad = 8.0f;
constexpr float kMinPanelW = 200.0f;
constexpr float kMinPanelH = 340.0f;

constexpr float kThreshMinDb = -20.0f;
constexpr float kThreshMaxDb = 0.0f;
constexpr float kCeilingMinDb = -12.0f;
constexpr float kCeilingMaxDb = 0.0f;
constexpr float kAttenMaxDb = 20.0f;

// One meter tick per JUCE timer tick, so the release coefficients below land on the
// same ballistics they were tuned for at 30 Hz.
constexpr float kMeterInterval = 1.0f / 30.0f;

constexpr unsigned int kPanelFill = 0x20202aff;
constexpr unsigned int kPanelBorder = 0x3a3a46ff;
constexpr unsigned int kHeaderAccent = 0xe05050ff;
constexpr unsigned int kCaptionText = 0xa0a0a8ff;
constexpr unsigned int kWellTop = 0x0c0c0cff;
constexpr unsigned int kWellBottom = 0x181818ff;
constexpr unsigned int kSegmentFill = 0x242429ff;
constexpr unsigned int kSegmentTop = 0x303036ff;
constexpr unsigned int kThreshFillTop = 0xd06060ff;
constexpr unsigned int kFillBottom = 0x5ac8e0ff;
constexpr unsigned int kCeilingFillTop = 0xd05050ff;
constexpr unsigned int kAttenFillTop = 0xe04040ff;
constexpr unsigned int kAttenFillBottom = 0xe0c050ff;
constexpr unsigned int kThreshLine = 0x80b0e0e6;
constexpr unsigned int kThreshBoxBorder = 0x5a8ad0ff;
constexpr unsigned int kThreshBoxText = 0x80b0e0ff;
constexpr unsigned int kCeilingLine = 0xe05050e6;
constexpr unsigned int kCeilingBoxText = 0xff8080ff;
constexpr unsigned int kValueBoxFill = 0x181820ff;
constexpr unsigned int kTickText = 0x707074ff;
constexpr unsigned int kGrText = 0xf09060ff;
constexpr unsigned int kLufsFill = 0x0d1218ff;
constexpr unsigned int kLufsBorder = 0x5ac8e0ff;
constexpr unsigned int kLufsCaption = 0x80c0d0ff;
constexpr unsigned int kLufsValue = 0xf0f0f0ff;
constexpr unsigned int kLufsSub = 0x909094ff;
constexpr unsigned int kKnobAccent = 0x5a8ad0ff;

const char* const kModeItems[] = { "Modern", "Transparent", "Punchy" };

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

float clampf (float lo, float hi, float value)
{
    return std::clamp (value, lo, std::max (lo, hi));
}

class MasteringLimiterViewImpl final : public DuskPanelView
{
public:
    MasteringLimiterViewImpl (MasteringParams& p, BrickwallLimiter& l)
        : params (p), limiter (l)
    {
    }

    ImVec2 preferredSize() const override { return ImVec2 (kMinPanelW, kMinPanelH); }
    bool wantsPlate() const override { return false; }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        const ImVec2 br (origin.x + size.x, origin.y + size.y);
        ctx.dl->AddRectFilled (origin, br, rgba (kPanelFill));
        ctx.dl->AddRect (ImVec2 (origin.x + 0.5f, origin.y + 0.5f),
                         ImVec2 (br.x - 0.5f, br.y - 0.5f), rgba (kPanelBorder), 0.0f, 0,
                         ctx.s (1.0f));

        tickMeters();

        const float inset = ctx.s (kOuterInset);
        const float left = origin.x + inset;
        const float right = br.x - inset;
        const float bottom = br.y - inset;
        if (right - left <= ctx.s (60.0f))
            return;

        float y = origin.y + inset;
        drawHeader (ctx, ImVec2 (left, y), right - left);
        y += ctx.s (kHeaderH) + ctx.s (kHeaderGap);

        const float rightColLeft = right - ctx.s (kRightColW);
        drawRightColumn (ctx, ImVec2 (rightColLeft, y), ImVec2 (right, bottom));

        const float metersRight = rightColLeft - ctx.s (kRightColGap);
        if (metersRight - left <= ctx.s (60.0f))
            return;
        drawMeters (ctx, ImVec2 (left, y), ImVec2 (metersRight, bottom));
    }

private:
    void tickMeters()
    {
        meterClock += ImGui::GetIO().DeltaTime;
        if (meterClock < kMeterInterval)
            return;
        meterClock = 0.0f;

        const float gr = limiter.getCurrentGrDb();
        if (gr < displayedGrDb)
            displayedGrDb = gr;
        else
            displayedGrDb += (gr - displayedGrDb) * 0.18f;

        // Pre / post limiter levels, approximated from the post-master meters the
        // mastering chain writes at the end of each block.
        const float post = std::max (params.meterPostMasterLDb.load (std::memory_order_relaxed),
                                     params.meterPostMasterRDb.load (std::memory_order_relaxed));
        if (post > displayedOutDb)
            displayedOutDb = post;
        else
            displayedOutDb += (post - displayedOutDb) * 0.15f;

        // The driven input is the post-limiter peak plus whatever the limiter pulled
        // out, so with no reduction the two columns read alike.
        const float driven = post - displayedGrDb;
        if (driven > displayedInDb)
            displayedInDb = driven;
        else
            displayedInDb += (driven - displayedInDb) * 0.15f;
    }

    void drawHeader (dw::Context& ctx, ImVec2 at, float width)
    {
        const bool engaged = params.limiterEnabled.load (std::memory_order_relaxed);
        const auto pill = dw::modulePill (ctx, "##limiter-header", at,
                                          ImVec2 (at.x + width, at.y + ctx.s (kHeaderH)),
                                          "LIMITER", engaged, rgba (kHeaderAccent));
        if (pill.toggled || pill.labelClicked)
            params.limiterEnabled.store (! engaged, std::memory_order_relaxed);
    }

    void drawRightColumn (dw::Context& ctx, ImVec2 tl, ImVec2 br)
    {
        const float left = tl.x + ctx.s (4.0f);
        const float right = br.x - ctx.s (4.0f);
        const float width = right - left;
        if (width <= ctx.s (20.0f))
            return;

        float y = tl.y;
        const auto caption = [&] (const char* label)
        {
            dw::text (ctx, ctx.fonts->pill, ctx.s (10.5f), ImVec2 (left, y), width,
                      rgba (kCaptionText), label);
            y += ctx.s (kCaptionH);
        };

        caption ("Mode");
        y += ctx.s (2.0f);
        {
            // Clamped at the parameter boundary so a stale value cannot select an item
            // the list does not have, which would render the box empty.
            mode = std::clamp (params.limiterMode.load (std::memory_order_relaxed), 0, 2);
            const ScopedFormStyle style (ctx);
            if (formCombo (ctx, "##limiter-mode", ImVec2 (left, y),
                           ImVec2 (right, y + ctx.s (kComboH)), kModeItems, 3, mode))
                params.limiterMode.store (mode, std::memory_order_relaxed);
        }
        y += ctx.s (kComboH) + ctx.s (12.0f);

        // The block carries the value readout under the dial, so the dial is the block
        // less the line of text below it.
        const float knobRadius = (std::min (width, ctx.s (kKnobBlockH)) - ctx.s (18.0f))
                               * 0.5f;
        const float centreX = (left + right) * 0.5f;

        caption ("Release");
        y += drawKnobRow (ctx, "##limiter-release", centreX, y, knobRadius,
                          params.limiterReleaseMs,
                          dw::Range::withMidPoint (10.0f, 1000.0f, 100.0f), 100.0f, "%.0f ms");
        y += ctx.s (8.0f);

        caption ("Lookahead");
        y += drawKnobRow (ctx, "##limiter-lookahead", centreX, y, knobRadius,
                          params.limiterLookaheadMs, dw::Range (0.1f, 10.0f), 2.0f, "%.1f ms");
        formTooltip ("More lookahead catches transients more cleanly but adds latency.");
        y += ctx.s (8.0f);

        stereoLink = params.limiterStereoLink.load (std::memory_order_relaxed);
        {
            const ScopedFormStyle style (ctx);
            if (formCheckbox (ctx, "##limiter-link", ImVec2 (left, y), ctx.s (kToggleH),
                              "Stereo link", stereoLink))
                params.limiterStereoLink.store (stereoLink, std::memory_order_relaxed);
            formTooltip ("When on, gain reduction is matched across L/R to preserve the "
                         "stereo image. Off limits each channel independently.");
        }
    }

    float drawKnobRow (dw::Context& ctx, const char* id, float centreX, float top,
                       float radius, std::atomic<float>& parameter, const dw::Range& range,
                       float defaultValue, const char* format)
    {
        const float value = parameter.load (std::memory_order_relaxed);
        char readout[24];
        std::snprintf (readout, sizeof readout, format, static_cast<double> (value));

        dw::KnobStyle style;
        style.fill = rgba (kKnobAccent);
        style.value = readout;

        const auto result = dw::knob (ctx, id, ImVec2 (centreX, top + radius + ctx.s (2.0f)),
                                      radius, value, range, defaultValue, style);
        if (result.changed)
            parameter.store (clampf (range.start, range.end, result.value),
                             std::memory_order_relaxed);
        return ctx.s (kKnobBlockH) + ctx.s (2.0f);
    }

    void drawMeters (dw::Context& ctx, ImVec2 tl, ImVec2 br)
    {
        const float lufsTop = br.y - ctx.s (kLufsBoxH);
        const float metersTop = tl.y + ctx.s (kMeterTopPad);
        const float metersBottom = lufsTop - ctx.s (4.0f) - ctx.s (kMeterBottomPad);
        if (metersBottom - metersTop <= ctx.s (30.0f))
            return;

        const float available = (br.x - tl.x) - ctx.s (kAttenW) - 2.0f * ctx.s (kMeterGap)
                              - 2.0f * ctx.s (kHandlePad);
        const float columnW = std::max (ctx.s (12.0f), available * 0.5f);

        float x = tl.x + ctx.s (kHandlePad);
        const Rect thresh { x, metersTop, x + columnW, metersBottom };
        x += columnW + ctx.s (kMeterGap) + ctx.s (kHandlePad);
        const Rect ceiling { x, metersTop, x + columnW, metersBottom };
        x += columnW + ctx.s (kMeterGap);
        const Rect atten { x, metersTop, x + ctx.s (kAttenW), metersBottom };

        drawThreshold (ctx, thresh);
        drawCeiling (ctx, ceiling);
        drawAtten (ctx, atten);
        drawLufs (ctx, ImVec2 (tl.x, lufsTop), ImVec2 (br.x, br.y));
    }

    struct Rect { float x0, y0, x1, y1; };

    static float dbToY (float db, float minDb, float maxDb, const Rect& bar)
    {
        const float frac = clampf (0.0f, 1.0f, (db - minDb) / (maxDb - minDb));
        return bar.y1 - 2.0f - frac * ((bar.y1 - bar.y0) - 4.0f);
    }

    static float yToDb (float y, float minDb, float maxDb, const Rect& bar)
    {
        const float travel = std::max (1.0f, (bar.y1 - bar.y0) - 4.0f);
        const float frac = clampf (0.0f, 1.0f, (bar.y1 - 2.0f - y) / travel);
        return clampf (minDb, maxDb, minDb + frac * (maxDb - minDb));
    }

    void drawWell (dw::Context& ctx, const Rect& bar)
    {
        auto& dl = *ctx.dl;
        const ImVec2 tl (bar.x0, bar.y0);
        const ImVec2 br (bar.x1, bar.y1);
        // Flat rather than the JUCE well's top-to-bottom ramp: a rounded rectangle and a
        // gradient cannot be the same draw-list call, and the two ends of that ramp are
        // twelve greys apart under a ladder that covers most of it.
        dl.AddRectFilled (tl, br, dw::lerpColour (rgba (kWellTop), rgba (kWellBottom), 0.5f),
                          ctx.s (5.0f));
        dl.AddRect (ImVec2 (tl.x + ctx.s (1.0f), tl.y + ctx.s (1.0f)),
                    ImVec2 (br.x - ctx.s (1.0f), br.y - ctx.s (1.0f)),
                    IM_COL32 (0, 0, 0, 128), ctx.s (4.0f), 0, ctx.s (1.0f));

        constexpr int kSegments = 20;
        const float gap = ctx.s (2.0f);
        const float innerX0 = tl.x + ctx.s (4.0f);
        const float innerX1 = br.x - ctx.s (4.0f);
        const float innerY0 = tl.y + ctx.s (4.0f);
        const float innerH = (br.y - ctx.s (4.0f)) - innerY0;
        const float segH = (innerH - (kSegments - 1) * gap) / kSegments;
        if (segH <= 0.0f)
            return;

        for (int s = 0; s < kSegments; ++s)
        {
            const float y0 = innerY0 + static_cast<float> (s) * (segH + gap);
            dl.AddRectFilled (ImVec2 (innerX0, y0), ImVec2 (innerX1, y0 + segH),
                              rgba (kSegmentFill), ctx.s (2.0f));
            if (innerX1 - innerX0 > ctx.s (4.0f))
                dl.AddRectFilled (ImVec2 (innerX0 + ctx.s (2.0f), y0 + ctx.s (1.0f)),
                                  ImVec2 (innerX1 - ctx.s (2.0f), y0 + ctx.s (2.0f)),
                                  rgba (kSegmentTop));
        }
    }

    void drawCaption (dw::Context& ctx, const Rect& bar, const char* label)
    {
        dw::text (ctx, ctx.fonts->pill, ctx.s (10.5f),
                  ImVec2 (bar.x0 - ctx.s (14.0f), bar.y0 - ctx.s (18.0f)),
                  (bar.x1 - bar.x0) + ctx.s (28.0f), rgba (kCaptionText), label);
    }

    void drawLevelFill (dw::Context& ctx, const Rect& bar, float frac, unsigned int top,
                        unsigned int bottom)
    {
        if (frac <= 0.001f)
            return;
        const float height = ((bar.y1 - bar.y0) - ctx.s (4.0f)) * frac;
        // The gradient spans the whole well rather than the lit part, so a column that
        // is only half up still shows the colour that height means.
        const ImU32 topColour = rgba (top);
        const ImU32 bottomColour = rgba (bottom);
        const float t = 1.0f - frac;
        ctx.dl->AddRectFilledMultiColor (
            ImVec2 (bar.x0 + ctx.s (2.0f), bar.y1 - ctx.s (2.0f) - height),
            ImVec2 (bar.x1 - ctx.s (2.0f), bar.y1 - ctx.s (2.0f)),
            dw::lerpColour (topColour, bottomColour, t),
            dw::lerpColour (topColour, bottomColour, t), bottomColour, bottomColour);
    }

    void drawValueBox (dw::Context& ctx, const Rect& bar, float boxY, unsigned int border,
                       unsigned int textColour, const char* value)
    {
        auto& dl = *ctx.dl;
        const ImVec2 tl (bar.x0 + ctx.s (2.0f), boxY);
        const ImVec2 br (bar.x1 - ctx.s (2.0f), boxY + ctx.s (14.0f));
        dl.AddRectFilled (tl, br, rgba (kValueBoxFill), ctx.s (2.0f));
        dl.AddRect (tl, br, rgba (border), ctx.s (2.0f), 0, ctx.s (0.6f));
        const float size = ctx.s (9.5f);
        dw::text (ctx, ctx.fonts->pill, size,
                  ImVec2 (tl.x, tl.y + (ctx.s (14.0f) - size) * 0.5f), br.x - tl.x,
                  rgba (textColour), value);
    }

    void drawThreshold (dw::Context& ctx, const Rect& bar)
    {
        drawWell (ctx, bar);
        drawLevelFill (ctx, bar,
                       clampf (0.0f, 1.0f,
                               (clampf (kThreshMinDb, kThreshMaxDb, displayedInDb)
                                - kThreshMinDb) / (kThreshMaxDb - kThreshMinDb)),
                       kThreshFillTop, kFillBottom);

        // The limiter has no threshold of its own - it drives the input into the
        // ceiling - so the handle sits at minus the drive.
        const float drive = params.limiterDriveDb.load (std::memory_order_relaxed);
        const float handleY = dbToY (-drive, kThreshMinDb, kThreshMaxDb, bar);
        ctx.dl->AddLine (ImVec2 (bar.x0, handleY), ImVec2 (bar.x1, handleY),
                         rgba (kThreshLine), ctx.s (1.4f));
        drawCaption (ctx, bar, "Threshold");

        char value[16];
        std::snprintf (value, sizeof value, "%.2f", static_cast<double> (-drive));
        drawValueBox (ctx, bar,
                      clampf (bar.y0 + ctx.s (1.0f), bar.y1 - ctx.s (15.0f),
                              handleY + ctx.s (1.0f)),
                      kThreshBoxBorder, kThreshBoxText, value);

        float dragged = 0.0f;
        const auto action = dragColumn (ctx, "##limiter-thresh", bar, dragged,
                                        threshResetHeld);
        if (action == DragAction::reset)
            params.limiterDriveDb.store (0.0f, std::memory_order_relaxed);
        else if (action == DragAction::moved)
            params.limiterDriveDb.store (-yToDb (dragged, kThreshMinDb, kThreshMaxDb, bar),
                                         std::memory_order_relaxed);
    }

    void drawCeiling (dw::Context& ctx, const Rect& bar)
    {
        drawWell (ctx, bar);
        drawLevelFill (ctx, bar,
                       clampf (0.0f, 1.0f,
                               (clampf (kCeilingMinDb, kCeilingMaxDb, displayedOutDb)
                                - kCeilingMinDb) / (kCeilingMaxDb - kCeilingMinDb)),
                       kCeilingFillTop, kFillBottom);

        const float ceiling = params.limiterCeilingDb.load (std::memory_order_relaxed);
        const float handleY = dbToY (ceiling, kCeilingMinDb, kCeilingMaxDb, bar);
        ctx.dl->AddLine (ImVec2 (bar.x0, handleY), ImVec2 (bar.x1, handleY),
                         rgba (kCeilingLine), ctx.s (1.4f));
        drawCaption (ctx, bar, "Ceiling");

        char value[16];
        std::snprintf (value, sizeof value, "%.2f", static_cast<double> (ceiling));
        drawValueBox (ctx, bar,
                      clampf (bar.y0 + ctx.s (1.0f), bar.y1 - ctx.s (15.0f),
                              handleY - ctx.s (15.0f)),
                      kCeilingLine, kCeilingBoxText, value);

        float dragged = 0.0f;
        const auto action = dragColumn (ctx, "##limiter-ceiling", bar, dragged,
                                        ceilingResetHeld);
        if (action == DragAction::reset)
            params.limiterCeilingDb.store (-0.3f, std::memory_order_relaxed);
        else if (action == DragAction::moved)
            params.limiterCeilingDb.store (yToDb (dragged, kCeilingMinDb, kCeilingMaxDb, bar),
                                           std::memory_order_relaxed);
    }

    enum class DragAction { none, moved, reset };

    // The whole column is the drag target, the way the JUCE editor's expanded hit
    // rectangle was, so the line can be grabbed anywhere across the meter.
    //
    // `resetHeld` is what keeps a double-click's reset: the second press leaves the
    // button down, and without it the frames before the release would drag the value
    // straight back to wherever the pointer was clicked.
    DragAction dragColumn (dw::Context& ctx, const char* id, const Rect& bar, float& atY,
                           bool& resetHeld)
    {
        const float padX = ctx.s (10.0f);
        const float padY = ctx.s (4.0f);
        dw::hitArea (ctx, id, ImVec2 (bar.x0 - padX, bar.y0 - padY),
                     ImVec2 (bar.x1 + padX, bar.y1 + padY));

        if (! ImGui::IsItemActive())
        {
            resetHeld = false;
            return DragAction::none;
        }
        if (ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
        {
            resetHeld = true;
            return DragAction::reset;
        }
        if (resetHeld)
            return DragAction::none;

        atY = ImGui::GetIO().MousePos.y;
        return DragAction::moved;
    }

    void drawAtten (dw::Context& ctx, const Rect& bar)
    {
        drawWell (ctx, bar);

        const float reduction = clampf (0.0f, kAttenMaxDb, std::abs (displayedGrDb));
        const float frac = reduction / kAttenMaxDb;
        if (frac > 0.001f)
        {
            const float height = ((bar.y1 - bar.y0) - ctx.s (4.0f)) * frac;
            ctx.dl->AddRectFilledMultiColor (
                ImVec2 (bar.x0 + ctx.s (2.0f), bar.y0 + ctx.s (2.0f)),
                ImVec2 (bar.x1 - ctx.s (2.0f), bar.y0 + ctx.s (2.0f) + height),
                rgba (kAttenFillTop), rgba (kAttenFillTop),
                dw::lerpColour (rgba (kAttenFillTop), rgba (kAttenFillBottom), frac),
                dw::lerpColour (rgba (kAttenFillTop), rgba (kAttenFillBottom), frac));
        }
        drawCaption (ctx, bar, "Atten");

        const float size = ctx.s (8.5f);
        for (const auto& entry : { std::pair<float, const char*> { 3.0f, "3" },
                                   std::pair<float, const char*> { 6.0f, "6" },
                                   std::pair<float, const char*> { 12.0f, "12" } })
        {
            const float y = bar.y0 + ctx.s (2.0f)
                          + entry.first / kAttenMaxDb * ((bar.y1 - bar.y0) - ctx.s (4.0f));
            dw::text (ctx, ctx.fonts->caption, size,
                      ImVec2 (bar.x1 + ctx.s (2.0f), y - size * 0.5f), ctx.s (18.0f),
                      rgba (kTickText), entry.second, dw::Align::left);
        }

        char readout[16];
        std::snprintf (readout, sizeof readout, "%.1f", static_cast<double> (displayedGrDb));
        dw::text (ctx, ctx.fonts->pill, ctx.s (9.5f),
                  ImVec2 (bar.x0, bar.y1 + ctx.s (2.0f)), bar.x1 - bar.x0, rgba (kGrText),
                  readout);
    }

    void drawLufs (dw::Context& ctx, ImVec2 tl, ImVec2 br)
    {
        const float width = std::min (ctx.s (180.0f), br.x - tl.x);
        const float x0 = tl.x + ((br.x - tl.x) - width) * 0.5f;
        const ImVec2 boxTl (x0 + ctx.s (1.0f), tl.y + ctx.s (1.0f));
        const ImVec2 boxBr (x0 + width - ctx.s (1.0f), tl.y + ctx.s (kLufsBoxH) - ctx.s (1.0f));

        auto& dl = *ctx.dl;
        dl.AddRectFilled (boxTl, boxBr, rgba (kLufsFill), ctx.s (3.0f));
        dl.AddRect (boxTl, boxBr, rgba (kLufsBorder), ctx.s (3.0f), 0, ctx.s (0.8f));

        const float boxW = boxBr.x - boxTl.x;
        dw::text (ctx, ctx.fonts->pill, ctx.s (10.0f),
                  ImVec2 (boxTl.x, boxTl.y + ctx.s (4.0f)), boxW, rgba (kLufsCaption),
                  "LUFS Long");

        char value[16];
        const float integrated = params.meterIntegratedLufs.load (std::memory_order_relaxed);
        if (integrated <= -99.0f)
            std::snprintf (value, sizeof value, "--");
        else
            std::snprintf (value, sizeof value, "%.1f", static_cast<double> (integrated));
        const float valueSize = ctx.s (24.0f);
        dw::text (ctx, ctx.fonts->valueLarge, valueSize,
                  ImVec2 (boxTl.x, boxTl.y + ctx.s (24.0f)), boxW, rgba (kLufsValue), value);

        char peak[24];
        const float truePeak = params.meterTruePeakDb.load (std::memory_order_relaxed);
        if (truePeak <= -99.0f)
            std::snprintf (peak, sizeof peak, "dBTP --");
        else
            std::snprintf (peak, sizeof peak, "dBTP %.1f", static_cast<double> (truePeak));

        char shortTerm[24];
        const float shortLufs = params.meterShortTermLufs.load (std::memory_order_relaxed);
        if (shortLufs <= -99.0f)
            std::snprintf (shortTerm, sizeof shortTerm, "Short --");
        else
            std::snprintf (shortTerm, sizeof shortTerm, "Short %.1f",
                           static_cast<double> (shortLufs));

        const float subSize = ctx.s (8.5f);
        const float subY = boxBr.y - ctx.s (14.0f);
        dw::text (ctx, ctx.fonts->caption, subSize, ImVec2 (boxTl.x, subY), boxW * 0.5f,
                  rgba (kLufsSub), peak);
        dw::text (ctx, ctx.fonts->caption, subSize,
                  ImVec2 (boxTl.x + boxW * 0.5f, subY), boxW * 0.5f, rgba (kLufsSub),
                  shortTerm);
    }

    MasteringParams& params;
    BrickwallLimiter& limiter;

    float displayedInDb = -100.0f;
    float displayedOutDb = -100.0f;
    float displayedGrDb = 0.0f;
    float meterClock = 0.0f;
    int mode = 0;
    bool stereoLink = false;
    bool threshResetHeld = false;
    bool ceilingResetHeld = false;
};
} // namespace

std::unique_ptr<DuskPanelView> makeMasteringLimiterView (MasteringParams& params,
                                                         BrickwallLimiter& limiter)
{
    return std::unique_ptr<DuskPanelView> (new MasteringLimiterViewImpl (params, limiter));
}
} // namespace duskstudio::imgui
