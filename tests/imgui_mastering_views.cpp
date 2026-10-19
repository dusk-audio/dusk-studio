#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BrickwallLimiter.h"
#include "dsp/PhaseAnalyzer.h"
#include "session/Session.h"
#include "ui/imgui/DuskTheme.h"
#include "ui/imgui/MasteringEqView.h"
#include "ui/imgui/MasteringLimiterView.h"
#include "ui/imgui/MasteringPhaseView.h"

#include <DuskWidgets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace dw = DuskWidgets;
using namespace duskstudio;

namespace
{
class HeadlessPanel
{
public:
    HeadlessPanel()
    {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext (context);

        auto& io = ImGui::GetIO();
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
        io.DisplaySize = ImVec2 (700.0f, 520.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();
        io.Fonts->Build();
        io.Fonts->SetTexID (static_cast<ImTextureID> (1));

        ImFont* const font = io.Fonts->Fonts.front();
        fonts.caption = fonts.label = fonts.pill = fonts.band = font;
        fonts.title = fonts.value = fonts.valueLarge = fonts.textEntry = font;
    }

    ~HeadlessPanel() { ImGui::DestroyContext (context); }

    void movePointer (ImVec2 to) { ImGui::GetIO().AddMousePosEvent (to.x, to.y); }
    void pressPointer (bool down)
    {
        ImGui::GetIO().AddMouseButtonEvent (ImGuiMouseButton_Left, down);
    }
    void setDeltaTime (float seconds) { ImGui::GetIO().DeltaTime = seconds; }
    void setSize (ImVec2 size) { ImGui::GetIO().DisplaySize = size; }
    void key (ImGuiKey key, bool down) { ImGui::GetIO().AddKeyEvent (key, down); }
    void type (const char* text) { ImGui::GetIO().AddInputCharactersUTF8 (text); }
    void scroll (float wheel) { ImGui::GetIO().AddMouseWheelEvent (0.0f, wheel); }

    void clickControl (imgui::DuskPanelView& view, const std::string& control, float position = 0.5f)
    {
        ImVec2 point;
        REQUIRE (view.controlPointForScenario (control, point, position));
        movePointer (point);
        frame (view);
        frame (view);
        pressPointer (true);
        frame (view);
        pressPointer (false);
        frame (view);
        frame (view);
    }

    const std::vector<ImVec2>& lastVertexPositions() const { return vertexPositions; }
    int renderedChildVertices() const { return childVertices; }

    // Draws one frame of `view` into the whole display and returns the vertex count.
    int frame (imgui::DuskPanelView& view)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImGui::GetIO().DisplaySize);
        ImGui::Begin ("##panel", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        dw::Context ctx;
        ctx.dl = ImGui::GetWindowDrawList();
        ctx.theme = &imgui::consolePalette().widgets;
        ctx.fonts = &fonts;
        ctx.drag = &drag;
        ctx.scale = 1.0f;

        view.draw (ctx, ImVec2 (0.0f, 0.0f), ImGui::GetIO().DisplaySize);
        const int vertices = ctx.dl->VtxBuffer.Size;
        vertexPositions.clear();
        vertexPositions.reserve (static_cast<std::size_t> (vertices));
        for (const auto& vertex : ctx.dl->VtxBuffer)
            vertexPositions.push_back (vertex.pos);

        ImGui::End();
        ImGui::Render();
        childVertices = 0;
        const auto* data = ImGui::GetDrawData();
        for (int i = 0; i < data->CmdListsCount; ++i)
            if (data->CmdLists[i] != ctx.dl)
                childVertices += data->CmdLists[i]->VtxBuffer.Size;
        return vertices;
    }

private:
    ImGuiContext* context = nullptr;
    dw::Fonts fonts;
    dw::DragState drag;
    std::vector<ImVec2> vertexPositions;
    int childVertices = 0;
};

// Both panels inset their body by 8 design pixels and open with the section header, so
// this lands inside it whatever the panel is sized to.
constexpr ImVec2 kHeaderPoint { 14.0f, 18.0f };

struct EqSnapshot
{
    std::array<float, MasteringParams::kNumEqBands> freq {}, gain {}, q {};

    explicit EqSnapshot (const MasteringParams& params)
    {
        for (int i = 0; i < MasteringParams::kNumEqBands; ++i)
        {
            freq[static_cast<std::size_t> (i)] = params.eqBandFreq[i].load();
            gain[static_cast<std::size_t> (i)] = params.eqBandGainDb[i].load();
            q[static_cast<std::size_t> (i)] = params.eqBandQ[i].load();
        }
    }

};

// Tolerance zero: the assertion is that no write happened at all.
void requireUnchanged (const EqSnapshot& after, const EqSnapshot& before)
{
    for (std::size_t i = 0; i < before.freq.size(); ++i)
    {
        REQUIRE_THAT (after.freq[i], Catch::Matchers::WithinAbs (before.freq[i], 0.0));
        REQUIRE_THAT (after.gain[i], Catch::Matchers::WithinAbs (before.gain[i], 0.0));
        REQUIRE_THAT (after.q[i], Catch::Matchers::WithinAbs (before.q[i], 0.0));
    }
}

void primeInvertedPhase (PhaseAnalyzer& analyzer, MasteringPhaseSettings settings)
{
    analyzer.prepare (48000.0, settings);
    std::vector<float> left ((size_t) analyzer.windowSize()), right (left.size());
    std::uint32_t state = 12345;
    for (size_t i = 0; i < left.size(); ++i)
    {
        state = state * 1664525u + 1013904223u;
        left[i] = (static_cast<float> (state >> 8) / 8388608.0f - 1.0f) * 0.1f;
        right[i] = -left[i];
    }
    for (int pass = 0; pass < 32; ++pass)
        analyzer.push (left.data(), right.data(), (int) left.size());
    REQUIRE (analyzer.report().broadband.flagged);
    REQUIRE (analyzer.report().broadband.coherenceValid);
}

void requireUnchanged (MasteringPhaseSettings after, MasteringPhaseSettings before)
{
    REQUIRE (after.bands == before.bands);
    REQUIRE_THAT (after.q, Catch::Matchers::WithinAbs (before.q, 0.0));
    REQUIRE_THAT (after.averagingMs, Catch::Matchers::WithinAbs (before.averagingMs, 0.0));
}

void requireUnchanged (const MasteringPhaseParams& after, MasteringPhaseSettings before)
{
    requireUnchanged (MasteringPhaseSettings { after.bands.load(), after.q.load(), after.averagingMs.load() }, before);
}
} // namespace

TEST_CASE ("the mastering EQ view draws without writing to its band parameters")
{
    Session session;
    auto& params = session.mastering();
    const EqSnapshot before (params);

    HeadlessPanel panel;
    // A null chain is the state before the engine has prepared: no scope to read, and
    // the response falls back to a nominal rate rather than dividing by zero.
    auto view = imgui::makeMasteringEqView (params, nullptr);

    panel.movePointer (ImVec2 (-100.0f, -100.0f));
    REQUIRE (panel.frame (*view) > 0);
    REQUIRE (panel.frame (*view) > 0);
    REQUIRE (panel.frame (*view) > 0);

    requireUnchanged (EqSnapshot (params), before);
}

TEST_CASE ("the mastering EQ header engages the section")
{
    Session session;
    auto& params = session.mastering();
    params.eqEnabled.store (false);

    HeadlessPanel panel;
    auto view = imgui::makeMasteringEqView (params, nullptr);

    // Two settling frames: Dear ImGui decides what the pointer is over from the window
    // it hovered on the previous frame.
    panel.movePointer (kHeaderPoint);
    panel.frame (*view);
    panel.frame (*view);
    panel.pressPointer (true);
    panel.frame (*view);
    panel.pressPointer (false);
    panel.frame (*view);

    REQUIRE (params.eqEnabled.load());
}

TEST_CASE ("the mastering EQ view takes no plate")
{
    Session session;
    auto view = imgui::makeMasteringEqView (session.mastering(), nullptr);
    // An inline stage panel paints its own surface across the whole child; a plate would
    // frame it like a modal and inset the body by the plate margin.
    REQUIRE_FALSE (view->wantsPlate());
}

TEST_CASE ("the mastering EQ curve follows a band moved in the view")
{
    // The view exposes no dot placement, so the dot is found by double-clicking down the
    // band's frequency column. These mirror MasteringEqView.cpp's layout constants
    // (kFreqMinHz/kFreqMaxHz, kOuterInset, kHeaderH, kHeaderGap, kCurveGap and the
    // controls-row share); if that layout changes, the gain-reset REQUIRE below fails
    // rather than the test passing on the wrong geometry.
    constexpr int band = 2;
    constexpr float freqMinHz = 20.0f;
    constexpr float freqMaxHz = 20000.0f;
    constexpr float outerInset = 8.0f;
    constexpr float headerHeight = 20.0f;
    constexpr float headerGap = 4.0f;
    constexpr float curveGap = 4.0f;
    constexpr float panelWidth = 700.0f;
    constexpr float panelHeight = 520.0f;

    Session session;
    auto& params = session.mastering();
    params.eqEnabled.store (true);
    params.eqBandGainDb[band].store (6.0f);

    HeadlessPanel panel;
    auto viewA = imgui::makeMasteringEqView (params, nullptr);
    const ImVec2 outside (-100.0f, -100.0f);
    panel.movePointer (outside);
    panel.frame (*viewA);
    panel.frame (*viewA);
    panel.frame (*viewA);

    const float plotLeft = outerInset + 1.0f;
    const float plotRight = panelWidth - outerInset - 1.0f;
    const float plotTop = outerInset + headerHeight + headerGap + 1.0f;
    const float remaining = (panelHeight - outerInset)
                          - (outerInset + headerHeight + headerGap);
    const float controlsHeight = std::clamp (remaining * 0.48f, 130.0f, 210.0f);
    const float plotBottom = plotTop + remaining - controlsHeight - curveGap - 2.0f;
    const float frequency = params.eqBandFreq[band].load();
    const float frequencyFraction = (std::log10 (frequency) - std::log10 (freqMinHz))
                                  / (std::log10 (freqMaxHz) - std::log10 (freqMinHz));
    const float dotX = plotLeft + frequencyFraction * (plotRight - plotLeft);

    for (float y = plotTop; y <= plotBottom && params.eqBandGainDb[band].load() != 0.0f;
         y += 4.0f)
    {
        panel.movePointer (ImVec2 (dotX, y));
        panel.frame (*viewA);
        panel.frame (*viewA);

        panel.pressPointer (true);
        panel.frame (*viewA);
        panel.pressPointer (false);
        panel.frame (*viewA);
        panel.pressPointer (true);
        panel.frame (*viewA);
        panel.pressPointer (false);
        panel.frame (*viewA);

        if (params.eqBandGainDb[band].load() != 0.0f)
        {
            panel.movePointer (outside);
            panel.setDeltaTime (1.0f);
            panel.frame (*viewA);
            panel.setDeltaTime (1.0f / 60.0f);
        }
    }
    REQUIRE_THAT (params.eqBandGainDb[band].load(), Catch::Matchers::WithinAbs (0.0f, 0.0));

    panel.movePointer (outside);
    panel.frame (*viewA);
    panel.frame (*viewA);
    const std::vector<ImVec2> viewAVertices = panel.lastVertexPositions();

    auto viewB = imgui::makeMasteringEqView (params, nullptr);
    panel.frame (*viewB);
    panel.frame (*viewB);
    const auto& viewBVertices = panel.lastVertexPositions();

    REQUIRE (viewAVertices.size() == viewBVertices.size());
    float maximumDelta = 0.0f;
    for (std::size_t i = 0; i < viewAVertices.size(); ++i)
    {
        maximumDelta = std::max (maximumDelta,
                                 std::abs (viewAVertices[i].x - viewBVertices[i].x));
        maximumDelta = std::max (maximumDelta,
                                 std::abs (viewAVertices[i].y - viewBVertices[i].y));
    }
    REQUIRE_THAT (maximumDelta, Catch::Matchers::WithinAbs (0.0f, 1.0e-5f));
}

TEST_CASE ("the mastering limiter view draws without writing to its parameters")
{
    Session session;
    auto& params = session.mastering();
    BrickwallLimiter limiter;
    const float drive = params.limiterDriveDb.load();
    const float ceiling = params.limiterCeilingDb.load();
    const int mode = params.limiterMode.load();

    HeadlessPanel panel;
    auto view = imgui::makeMasteringLimiterView (params, limiter);

    panel.movePointer (ImVec2 (-100.0f, -100.0f));
    REQUIRE (panel.frame (*view) > 0);
    REQUIRE (panel.frame (*view) > 0);
    REQUIRE (panel.frame (*view) > 0);

    REQUIRE_THAT (params.limiterDriveDb.load(), Catch::Matchers::WithinAbs (drive, 0.0));
    REQUIRE_THAT (params.limiterCeilingDb.load(), Catch::Matchers::WithinAbs (ceiling, 0.0));
    REQUIRE (params.limiterMode.load() == mode);
}

TEST_CASE ("the mastering limiter header engages the section")
{
    Session session;
    auto& params = session.mastering();
    params.limiterEnabled.store (true);
    BrickwallLimiter limiter;

    HeadlessPanel panel;
    auto view = imgui::makeMasteringLimiterView (params, limiter);

    panel.movePointer (kHeaderPoint);
    panel.frame (*view);
    panel.frame (*view);
    panel.pressPointer (true);
    panel.frame (*view);
    panel.pressPointer (false);
    panel.frame (*view);

    REQUIRE_FALSE (params.limiterEnabled.load());
}

TEST_CASE ("the mastering limiter view takes no plate")
{
    Session session;
    BrickwallLimiter limiter;
    auto view = imgui::makeMasteringLimiterView (session.mastering(), limiter);
    REQUIRE_FALSE (view->wantsPlate());
}

TEST_CASE ("the mastering Phase view preserves parameters and held analysis across recreation", "[mastering][phase][imgui]")
{
    MasteringPhaseParams params;
    params.bands.store (8);
    params.q.store (0.5f);
    params.averagingMs.store (100.0f);
    const auto before = params.snapshot();
    PhaseAnalyzer analyzer;
    primeInvertedPhase (analyzer, before);
    const auto frames = analyzer.report().frames;
    const auto minimum = analyzer.report().broadband.minimumCorrelation;
    int monoWrites = 0;
    HeadlessPanel panel;
    const auto makeView = [&]
    {
        return imgui::makeMasteringPhaseView (params, analyzer, [] { return false; },
                                              [&] (bool) { ++monoWrites; });
    };
    auto view = makeView();
    REQUIRE_FALSE (view->wantsPlate());
    panel.movePointer ({ -100.0f, -100.0f });
    for (int frame = 0; frame < 3; ++frame) panel.frame (*view);
    REQUIRE (panel.renderedChildVertices() > 100);
    view.reset();
    view = makeView();
    for (int frame = 0; frame < 3; ++frame) panel.frame (*view);
    REQUIRE (panel.renderedChildVertices() > 100);
    REQUIRE_FALSE (view->capturesKeyboard());
    requireUnchanged (params, before);
    REQUIRE (monoWrites == 0);
    REQUIRE (analyzer.report().frames == frames);
    REQUIRE (analyzer.report().broadband.flagged);
    REQUIRE_THAT (analyzer.report().broadband.minimumCorrelation, Catch::Matchers::WithinAbs (minimum, 0.0));
}

TEST_CASE ("the mastering Phase controls wire settings monitoring reset and keyboard input", "[mastering][phase][imgui]")
{
    MasteringPhaseParams params;
    params.bands.store (4);
    params.q.store (0.25f);
    params.averagingMs.store (50.0f);
    const auto before = params.snapshot();
    PhaseAnalyzer analyzer;
    primeInvertedPhase (analyzer, before);
    bool mono = false;
    int monoWrites = 0;
    HeadlessPanel panel;
    auto view = imgui::makeMasteringPhaseView (params, analyzer, [&] { return mono; },
                                              [&] (bool value) { mono = value; ++monoWrites; });
    panel.movePointer ({ -100.0f, -100.0f });
    panel.frame (*view);
    panel.frame (*view);

    SECTION ("Mono and Reset change only their owned state")
    {
        panel.clickControl (*view, "mono");
        REQUIRE (mono);
        REQUIRE (monoWrites == 1);
        const auto frames = analyzer.report().frames;
        const auto correlation = analyzer.report().broadband.correlation;
        panel.clickControl (*view, "reset");
        REQUIRE_FALSE (analyzer.report().broadband.minimumValid);
        REQUIRE_FALSE (analyzer.report().broadband.flagged);
        for (int i = 0; i < before.bands; ++i)
        {
            REQUIRE_FALSE (analyzer.report().bands[(size_t) i].minimumValid);
            REQUIRE_FALSE (analyzer.report().bands[(size_t) i].flagged);
        }
        REQUIRE (analyzer.report().frames == frames);
        REQUIRE_THAT (analyzer.report().broadband.correlation, Catch::Matchers::WithinAbs (correlation, 0.0));
        REQUIRE (mono);
        REQUIRE (monoWrites == 1);
        requireUnchanged (params, before);
        panel.clickControl (*view, "mono");
        REQUIRE_FALSE (mono);
        REQUIRE (monoWrites == 2);
    }
    SECTION ("Typed control values clamp to both bounds without reconfiguring the borrowed analyzer")
    {
        const auto enter = [&] (const char* name, const char* text)
        {
            const auto control = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
            panel.key (control, true);
            panel.frame (*view);
            panel.clickControl (*view, name);
            REQUIRE (view->capturesKeyboard());
            panel.key (control, false);
            panel.frame (*view);
            panel.type (text);
            panel.frame (*view);
            panel.key (ImGuiKey_Enter, true);
            panel.frame (*view);
            panel.key (ImGuiKey_Enter, false);
            panel.frame (*view);
            panel.frame (*view);
            REQUIRE_FALSE (view->capturesKeyboard());
        };
        enter ("bands", "128");
        enter ("q", "16");
        enter ("averaging", "10000");
        REQUIRE (params.bands.load() == 64);
        REQUIRE_THAT (params.q.load(), Catch::Matchers::WithinAbs (8.0f, 1.0e-6f));
        REQUIRE_THAT (params.averagingMs.load(), Catch::Matchers::WithinAbs (5000.0f, 1.0e-3f));
        for (const auto* control : { "bands", "q", "averaging" }) enter (control, "-100");
        requireUnchanged (params, before);
        requireUnchanged (analyzer.report().settings, before);
        REQUIRE (monoWrites == 0);
        REQUIRE (analyzer.report().broadband.flagged);
    }
    SECTION ("Q text entry keeps the keyboard and commits the typed value")
    {
        const auto control = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        panel.key (control, true);
        panel.frame (*view);
        panel.clickControl (*view, "q");
        REQUIRE (view->capturesKeyboard());
        panel.key (ImGuiKey_A, true);
        panel.frame (*view);
        panel.key (ImGuiKey_A, false);
        panel.frame (*view);
        panel.key (control, false);
        panel.frame (*view);
        panel.type ("3.5");
        panel.frame (*view);
        panel.key (ImGuiKey_Enter, true);
        panel.frame (*view);
        panel.key (ImGuiKey_Enter, false);
        panel.frame (*view);
        panel.frame (*view);
        REQUIRE_THAT (params.q.load(), Catch::Matchers::WithinAbs (3.5f, 1.0e-6f));
        REQUIRE (params.bands.load() == before.bands);
        REQUIRE_THAT (params.averagingMs.load(), Catch::Matchers::WithinAbs (before.averagingMs, 0.0));
        REQUIRE_FALSE (view->capturesKeyboard());
        REQUIRE (monoWrites == 0);
    }
    SECTION ("Mode popup takes Escape without changing saved settings")
    {
        panel.clickControl (*view, "mode");
        REQUIRE (view->capturesKeyboard());
        REQUIRE_FALSE (view->escapeDismisses());
        panel.key (ImGuiKey_Escape, true);
        panel.frame (*view);
        panel.key (ImGuiKey_Escape, false);
        panel.frame (*view);
        panel.frame (*view);
        REQUIRE_FALSE (view->capturesKeyboard());
        REQUIRE (view->escapeDismisses());
        requireUnchanged (params, before);
        REQUIRE (monoWrites == 0);
    }
}

TEST_CASE ("the compact mastering Phase view scrolls clipped controls without changing state", "[mastering][phase][imgui]")
{
    MasteringPhaseParams params;
    params.bands.store (4);
    params.q.store (0.25f);
    params.averagingMs.store (50.0f);
    const auto before = params.snapshot();
    PhaseAnalyzer analyzer;
    primeInvertedPhase (analyzer, before);
    const auto frames = analyzer.report().frames;
    int monoWrites = 0;
    HeadlessPanel panel;
    panel.setSize ({ 180.0f, 200.0f });
    auto view = imgui::makeMasteringPhaseView (params, analyzer, [] { return false; },
                                              [&] (bool) { ++monoWrites; });
    panel.movePointer ({ 90.0f, 110.0f });
    panel.frame (*view);
    panel.frame (*view);
    ImVec2 reset, mono, band;
    REQUIRE (view->controlPointForScenario ("band", band));
    REQUIRE_FALSE (view->controlPointForScenario ("reset", reset));
    REQUIRE_FALSE (view->controlPointForScenario ("mono", mono));
    for (int step = 0; step < 20 && (view->controlPointForScenario ("band", band)
                                   || ! view->controlPointForScenario ("reset", reset)); ++step)
    {
        panel.scroll (-1.0f);
        panel.frame (*view);
        panel.frame (*view);
    }
    REQUIRE (view->controlPointForScenario ("reset", reset));
    REQUIRE (view->controlPointForScenario ("mono", mono));
    REQUIRE (std::abs (reset.x - mono.x) > 20.0f);
    REQUIRE_FALSE (view->controlPointForScenario ("band", band));
    REQUIRE (panel.renderedChildVertices() > 100);
    for (int step = 0; step < 20 && (view->controlPointForScenario ("reset", reset)
                                   || ! view->controlPointForScenario ("band", band)); ++step)
    {
        panel.scroll (1.0f);
        panel.frame (*view);
        panel.frame (*view);
    }
    REQUIRE (view->controlPointForScenario ("band", band));
    REQUIRE_FALSE (view->controlPointForScenario ("reset", reset));
    REQUIRE_FALSE (view->controlPointForScenario ("mono", mono));
    requireUnchanged (params, before);
    REQUIRE (monoWrites == 0);
    REQUIRE (analyzer.report().frames == frames);
    REQUIRE (analyzer.report().broadband.flagged);
}
