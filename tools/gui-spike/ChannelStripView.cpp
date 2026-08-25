#include "ChannelStripView.h"
#include "StripLayout.h"

#include "../../src/ui/imgui/DuskTheme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>

namespace duskspike
{
namespace
{
using duskstudio::imgui::consolePalette;

namespace dw = DuskWidgets;

// Every knob on the strip: read the parameter, run the widget, write back what came out.
bool knob (dw::Context& ctx, const char* id, ImVec2 centre, float radius,
           std::atomic<float>& value, const Range& range, float defaultValue,
           unsigned int fill, const char* caption, const char* valueText,
           unsigned int outline = 0)
{
    dw::KnobStyle style;
    style.fill = fill;
    style.outline = outline;
    style.caption = caption;
    style.value = valueText;

    const auto result = dw::knob (ctx, id, centre, radius,
                                  value.load (std::memory_order_relaxed), range, defaultValue,
                                  style);
    if (result.changed)
        value.store (result.value, std::memory_order_relaxed);
    return result.dragging;
}

bool button (dw::Context& ctx, const char* id, ImVec2 tl, ImVec2 br, const char* label,
             std::atomic<bool>& flag, unsigned int onFill, unsigned int offText,
             unsigned int onText, float fontSize)
{
    dw::ButtonStyle style;
    style.onFill = onFill;
    style.offText = offText;
    style.onText = onText;
    style.fontSize = fontSize;

    const bool on = flag.load (std::memory_order_relaxed);
    if (! dw::textButton (ctx, id, tl, br, label, on, style).clicked)
        return false;

    flag.store (! on, std::memory_order_relaxed);
    return true;
}
} // namespace

// --------------------------------------------------------------------------------------

StripFrameResult ChannelStripView::draw (dw::Context& ctx, ImVec2 origin, float width,
                                         float height, StripParams& params)
{
    const auto& pal = consolePalette();
    const auto& theme = pal.widgets;
    auto* const dl = ctx.dl;

    StripFrameResult result;
    const int widgetsBefore = ctx.widgets;
    dragging = false;

    const float scale = ctx.scale;
    auto s = [scale] (float v) { return v * scale; };
    char buf[32];

    dw::panel (ctx, origin, ImVec2 (origin.x + width, origin.y + height),
               pal.stripFill, pal.stripBorder, 1.0f, s (4.0f), s (1.0f));

    dl->AddRectFilled (ImVec2 (origin.x + s (1.5f), origin.y + s (1.5f)),
                       ImVec2 (origin.x + width - s (1.5f), origin.y + s (5.5f)),
                       dw::withAlpha (pal.trackColour, 0.85f), s (2.0f));

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
            const auto field = dw::textField (ctx, "##name", tl, br, nameBuffer,
                                              sizeof nameBuffer, nameFocusPending);
            nameFocusPending = false;
            if (field.committed)
            {
                params.name = nameBuffer;
                editingName = false;
            }
            else if (field.cancelled)
            {
                editingName = false;
            }
        }
        else
        {
            const auto label = dw::textButton (ctx, "##nameLabel", tl, br, nullptr, false,
                                               dw::ButtonStyle { 0, 0, 0, 0, nullptr, 0.0f,
                                                                 0.0f, false });
            if (label.hovered && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left))
            {
                std::snprintf (nameBuffer, sizeof nameBuffer, "%s", params.name.c_str());
                editingName = true;
            }
            dw::text (ctx, ctx.fonts->title, s (13.0f), ImVec2 (tl.x, tl.y + s (3.0f)), innerW,
                      theme.textBright, params.name.c_str());
        }
        y = br.y + s (2.0f);
    }

    {
        dw::ButtonStyle style;
        style.offFill = theme.buttonPanel;
        style.offText = pal.ioText;
        if (dw::textButton (ctx, "##io", ImVec2 (left, y),
                            ImVec2 (right, y + s (layout::kIoButtonH)), "MONO  IN 1", false,
                            style)
                .clicked)
            result.openIoModal = true;
    }
    y += s (layout::kIoButtonH) + s (3.0f);

    {
        const float w3 = innerW / 3.0f;
        const float h = s (layout::kRowH);
        struct { const char* id; const char* label; std::atomic<bool>* flag;
                 unsigned int on; unsigned int offText; unsigned int onText; } row[] = {
            { "##in",    "IN",    &params.inputMonitor, pal.panCyan, 0xff908070u, theme.textOn },
            { "##arm",   "ARM",   &params.recordArmed,  pal.armOn,   pal.armOffText,
              theme.textBright },
            { "##print", "PRINT", &params.printEffects, pal.printOn, 0xff60708au, theme.textOn },
        };
        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 tl (left + w3 * i + s (1.0f), y + s (1.0f));
            const ImVec2 br (left + w3 * (i + 1) - s (1.0f), y + h - s (1.0f));
            button (ctx, row[i].id, tl, br, row[i].label, *row[i].flag, row[i].on,
                    row[i].offText, row[i].onText, 9.5f);
        }
        y += h;
    }

    {
        const ImVec2 tl (left + s (3.0f), y), br (right - s (3.0f), y + s (layout::kRowH));
        dw::ButtonStyle style;
        style.offFill = theme.buttonPanel;
        style.offText = pal.insertText;
        const auto insert = dw::textButton (ctx, "##insert", tl, br, "Baroness", false, style);
        if (insert.rightClicked)
        {
            result.openInsertMenu = true;
            result.insertMenuAt = ImGui::GetIO().MousePos;
        }
        dl->AddCircleFilled (ImVec2 (tl.x + s (8.0f), (tl.y + br.y) * 0.5f), s (4.0f),
                             IM_COL32 (0x60, 0xd0, 0x60, 0xff), 16);
        y = br.y + s (2.0f);
    }

    // EQ
    {
        const float h = s (layout::kEqPanelH);
        dw::panel (ctx, ImVec2 (left, y), ImVec2 (right, y + h), pal.eqFill, pal.eqAccent,
                   0.40f, s (3.0f), s (0.8f));

        const float px = left + s (3.0f);
        const float pw = innerW - s (6.0f);
        float py = y;

        const ImVec2 chipTL (px + pw - s (28.0f), py + s (1.0f));
        const ImVec2 chipBR (px + pw - s (1.0f), py + s (layout::kEqHeaderH) - s (1.0f));
        const bool black = params.eqBlackMode.load (std::memory_order_relaxed);
        {
            dw::ButtonStyle style;
            style.onFill = pal.eqTypeChip;
            style.offText = theme.textBright;
            style.onText = theme.textBright;
            if (dw::textButton (ctx, "##eqtype", chipTL, chipBR, black ? "G" : "E", black, style)
                    .clicked)
                params.eqBlackMode.store (! black, std::memory_order_relaxed);
        }

        const bool eqOn = params.eqEnabled.load (std::memory_order_relaxed);
        if (dw::modulePill (ctx, "##eqhdr", ImVec2 (px, py),
                            ImVec2 (chipTL.x - s (2.0f), py + s (layout::kEqHeaderH)), "EQ",
                            eqOn, pal.lfGreen)
                .toggled)
            params.eqEnabled.store (! eqOn, std::memory_order_relaxed);
        py += s (layout::kEqHeaderH);

        const float halfW = pw * 0.5f;
        dw::text (ctx, ctx.fonts->band, s (12.0f), ImVec2 (px, py), halfW, theme.textDim, "HPF");
        dw::text (ctx, ctx.fonts->band, s (12.0f), ImVec2 (px + halfW, py), halfW, theme.textDim,
                  "LPF");
        py += s (layout::kFilterLabelH);

        const float kr = s (layout::kKnobSize) * 0.5f;
        dw::formatFrequency (buf, sizeof buf, params.hpfFreq.load (std::memory_order_relaxed));
        dragging |= knob (ctx, "##hpf", ImVec2 (px + halfW * 0.5f, py + kr), kr, params.hpfFreq,
                          StripParams::hpfRange(), 20.0f, pal.filterWhite, nullptr, buf);
        dw::formatFrequency (buf, sizeof buf, params.lpfFreq.load (std::memory_order_relaxed));
        dragging |= knob (ctx, "##lpf", ImVec2 (px + halfW * 1.5f, py + kr), kr, params.lpfFreq,
                          StripParams::lpfRange(), 20000.0f, pal.filterWhite, nullptr, buf);
        py += s (layout::kKnobBlockH);

        dl->AddLine (ImVec2 (px, py + s (2.5f)), ImVec2 (px + pw, py + s (2.5f)),
                     dw::withAlpha (pal.eqAccent, 0.18f), s (1.0f));
        py += s (layout::kFilterBandGap);

        const float colW = (pw - s (layout::kRowLabelW)) / 3.0f;
        const char* heads[] = { "GAIN", "FREQ", "Q" };
        for (int i = 0; i < 3; ++i)
            dw::text (ctx, ctx.fonts->caption, s (8.0f),
                      ImVec2 (px + s (layout::kRowLabelW) + colW * i, py), colW, theme.textDim,
                      heads[i]);
        py += s (layout::kColumnHeaderH);

        struct Band { const char* label; std::atomic<float>* gain; std::atomic<float>* freq;
                      std::atomic<float>* q; const Range* freqRange; float freqDefault;
                      unsigned int colour; };
        const Band bands[] = {
            { "HF", &params.hfGainDb, &params.hfFreq, nullptr, &StripParams::hfFreqRange(),
              8000.0f, pal.hfRed },
            { "HM", &params.hmGainDb, &params.hmFreq, &params.hmQ, &StripParams::hmFreqRange(),
              2000.0f, pal.hmGreen },
            { "LM", &params.lmGainDb, &params.lmFreq, &params.lmQ, &StripParams::lmFreqRange(),
              600.0f, pal.lmBlue },
            { "LF", &params.lfGainDb, &params.lfFreq, nullptr, &StripParams::lfFreqRange(),
              100.0f, pal.lfGraphite },
        };

        for (int b = 0; b < 4; ++b)
        {
            const float rowY = py + (s (layout::kEqBandRowH) + s (layout::kEqBandGap)) * b;
            dw::text (ctx, ctx.fonts->band, s (12.0f), ImVec2 (px, rowY + s (7.0f)),
                      s (layout::kRowLabelW), bands[b].colour, bands[b].label);

            const float cy = rowY + kr;
            char id[24];

            dw::formatGain (buf, sizeof buf, bands[b].gain->load (std::memory_order_relaxed));
            std::snprintf (id, sizeof id, "##g%d", b);
            dragging |= knob (ctx, id, ImVec2 (px + s (layout::kRowLabelW) + colW * 0.5f, cy), kr,
                              *bands[b].gain, StripParams::gainRange(), 0.0f, bands[b].colour,
                              nullptr, buf);

            dw::formatFrequency (buf, sizeof buf,
                                 bands[b].freq->load (std::memory_order_relaxed));
            std::snprintf (id, sizeof id, "##f%d", b);
            dragging |= knob (ctx, id, ImVec2 (px + s (layout::kRowLabelW) + colW * 1.5f, cy), kr,
                              *bands[b].freq, *bands[b].freqRange, bands[b].freqDefault,
                              bands[b].colour, nullptr, buf);

            if (bands[b].q != nullptr)
            {
                std::snprintf (buf, sizeof buf, "%.2f",
                               bands[b].q->load (std::memory_order_relaxed));
                std::snprintf (id, sizeof id, "##q%d", b);
                dragging |= knob (ctx, id,
                                  ImVec2 (px + s (layout::kRowLabelW) + colW * 2.5f, cy), kr,
                                  *bands[b].q, StripParams::qRange(), 0.7f, bands[b].colour,
                                  nullptr, buf);
            }
        }

        y += h + s (3.0f);
    }

    // COMP
    {
        const float h = s (layout::kCompPanelH);
        dw::panel (ctx, ImVec2 (left, y), ImVec2 (right, y + h), pal.compFill, pal.compGold,
                   0.40f, s (3.0f), s (0.8f));

        const float px = left + s (3.0f);
        const float pw = innerW - s (6.0f);
        const bool compOn = params.compEnabled.load (std::memory_order_relaxed);
        if (dw::modulePill (ctx, "##comphdr", ImVec2 (px, y + s (2.0f)),
                            ImVec2 (px + pw, y + s (18.0f)), "COMP", compOn, pal.compGold)
                .toggled)
            params.compEnabled.store (! compOn, std::memory_order_relaxed);

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
            dw::text (ctx, ctx.fonts->label, s (9.0f), ImVec2 (cx - colW * 0.5f, bodyY), colW,
                      pal.compLabel, cells[i].label);

            const ImVec2 centre (cx, bodyY + s (layout::kCompKnobLabelH) + kr);
            if (cells[i].range == nullptr)
            {
                const int index = std::clamp (static_cast<int> (std::lround (
                    cells[i].value->load (std::memory_order_relaxed))), 0, 4);
                dragging |= knob (ctx, cells[i].id, centre, kr, *cells[i].value, ratioRange, 0.0f,
                                  pal.compGold, nullptr, kRatios[index]);
            }
            else
            {
                std::snprintf (buf, sizeof buf, cells[i].fmt,
                               cells[i].value->load (std::memory_order_relaxed));
                dragging |= knob (ctx, cells[i].id, centre, kr, *cells[i].value, *cells[i].range,
                                  cells[i].def, pal.compGold, nullptr, buf);
            }
        }

        y += h + s (6.0f);
    }

    // AUX sends
    {
        const float h = s (layout::kAuxPanelH);
        dw::panel (ctx, ImVec2 (left, y), ImVec2 (right, y + h), pal.sendFill, pal.sendPurple,
                   0.40f, s (3.0f), s (0.8f));

        const float px = left + s (3.0f);
        const float pw = innerW - s (6.0f);
        const float colW = pw / 4.0f;
        const float kr = s (layout::kAuxKnobSize) * 0.5f;
        const char* names[] = { "REV", "DLY", "CUE", "FX" };

        for (std::size_t i = 0; i < 4; ++i)
        {
            const float cx = px + colW * (static_cast<float> (i) + 0.5f);
            dw::text (ctx, ctx.fonts->caption, s (8.5f),
                      ImVec2 (cx - colW * 0.5f, y + s (3.0f)), colW, pal.sendPurple, names[i]);

            const float knobY = y + s (3.0f + 11.0f + 1.0f)
                              + (i % 2 == 0 ? 0.0f : s (layout::kAuxStaggerY));
            const int index = static_cast<int> (i);
            const float db = params.auxSendDb[i].load (std::memory_order_relaxed);
            if (db <= StripParams::kAuxSendOffDb + 0.5f)
                std::snprintf (buf, sizeof buf, "-");
            else
                std::snprintf (buf, sizeof buf, "%.0f", db);

            char id[16];
            std::snprintf (id, sizeof id, "##aux%d", index);
            dragging |= knob (ctx, id, ImVec2 (cx, knobY + kr), kr, params.auxSendDb[i],
                              StripParams::auxRange(), StripParams::kAuxSendOffDb,
                              pal.auxFill[i], nullptr, buf,
                              params.auxSendPreFader[i].load (std::memory_order_relaxed)
                                  ? pal.auxPreOutline : 0u);
        }

        y += h + s (3.0f);
    }

    // Fader column: pan on top, then bus buttons / fader / meter / GR side by side.
    {
        const float bottomBlock = s (layout::kPeakLabelH + 2.0f + layout::kAutoH + 4.0f
                                     + layout::kMsRowH);
        const float colBottom = origin.y + height - s (layout::kOuterInset) - bottomBlock;
        const float cx = (left + right) * 0.5f;

        const float panTop = y;
        dw::text (ctx, ctx.fonts->pill, s (10.5f), ImVec2 (cx - s (28.0f), panTop), s (56.0f),
                  theme.textDim, "PAN");
        const float pan = params.pan.load (std::memory_order_relaxed);
        if (std::fabs (pan) < 0.005f)      std::snprintf (buf, sizeof buf, "C");
        else if (pan < 0.0f)               std::snprintf (buf, sizeof buf, "L%.0f", -pan * 100.0f);
        else                               std::snprintf (buf, sizeof buf, "R%.0f", pan * 100.0f);
        dragging |= knob (ctx, "##pan",
                          ImVec2 (cx, panTop + s (layout::kPanLabelH) + s (13.0f)),
                          s (layout::kPanKnobSize) * 0.5f, params.pan, StripParams::panRange(),
                          0.0f, pal.panRed, nullptr, buf);

        const float faderTop = panTop + s (layout::kPanLabelH + layout::kPanBlockH
                                           + layout::kPanFaderGap);
        const float faderW = std::clamp (innerW - s (74.0f), s (22.0f), s (40.0f));

        {
            dw::FaderStyle style;
            style.gutterLeft = left + s (layout::kBusColumnW) + s (6.0f);
            style.trackInset = layout::kFaderTrackPad;
            style.capWidth = layout::kFaderCapW;
            style.capHeight = layout::kFaderCapH;

            const auto moved = dw::fader (ctx, "##fader",
                                          ImVec2 (cx - faderW * 0.5f, faderTop),
                                          ImVec2 (cx + faderW * 0.5f, colBottom),
                                          params.faderDb.load (std::memory_order_relaxed),
                                          StripParams::faderRange(), 0.0f, style);
            if (moved.changed)
                params.faderDb.store (moved.value, std::memory_order_relaxed);
            dragging |= moved.dragging;
        }

        // Meters share the fader's top and bottom trim so 0 dB is on the same scan line.
        const float meterTop = faderTop + s (layout::kFaderTrackPad);
        const float meterBottom = colBottom - s (layout::kFaderTrackPad);
        const float meterX = cx + faderW * 0.5f + s (1.0f);

        const float incoming = params.meterInputDb.load (std::memory_order_relaxed);
        if (staticMeters)
        {
            inputMeter.displayed = incoming;
            inputMeter.peakHold = incoming;
        }
        else
        {
            inputMeter.tick (incoming, ImGui::GetIO().Framerate > 1.0f
                                           ? ImGui::GetIO().Framerate / 30.0f : 2.0f);
        }
        dw::meter (ctx, ImVec2 (meterX, meterTop),
                   ImVec2 (meterX + s (layout::kMeterWidth), meterBottom), inputMeter.displayed,
                   inputMeter.peakHold);

        const float grX = meterX + s (layout::kMeterWidth) + s (1.0f);
        const float gr = params.meterGrDb.load (std::memory_order_relaxed);
        if (staticMeters)            displayedGrDb = gr;
        else if (gr < displayedGrDb) displayedGrDb = gr;
        else                         displayedGrDb += (gr - displayedGrDb) * 0.18f;

        {
            dw::GainReductionStyle style;
            style.handle = true;
            const auto threshold = dw::gainReduction (
                ctx, "##thr", ImVec2 (grX, meterTop),
                ImVec2 (grX + s (layout::kGrLedW), meterBottom), displayedGrDb,
                params.compFetThresholdDb.load (std::memory_order_relaxed), style);
            if (threshold.changed)
                params.compFetThresholdDb.store (threshold.threshold, std::memory_order_relaxed);
        }

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

            for (std::size_t i = 0; i < 4; ++i)
            {
                const int index = static_cast<int> (i);
                const ImVec2 tl (left + s (3.0f), stackTop + static_cast<float> (step * index));
                const ImVec2 br (tl.x + s (layout::kBusColumnW), tl.y + bh);
                char id[16];
                std::snprintf (id, sizeof id, "##bus%d", index);
                std::snprintf (buf, sizeof buf, "%d", index + 1);
                button (ctx, id, tl, br, buf, params.busAssign[i], pal.busColour[i],
                        dw::brighter (pal.busColour[i], 0.15f), theme.textOn, 10.0f);
            }
        }

        y = colBottom;
    }

    // Readouts, automation mode, mute / solo / phase.
    {
        dw::formatDecibels (buf, sizeof buf, params.faderDb.load (std::memory_order_relaxed));
        dw::text (ctx, ctx.fonts->valueLarge, s (14.0f), ImVec2 (left, y + s (2.0f)),
                  innerW * 0.55f, theme.textValue, buf);

        const float peak = inputMeter.peakHold;
        std::snprintf (buf, sizeof buf, "%.1f", peak);
        const unsigned int peakCol = peak >= -3.0f
                                       ? IM_COL32 (0xff, 0x50, 0x50, 0xff)
                                       : (peak >= -12.0f ? IM_COL32 (0xe0, 0xc0, 0x50, 0xff)
                                                         : theme.textValue);
        dw::text (ctx, ctx.fonts->value, s (11.0f),
                  ImVec2 (left + innerW * 0.55f, y + s (4.0f)), innerW * 0.45f, peakCol, buf,
                  dw::Align::right);
        y += s (layout::kPeakLabelH + 2.0f);

        {
            dw::ButtonStyle style;
            style.onFill = pal.autoReadOn;
            style.onText = pal.autoReadText;
            style.fontSize = 9.5f;
            dw::textButton (ctx, "##auto", ImVec2 (left + s (1.0f), y),
                            ImVec2 (right - s (1.0f), y + s (layout::kAutoH)), "AUTO: READ",
                            true, style);
        }
        y += s (layout::kAutoH + 4.0f);

        const float w3 = innerW / 3.0f;
        struct { const char* id; const char* label; std::atomic<bool>* flag;
                 unsigned int on; } msp[] = {
            { "##mute",  "M", &params.mute, pal.muteOn },
            { "##solo",  "S", &params.solo, pal.soloOn },
            { "##phase", "\xc3\x98", &params.phaseInvert, pal.phaseOn },
        };
        for (int i = 0; i < 3; ++i)
        {
            const ImVec2 tl (left + w3 * i + s (1.0f), y + s (1.0f));
            const ImVec2 br (left + w3 * (i + 1) - s (1.0f), y + s (layout::kMsRowH) - s (1.0f));
            button (ctx, msp[i].id, tl, br, msp[i].label, *msp[i].flag, msp[i].on,
                    theme.textDim, theme.textOn, 11.0f);
        }
    }

    lastWidgetCount = ctx.widgets - widgetsBefore;
    return result;
}

} // namespace duskspike
