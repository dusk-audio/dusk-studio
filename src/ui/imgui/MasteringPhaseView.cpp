#include "MasteringPhaseView.h"
#include "PanelControls.h"
#include "../../dsp/PhaseAnalyzer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;
constexpr ImU32 kWarning = IM_COL32 (240, 112, 96, 255);
constexpr ImU32 kHealthy = IM_COL32 (112, 194, 112, 255);
constexpr ImU32 kAccent = IM_COL32 (90, 138, 208, 255);
constexpr ImU32 kBright = IM_COL32 (224, 224, 230, 255);
constexpr ImU32 kMuted = IM_COL32 (150, 150, 160, 255);

struct Rect
{
    ImVec2 first {}, last {};
};

class MasteringPhaseViewImpl final : public DuskPanelView
{
public:
    MasteringPhaseViewImpl (MasteringPhaseParams& p, PhaseAnalyzer& a,
                           std::function<bool()> get, std::function<void (bool)> set)
        : params (p), analyzer (a), getMono (std::move (get)), setMono (std::move (set)) {}

    ImVec2 preferredSize() const override { return { 260.0f, 420.0f }; }
    bool wantsPlate() const override { return false; }
    bool capturesKeyboard() const override { return keyboardActive; }
    bool escapeDismisses() const override { return ! popupWasOpen && ! keyboardWasActive; }

    bool controlPointForScenario (const std::string& name, ImVec2& point, float fraction) const override
    {
        static constexpr const char* names[] = { "reset", "mono", "mode", "bands", "q", "averaging", "band" };
        for (size_t i = 0; i < controls.size(); ++i)
            if (name == names[i] && controls[i].last.x > controls[i].first.x)
            {
                const auto& r = controls[i];
                point = { std::min (r.last.x - 0.5f, std::max (r.first.x + 0.5f,
                              r.first.x + std::clamp (fraction, 0.0f, 1.0f) * (r.last.x - r.first.x))),
                          0.5f * (r.first.y + r.last.y) };
                return point.x >= viewport.first.x && point.x < viewport.last.x
                    && point.y >= viewport.first.y && point.y < viewport.last.y;
            }
        return false;
    }

    void draw (dw::Context& ctx, ImVec2 origin, ImVec2 size) override
    {
        popupWasOpen = popupOpen;
        keyboardWasActive = keyboardActive;
        controls = {};
        viewport = {};
        const ImVec2 bottomRight { origin.x + size.x, origin.y + size.y };
        ctx.dl->AddRectFilled (origin, bottomRight, IM_COL32 (32, 32, 42, 255));
        ctx.dl->AddRect (origin, bottomRight, IM_COL32 (58, 58, 70, 255));
        const ScopedFormStyle style (ctx);
        ImGui::SetCursorScreenPos (origin);
        if (ImGui::BeginChild ("##phase-body", size, ImGuiChildFlags_None,
                               ImGuiWindowFlags_NoBackground))
        {
            auto* outer = ctx.dl;
            ctx.dl = ImGui::GetWindowDrawList();
            viewport = { ctx.dl->GetClipRectMin(), ctx.dl->GetClipRectMax() };
            drawBody (ctx, ImGui::GetCursorScreenPos(), { ImGui::GetContentRegionAvail().x, size.y });
            ctx.dl = outer;
        }
        ImGui::EndChild();
        popupOpen = ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        keyboardActive = popupOpen || ImGui::IsAnyItemActive();
    }

private:
    void drawBody (dw::Context& ctx, ImVec2 origin, ImVec2 size)
    {
        const float inset = ctx.s (8.0f), gap = ctx.s (8.0f), row = ctx.s (24.0f);
        const float left = origin.x + inset, right = origin.x + size.x - inset;
        const float width = right - left;
        if (width < ctx.s (100.0f)) return;
        const auto& report = analyzer.report();
        float y = origin.y + inset;
        ctx.dl->AddRectFilled ({ left, y }, { right, y + ctx.s (22.0f) }, ctx.theme->pillFill, ctx.s (4.0f));
        ctx.dl->AddRect ({ left, y }, { right, y + ctx.s (22.0f) }, ctx.theme->pillBorder, ctx.s (4.0f));
        ctx.dl->AddCircleFilled ({ left + ctx.s (10.0f), y + ctx.s (11.0f) }, ctx.s (4.0f),
                                report.broadband.correlationValid ? kHealthy : ctx.theme->ledOff);
        dw::text (ctx, ctx.fonts->pill, ctx.s (10.5f), { left, y + ctx.s (5.0f) }, width, kBright, "PHASE");
        y += ctx.s (22.0f) + gap;
        controls[2] = { { left, y }, { right, y + row } };
        static constexpr const char* modes[] = { "Correlation", "Coherence / time" };
        formCombo (ctx, "##phase-mode", controls[2].first, controls[2].last, modes, 2, mode);
        y += row + gap;

        const float graphHeight = std::max (ctx.s (152.0f), size.y - (y - origin.y) - ctx.s (304.0f));
        drawBands (ctx, { left, y }, { right, y + graphHeight }, report);
        y += graphHeight + gap;
        drawBroadband (ctx, { left, y }, width, report.broadband);
        y += ctx.s (44.0f) + gap;

        controls[0] = { { left, y }, { left + ctx.s (62.0f), y + row } };
        if (formButton (ctx, "##phase-reset", controls[0].first, controls[0].last, "Reset")) analyzer.resetHeld();
        formTooltip ("Clear held correlation minima and sticky warnings.");
        bool mono = getMono();
        controls[1] = { { right - ctx.s (78.0f), y }, { right, y + row } };
        if (formCheckbox (ctx, "##phase-mono", controls[1].first, row, "Mono", mono)) setMono (mono);
        formTooltip ("Fold live monitoring to (L+R)/2. Analysis and export stay stereo.");
        y += row + gap;

        auto settings = params.snapshot();
        const float column = std::min (width / 3.0f, ctx.s (104.0f));
        const float knobLeft = left + (width - column * 3.0f) * 0.5f;
        float bands = (float) settings.bands;
        if (drawControl (ctx, 3, "Bands", "##phase-bands", { knobLeft, y }, column,
                         bands, dw::Range (4.0f, 64.0f), 16.0f, "%.0f", 1.0f))
            params.bands.store ((int) std::lround (bands), std::memory_order_relaxed);
        if (drawControl (ctx, 4, "Q", "##phase-q", { knobLeft + column, y }, column,
                         settings.q, dw::Range::withMidPoint (0.25f, 8.0f, 2.0f), 2.0f, "%.2f", 0.01f))
            params.q.store (settings.q, std::memory_order_relaxed);
        if (drawControl (ctx, 5, "Average", "##phase-average", { knobLeft + column * 2.0f, y }, column,
                         settings.averagingMs, dw::Range::withMidPoint (50.0f, 5000.0f, 500.0f), 500.0f, "%.0f ms", 10.0f))
            params.averagingMs.store (settings.averagingMs, std::memory_order_relaxed);
        formTooltip ("Correlation time constant. Coherence averages at least eight FFT windows and needs sustained audio to settle.");
        y += ctx.s (88.0f) + gap;

        const auto& band = report.bands[(size_t) selectedBand];
        ctx.dl->AddLine ({ left, y }, { right, y }, ctx.theme->pillDivider);
        y += ctx.s (8.0f);
        char range[64];
        std::snprintf (range, sizeof range, "%.0f to %.0f Hz", band.lowHz, band.highHz);
        dw::text (ctx, ctx.fonts->pill, ctx.s (11.0f), { left, y }, width, kBright, range, dw::Align::left);
        y += ctx.s (17.0f);
        ImGui::SetCursorScreenPos ({ left, y });
        if (mode == 0)
        {
            if (band.correlationValid) ImGui::Text ("Correlation %+.2f", band.correlation);
            else ImGui::TextUnformatted ("Correlation --");
            ImGui::SameLine();
            if (band.minimumValid) ImGui::TextDisabled (" / min %+.2f", band.minimumCorrelation);
            else ImGui::TextDisabled (" / min --");
        }
        else
        {
            if (band.coherenceValid) ImGui::Text ("Coherence %.2f", band.coherence);
            else ImGui::TextUnformatted ("Coherence --");
        }
        if (band.monoValid) ImGui::Text ("Mono loss %.1f dB", band.monoLossDb);
        else ImGui::TextUnformatted ("Mono loss --");
        y = ImGui::GetCursorScreenPos().y + ctx.s (4.0f);
        int warnings = 0;
        for (int i = 0; i < report.settings.bands; ++i)
            if (report.bands[(size_t) i].flagged) ++warnings;
        ImGui::SetCursorScreenPos ({ left, y });
        if (warnings > 0)
        {
            ImGui::PushStyleColor (ImGuiCol_Text, kWarning);
            ImGui::Text ("%d held phase warning%s", warnings, warnings == 1 ? "" : "s");
            ImGui::PopStyleColor();
        }
        else ImGui::TextDisabled ("No held phase warnings");
        ImGui::SetCursorScreenPos ({ left, ImGui::GetCursorScreenPos().y + ctx.s (4.0f) });
        if (ImGui::CollapsingHeader ("Details")) drawReadings (report);
        ImGui::Dummy ({ 0.0f, inset });
    }

    bool drawControl (dw::Context& ctx, size_t index, const char* label, const char* id, ImVec2 at,
                      float width, float& value, const dw::Range& range, float defaultValue,
                      const char* format, float speed)
    {
        dw::text (ctx, ctx.fonts->caption, ctx.s (10.5f), at, width, kMuted, label);
        const float radius = std::min (ctx.s (22.0f), width * 0.28f);
        dw::KnobStyle style;
        style.fill = kAccent;
        const auto knob = dw::knob (ctx, id, { at.x + width * 0.5f, at.y + ctx.s (16.0f) + radius },
                                    radius, value, range, defaultValue, style);
        if (knob.changed) value = index == 3 ? std::round (knob.value) : knob.value;
        formTooltip ("Drag or scroll to adjust; double-click to reset. Ctrl-click the value to type.");
        ImGui::SetCursorScreenPos ({ at.x + ctx.s (3.0f), at.y + ctx.s (68.0f) });
        ImGui::SetNextItemWidth (width - ctx.s (6.0f));
        ImGui::PushID (id);
        ImGui::PushStyleColor (ImGuiCol_FrameBg, IM_COL32 (24, 24, 32, 255));
        const bool edited = ImGui::DragFloat ("##value", &value, speed, range.start, range.end, format,
                                               ImGuiSliderFlags_AlwaysClamp);
        ImGui::PopStyleColor();
        ImGui::PopID();
        controls[index] = { ImGui::GetItemRectMin(), ImGui::GetItemRectMax() };
        if (edited && index == 3) value = std::round (value);
        return knob.changed || edited;
    }

    void drawBands (dw::Context& ctx, ImVec2 tl, ImVec2 br, const PhaseAnalyzer::Report& report)
    {
        const ImVec2 plot { tl.x + ctx.s (27.0f), tl.y + ctx.s (10.0f) };
        const ImVec2 end { br.x - ctx.s (8.0f), br.y - ctx.s (23.0f) };
        const float height = end.y - plot.y, width = end.x - plot.x;
        const int count = report.settings.bands;
        selectedBand = std::clamp (selectedBand, 0, count - 1);
        const float zero = mode == 0 ? (plot.y + end.y) * 0.5f : end.y;
        const float scale = mode == 0 ? height * 0.5f : height;
        ctx.dl->AddRectFilled (tl, br, IM_COL32 (15, 17, 23, 255), ctx.s (3.0f));
        ctx.dl->AddRect (tl, br, ctx.theme->pillDivider, ctx.s (3.0f));
        for (int i = 0; i <= 4; ++i)
        {
            const float y = plot.y + height * (float) i / 4.0f;
            ctx.dl->AddLine ({ plot.x, y }, { end.x, y }, IM_COL32 (43, 47, 54, 255));
            if (i % 2 == 0 || height >= ctx.s (220.0f))
            {
                char label[12];
                const float value = mode == 0 ? 1.0f - (float) i * 0.5f : 1.0f - (float) i * 0.25f;
                std::snprintf (label, sizeof label, mode == 0 ? "%+.1f" : "%.2g", (double) value);
                dw::text (ctx, ctx.fonts->caption, ctx.s (9.5f), { tl.x, y - ctx.s (5.0f) },
                          ctx.s (23.0f), i == 2 && mode == 0 ? kBright : kMuted, label, dw::Align::right);
            }
        }
        static constexpr float frequencies[] = { 20.0f, 100.0f, 1000.0f, 10000.0f, 20000.0f };
        static constexpr const char* labels[] = { "20", "100", "1k", "10k", "20k Hz" };
        for (size_t i = 0; i < std::size (frequencies); ++i)
        {
            if (i == 3 && width < ctx.s (300.0f)) continue;
            const float x = plot.x + std::log10 (frequencies[i] / 20.0f) / 3.0f * width;
            ctx.dl->AddLine ({ x, plot.y }, { x, end.y }, IM_COL32 (38, 42, 49, 255));
            const float labelWidth = ctx.s (i == 4 ? 44.0f : 30.0f);
            dw::text (ctx, ctx.fonts->caption, ctx.s (9.5f),
                      { std::clamp (x - labelWidth * 0.5f, plot.x - ctx.s (8.0f), br.x - labelWidth), end.y + ctx.s (6.0f) },
                      labelWidth, kMuted, labels[i]);
        }
        const float step = width / (float) count;
        bool validReading = false;
        for (int i = 0; i < count; ++i)
        {
            const auto& band = report.bands[(size_t) i];
            const float x = plot.x + (float) i * step;
            const float inset = std::min (ctx.s (1.5f), step * 0.15f);
            if (i == selectedBand)
                ctx.dl->AddRectFilled ({ x, plot.y }, { x + step, end.y }, IM_COL32 (90, 138, 208, 24));
            const bool valid = mode == 0 ? band.correlationValid : band.coherenceValid;
            if (valid)
            {
                validReading = true;
                const float value = std::clamp ((float) (mode == 0 ? band.correlation : band.coherence),
                                                mode == 0 ? -1.0f : 0.0f, 1.0f);
                const float at = zero - value * scale;
                const bool negative = mode == 0 && value < 0.0f;
                const ImU32 top = negative ? kWarning : kHealthy;
                const ImU32 bottom = negative ? IM_COL32 (161, 66, 59, 255) : IM_COL32 (192, 176, 77, 255);
                const float barTop = std::min (zero, at), barBottom = std::max (zero, at);
                ctx.dl->AddRectFilledMultiColor ({ x + inset, barTop }, { x + step - inset, barBottom },
                                                 top, top, bottom, bottom);
                for (float y = plot.y + ctx.s (7.0f); y < end.y; y += ctx.s (7.0f))
                    if (y > barTop && y < barBottom)
                        ctx.dl->AddLine ({ x + inset, y }, { x + step - inset, y },
                                         IM_COL32 (15, 17, 23, 150), ctx.s (1.0f));
            }
            if (mode == 0 && band.minimumValid)
            {
                const float at = zero - (float) band.minimumCorrelation * scale;
                ctx.dl->AddLine ({ x + inset, at }, { x + step - inset, at }, kBright, ctx.s (1.5f));
            }
            if (band.flagged)
                ctx.dl->AddRectFilled ({ x + inset, end.y + ctx.s (2.0f) },
                                       { x + step - inset, end.y + ctx.s (4.0f) }, kWarning);
        }
        ctx.dl->AddLine ({ plot.x, zero }, { end.x, zero }, IM_COL32 (160, 166, 176, 255), ctx.s (1.0f));
        if (! validReading)
            dw::text (ctx, ctx.fonts->caption, ctx.s (11.0f), { plot.x, plot.y + height * 0.25f }, width,
                      kMuted, report.warming ? "Warming up" : "No stereo reading");
        controls[6] = { plot, end };
        dw::hitArea (ctx, "##phase-band", plot, end);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        {
            const int hovered = std::clamp ((int) ((ImGui::GetIO().MousePos.x - plot.x) / step), 0, count - 1);
            if (ImGui::IsMouseClicked (ImGuiMouseButton_Left)) selectedBand = hovered;
            if (ImGui::IsItemHovered())
            {
                ImGui::BeginTooltip();
                ImGui::Text ("%.0f to %.0f Hz; click for readings", report.bands[(size_t) hovered].lowHz,
                             report.bands[(size_t) hovered].highHz);
                ImGui::EndTooltip();
            }
        }
    }

    void drawBroadband (dw::Context& ctx, ImVec2 at, float width, const PhaseAnalyzer::Reading& reading)
    {
        char correlation[24] = "--", mono[24] = "--";
        if (reading.correlationValid) std::snprintf (correlation, sizeof correlation, "%+.2f", reading.correlation);
        if (reading.monoValid) std::snprintf (mono, sizeof mono, "%.1f dB", reading.monoLossDb);
        const float half = width * 0.5f;
        dw::text (ctx, ctx.fonts->caption, ctx.s (10.0f), at, half, kMuted, "OVERALL", dw::Align::left);
        dw::text (ctx, ctx.fonts->caption, ctx.s (10.0f), { at.x + half, at.y }, half, kMuted, "MONO LOSS", dw::Align::right);
        dw::text (ctx, ctx.fonts->pill, ctx.s (17.0f), { at.x, at.y + ctx.s (13.0f) }, half,
                  reading.correlationValid && reading.correlation < 0.0 ? kWarning : kBright, correlation, dw::Align::left);
        dw::text (ctx, ctx.fonts->pill, ctx.s (17.0f), { at.x + half, at.y + ctx.s (13.0f) }, half, kBright, mono, dw::Align::right);
        const ImVec2 tl { at.x, at.y + ctx.s (35.0f) }, br { at.x + width, at.y + ctx.s (43.0f) };
        const float zero = at.x + width * 0.5f;
        ctx.dl->AddRectFilled (tl, br, IM_COL32 (15, 17, 23, 255), ctx.s (2.0f));
        if (reading.correlationValid)
        {
            const float end = zero + (float) reading.correlation * width * 0.5f;
            ctx.dl->AddRectFilled ({ std::min (zero, end), tl.y }, { std::max (zero, end), br.y },
                                   reading.correlation < 0.0 ? kWarning : kHealthy);
        }
        ctx.dl->AddLine ({ zero, tl.y }, { zero, br.y }, kMuted);
        if (reading.minimumValid)
        {
            const float held = zero + (float) reading.minimumCorrelation * width * 0.5f;
            ctx.dl->AddLine ({ held, tl.y }, { held, br.y }, kBright);
        }
    }

    void drawReadings (const PhaseAnalyzer::Report& report)
    {
        if (report.warming) ImGui::TextWrapped ("Warming up: play a mix to fill the analysis window.");
        const auto& band = report.bands[(size_t) selectedBand];
        ImGui::Text ("%.0f to %.0f Hz", band.lowHz, band.highHz);
        if (! band.available) ImGui::TextWrapped ("Unavailable above Nyquist or before audio preparation.");
        if (band.correlationValid) ImGui::Text ("Correlation %+.3f", band.correlation);
        else ImGui::TextUnformatted ("Correlation --");
        if (band.minimumValid) ImGui::Text ("Held minimum %+.3f", band.minimumCorrelation);
        if (band.monoValid) ImGui::Text ("Mono loss %.2f dB", band.monoLossDb);
        else ImGui::TextUnformatted ("Mono loss --");
        if (mode == 1)
        {
            if (band.coherenceValid) ImGui::Text ("Coherence %.3f", band.coherence);
            else ImGui::TextWrapped ("Coherence -- (needs sustained audio)");
            if (band.phaseValid)
            {
                ImGui::Text ("L/R phase %+.1f deg", band.phaseDegrees);
                ImGui::Text ("Wrapped offset %+.3f ms", band.equivalentDelayMs);
            }
            if (band.delayValid) ImGui::Text ("Fitted R lag %+.3f ms", band.delayMs);
            else ImGui::TextWrapped ("Fitted delay -- (needs sustained coherent bandwidth; a single tone is ambiguous)");
            ImGui::TextWrapped ("Wrapped offsets are ambiguous. Fitted delay range: +/-%.2f ms; positive means R lags.",
                                report.windowSeconds * 1000.0 / 6.0);
            ImGui::TextWrapped ("Low coherence can be normal stereo width.");
        }
        ImGui::TextWrapped ("FFT window %.0f ms; coherence averaging %.1f s. Narrow bands need longer to settle.",
                            report.windowSeconds * 1000.0, report.coherenceTimeSeconds);
        bool flagged = false;
        for (int i = 0; i < report.settings.bands; ++i)
            if (report.bands[(size_t) i].flagged)
            {
                flagged = true;
                ImGui::PushStyleColor (ImGuiCol_Text, kWarning);
                ImGui::TextWrapped ("Out of phase: %.0f to %.0f Hz", report.bands[(size_t) i].lowHz,
                                    report.bands[(size_t) i].highHz);
                ImGui::PopStyleColor();
            }
        if (! flagged) ImGui::TextUnformatted ("No held phase warnings.");
    }

    MasteringPhaseParams& params;
    PhaseAnalyzer& analyzer;
    std::function<bool()> getMono;
    std::function<void (bool)> setMono;
    std::array<Rect, 7> controls {};
    Rect viewport;
    int mode = 0, selectedBand = 0;
    bool popupOpen = false, popupWasOpen = false;
    bool keyboardActive = false, keyboardWasActive = false;
};
}

std::unique_ptr<DuskPanelView> makeMasteringPhaseView (
    MasteringPhaseParams& params, PhaseAnalyzer& analyzer,
    std::function<bool()> getMono, std::function<void (bool)> setMono)
{
    return std::make_unique<MasteringPhaseViewImpl> (params, analyzer, std::move (getMono), std::move (setMono));
}
}
