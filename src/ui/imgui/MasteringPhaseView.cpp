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
constexpr ImU32 kHealthy = IM_COL32 (90, 200, 180, 255);
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
                               ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_AlwaysVerticalScrollbar))
        {
            auto* outer = ctx.dl;
            ctx.dl = ImGui::GetWindowDrawList();
            viewport = { ctx.dl->GetClipRectMin(), ctx.dl->GetClipRectMax() };
            drawBody (ctx, ImGui::GetCursorScreenPos(), ImGui::GetContentRegionAvail().x);
            ctx.dl = outer;
        }
        ImGui::EndChild();
        popupOpen = ImGui::IsPopupOpen (nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel);
        keyboardActive = popupOpen || ImGui::IsAnyItemActive();
    }

private:
    void drawBody (dw::Context& ctx, ImVec2 origin, float width)
    {
        const float left = origin.x, right = origin.x + width;
        const float row = ctx.s (24.0f), gap = ctx.s (4.0f);
        float y = origin.y;
        dw::text (ctx, ctx.fonts->pill, ctx.s (12.0f), { left, y + ctx.s (4.0f) }, ctx.s (60.0f),
                  kHealthy, "PHASE", dw::Align::left);
        if (width < ctx.s (202.0f)) y += row + gap;
        controls[0] = { { right - ctx.s (142.0f), y }, { right - ctx.s (86.0f), y + row } };
        if (formButton (ctx, "##phase-reset", controls[0].first, controls[0].last, "Reset"))
            analyzer.resetHeld();
        formTooltip ("Clear held correlation minima and sticky warnings.");
        bool mono = getMono();
        controls[1] = { { right - ctx.s (78.0f), y }, { right, y + row } };
        if (formCheckbox (ctx, "##phase-mono", controls[1].first, row, "Mono", mono)) setMono (mono);
        formTooltip ("Fold live monitoring to (L+R)/2. Analysis and export stay stereo.");
        y += row + gap;
        controls[2] = { { left, y }, { right, y + row } };
        static constexpr const char* modes[] = { "Correlation", "Coherence / time" };
        formCombo (ctx, "##phase-mode", controls[2].first, controls[2].last, modes, 2, mode);
        y += row + gap;
        auto settings = params.snapshot();
        const auto slider = [&] (size_t index, const char* label, const char* id, float& value,
                                  float low, float high, const char* format)
        {
            formLabel (ctx, { left, y }, ctx.s (54.0f), row, label, dw::Align::left);
            controls[index] = { { left + ctx.s (58.0f), y }, { right, y + row } };
            const auto result = formSlider (ctx, id, controls[index].first, controls[index].last,
                                            value, low, high, format);
            y += row + gap;
            return result.changed;
        };
        float bands = (float) settings.bands;
        if (slider (3, "Bands", "##phase-bands", bands, 4.0f, 64.0f, "%.0f"))
            params.bands.store ((int) std::lround (bands), std::memory_order_relaxed);
        if (slider (4, "Q", "##phase-q", settings.q, 0.25f, 8.0f, "%.2f"))
            params.q.store (settings.q, std::memory_order_relaxed);
        if (slider (5, "Average", "##phase-average", settings.averagingMs, 50.0f, 5000.0f, "%.0f ms"))
            params.averagingMs.store (settings.averagingMs, std::memory_order_relaxed);
        formTooltip ("Correlation time constant. Coherence averages at least eight FFT windows and needs sustained audio to settle.");

        const auto& report = analyzer.report();
        const float graphHeight = ctx.s (100.0f);
        drawBands (ctx, { left + ctx.s (16.0f), y }, { right, y + graphHeight }, report);
        y += graphHeight + gap;
        drawBroadband (ctx, { left, y }, width, report.broadband);
        y += ctx.s (26.0f);
        ImGui::SetCursorScreenPos ({ left, y });
        drawReadings (report);
    }

    void drawBands (dw::Context& ctx, ImVec2 tl, ImVec2 br, const PhaseAnalyzer::Report& report)
    {
        const int count = report.settings.bands;
        selectedBand = std::clamp (selectedBand, 0, count - 1);
        const float zero = mode == 0 ? (tl.y + br.y) * 0.5f : br.y;
        const float scale = mode == 0 ? (br.y - tl.y) * 0.5f : br.y - tl.y;
        ctx.dl->AddRectFilled (tl, br, IM_COL32 (15, 17, 23, 255));
        ctx.dl->AddLine ({ tl.x, zero }, { br.x, zero }, kMuted);
        const float step = (br.x - tl.x) / (float) count;
        for (int i = 0; i < count; ++i)
        {
            const auto& band = report.bands[(size_t) i];
            const float x = tl.x + i * step;
            const bool valid = mode == 0 ? band.correlationValid : band.coherenceValid;
            if (valid)
            {
                const float value = (float) (mode == 0 ? band.correlation : band.coherence);
                const float at = zero - value * scale;
                ctx.dl->AddRectFilled ({ x + step * 0.1f, std::min (zero, at) },
                                       { x + step * 0.9f, std::max (zero, at) + ctx.s (0.5f) },
                                       band.correlationValid && band.correlation < 0.0 ? kWarning : kHealthy);
            }
            if (mode == 0 && band.minimumValid)
            {
                const float at = zero - (float) band.minimumCorrelation * scale;
                ctx.dl->AddLine ({ x, at }, { x + step, at }, kMuted);
            }
            if (band.flagged)
                ctx.dl->AddRectFilled ({ x, br.y - ctx.s (3.0f) }, { x + step, br.y }, kWarning);
            if (i == selectedBand)
                ctx.dl->AddRect ({ x, tl.y }, { x + step, br.y }, IM_COL32 (210, 220, 230, 255));
        }
        dw::text (ctx, ctx.fonts->caption, ctx.s (9.0f), { tl.x - ctx.s (16.0f), tl.y },
                  ctx.s (15.0f), kMuted, "+1");
        dw::text (ctx, ctx.fonts->caption, ctx.s (9.0f), { tl.x - ctx.s (16.0f), br.y - ctx.s (10.0f) },
                  ctx.s (15.0f), kMuted, mode == 0 ? "-1" : "0");
        controls[6] = { tl, br };
        dw::hitArea (ctx, "##phase-band", tl, br);
        if (ImGui::IsItemHovered() || ImGui::IsItemActive())
        {
            const int hovered = std::clamp ((int) ((ImGui::GetIO().MousePos.x - tl.x) / step), 0, count - 1);
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
        char line[96];
        std::snprintf (line, sizeof line, "Overall %s   Mono %s", correlation, mono);
        dw::text (ctx, ctx.fonts->caption, ctx.s (10.5f), at, width, kMuted, line, dw::Align::left);
        const ImVec2 tl { at.x, at.y + ctx.s (14.0f) }, br { at.x + width, at.y + ctx.s (21.0f) };
        const float zero = at.x + width * 0.5f;
        ctx.dl->AddRectFilled (tl, br, IM_COL32 (15, 17, 23, 255));
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
            ctx.dl->AddLine ({ held, tl.y }, { held, br.y }, kMuted);
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
