#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BrickwallLimiter.h"
#include "dsp/MasteringChain.h"
#include "dsp/MultiCompStage.h"
#include "dsp/MultibandCompPresets.h"
#include "dsp/PhaseAnalyzer.h"
#include "foundation/Decibels.h"
#include "session/Session.h"
#include "ui/imgui/DuskTheme.h"
#include "ui/imgui/MasteringCompView.h"
#include "ui/imgui/MasteringEqView.h"
#include "ui/imgui/MasteringLimiterView.h"
#include "ui/imgui/MasteringPhaseView.h"
#include "ui/imgui/ScenarioControlMap.h"

#include <DuskWidgets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <optional>
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
    const std::vector<ImU32>& lastVertexColours() const { return vertexColours; }
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
        vertexColours.clear();
        vertexColours.reserve (static_cast<std::size_t> (vertices));
        for (const auto& vertex : ctx.dl->VtxBuffer)
        {
            vertexPositions.push_back (vertex.pos);
            vertexColours.push_back (vertex.col);
        }

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
    std::vector<ImU32> vertexColours;
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

TEST_CASE ("a scenario press at either end of a limiter column takes that column", "[mastering][imgui]")
{
    Session session;
    auto& params = session.mastering();
    BrickwallLimiter limiter;
    HeadlessPanel panel;
    panel.movePointer ({ -100.0f, -100.0f });
    auto view = imgui::makeMasteringLimiterView (params, limiter);
    panel.frame (*view);
    panel.frame (*view);

    // Threshold is minus the drive: its column runs from -20 dB at the foot to 0 at the top.
    params.limiterDriveDb.store (10.0f);
    panel.clickControl (*view, "limiter_drive_db", 1.0f);
    REQUIRE_THAT (params.limiterDriveDb.load(), Catch::Matchers::WithinAbs (0.0, 1.0e-4));
    panel.clickControl (*view, "limiter_drive_db", 0.0f);
    REQUIRE_THAT (params.limiterDriveDb.load(), Catch::Matchers::WithinAbs (20.0, 1.0e-4));

    panel.clickControl (*view, "limiter_ceiling_db", 1.0f);
    REQUIRE_THAT (params.limiterCeilingDb.load(), Catch::Matchers::WithinAbs (0.0, 1.0e-4));
    panel.clickControl (*view, "limiter_ceiling_db", 0.0f);
    REQUIRE_THAT (params.limiterCeilingDb.load(), Catch::Matchers::WithinAbs (-12.0, 1.0e-4));
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
    SECTION ("The header's light switches the module, letting go of Mono and holding it off")
    {
        panel.clickControl (*view, "mono");
        REQUIRE (mono);
        panel.clickControl (*view, "header");
        REQUIRE_FALSE (params.enabled.load());
        REQUIRE_FALSE (mono);
        const int writes = monoWrites;
        panel.clickControl (*view, "mono");
        REQUIRE_FALSE (mono);
        REQUIRE (monoWrites == writes);
        REQUIRE (analyzer.report().broadband.flagged);
        panel.clickControl (*view, "header");
        REQUIRE (params.enabled.load());
        REQUIRE_FALSE (mono);
        requireUnchanged (params, before);
    }
    SECTION ("Wheeling visible knobs changes their value without scrolling the short panel")
    {
        panel.setSize ({ 260.0f, 260.0f });
        panel.movePointer ({ 20.0f, 100.0f });
        panel.frame (*view);
        panel.frame (*view);
        ImVec2 point;
        for (int step = 0; step < 20; ++step)
        {
            if (view->controlPointForScenario ("q-knob", point) && point.y > 30.0f && point.y < 230.0f) break;
            panel.scroll (-1.0f);
            panel.frame (*view);
            panel.frame (*view);
        }
        REQUIRE (view->controlPointForScenario ("q-knob", point));
        for (const auto* name : { "bands-knob", "q-knob", "averaging-knob" })
        {
            REQUIRE (view->controlPointForScenario (name, point));
            const auto previous = params.snapshot();
            panel.movePointer (point);
            panel.frame (*view);
            panel.frame (*view);
            panel.scroll (1.0f);
            panel.frame (*view);
            panel.frame (*view);
            ImVec2 after;
            REQUIRE (view->controlPointForScenario (name, after));
            REQUIRE_THAT (after.y, Catch::Matchers::WithinAbs (point.y, 0.01f));
            if (std::string (name) == "bands-knob") REQUIRE (params.bands.load() > previous.bands);
            else if (std::string (name) == "q-knob") REQUIRE (params.q.load() > previous.q);
            else REQUIRE (params.averagingMs.load() > previous.averagingMs);
        }
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
    SECTION ("A value being typed is dropped when another session opens")
    {
        const auto control = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        panel.key (control, true);
        panel.frame (*view);
        panel.clickControl (*view, "bands");
        REQUIRE (view->capturesKeyboard());
        panel.key (control, false);
        panel.frame (*view);
        panel.type ("3");
        panel.frame (*view);
        view->sessionReplaced();
        params.bands.store (16);
        params.q.store (2.0f);
        params.averagingMs.store (500.0f);
        const auto loaded = params.snapshot();
        panel.type ("2");
        panel.frame (*view);
        panel.key (ImGuiKey_Enter, true);
        panel.frame (*view);
        panel.key (ImGuiKey_Enter, false);
        panel.frame (*view);
        panel.frame (*view);
        requireUnchanged (params, loaded);
        REQUIRE_FALSE (view->capturesKeyboard());
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

TEST_CASE ("a mastering view's live stamp holds while what it shows holds, and moves when that changes",
           "[mastering][imgui]")
{
    Session session;
    auto& params = session.mastering();
    HeadlessPanel panel;
    panel.movePointer ({ -100.0f, -100.0f });

    SECTION ("the EQ, whose trace rests on a scope that stopped and falls again on new silence")
    {
        params.compEnabled.store (false);
        params.limiterEnabled.store (false);
        MasteringChain chain;
        chain.prepare (48000.0, 512);
        chain.bind (params);
        std::vector<float> left (512), right (512);
        int played = 0;
        const auto play = [&] (float level, int blocks)
        {
            for (int block = 0; block < blocks; ++block, played += 512)
            {
                for (int i = 0; i < 512; ++i)
                    left[(size_t) i] = right[(size_t) i] = level * std::sin (0.05f * static_cast<float> (played + i));
                chain.processInPlace (left.data(), right.data(), 512);
            }
        };
        auto view = imgui::makeMasteringEqView (params, &chain);
        const auto rest = [&]
        {
            for (int frame = 0; frame < 600 && ! view->liveStamp().has_value(); ++frame)
                panel.frame (*view);
            return view->liveStamp();
        };

        play (0.0f, 8);
        const auto idle = rest();
        REQUIRE (idle.has_value());
        panel.frame (*view);
        REQUIRE (view->liveStamp() == idle);

        play (0.5f, 8);
        REQUIRE_FALSE (view->liveStamp().has_value());
        REQUIRE (rest() == idle);

        play (0.0f, 8);
        REQUIRE_FALSE (view->liveStamp().has_value());
        REQUIRE (rest() == idle);

        params.eqBandGainDb[2].store (params.eqBandGainDb[2].load() + 3.0f);
        REQUIRE (view->liveStamp() != idle);

        // The audio callback writes the scope every block, stopped or not.
        std::uint32_t state = 1;
        const auto hiss = [&] (float rms)
        {
            for (int i = 0; i < 512; ++i)
            {
                state = state * 1664525u + 1013904223u;
                left[(size_t) i] = right[(size_t) i]
                    = rms * 1.7320508f * (static_cast<float> (state >> 8) / 8388608.0f - 1.0f);
            }
            chain.processInPlace (left.data(), right.data(), 512);
        };
        const float underFloor = dusk::audio::decibelsToGain (-95.0f);
        for (int block = 0; block < 8; ++block) hiss (underFloor);
        rest();
        int resting = 0;
        for (int block = 0; block < 60; ++block)
        {
            hiss (underFloor);
            resting += view->liveStamp().has_value() ? 1 : 0;
            panel.frame (*view);
        }
        REQUIRE (resting == 60);

        hiss (dusk::audio::decibelsToGain (-40.0f));
        REQUIRE_FALSE (view->liveStamp().has_value());
    }

    SECTION ("the limiter, which asks for every frame while its meters ease")
    {
        BrickwallLimiter limiter;
        auto view = imgui::makeMasteringLimiterView (params, limiter);
        const auto settle = [&]
        {
            for (int frame = 0; frame < 2000 && ! view->liveStamp().has_value(); ++frame)
                panel.frame (*view);
            return view->liveStamp();
        };
        const auto idle = settle();
        REQUIRE (idle.has_value());
        panel.frame (*view);
        REQUIRE (view->liveStamp() == idle);

        params.meterPostMasterLDb.store (-6.0f);
        REQUIRE_FALSE (view->liveStamp().has_value());
        for (int frame = 0; frame < 4; ++frame) panel.frame (*view);
        params.meterPostMasterLDb.store (-100.0f);
        REQUIRE_FALSE (view->liveStamp().has_value());
        REQUIRE (settle() == idle);

        params.limiterCeilingDb.store (params.limiterCeilingDb.load() - 0.5f);
        REQUIRE (view->liveStamp() != idle);
    }

    SECTION ("the limiter, which rests on a hiss under its meters' scales and soon after Stop")
    {
        BrickwallLimiter limiter;
        auto view = imgui::makeMasteringLimiterView (params, limiter);
        std::uint32_t state = 1;
        const auto hissDb = [&state]
        {
            state = state * 1664525u + 1013904223u;
            return -60.0f + static_cast<float> (state >> 8) / 16777216.0f * 3.0f;
        };
        for (int frame = 0; frame < 120; ++frame)
        {
            params.meterPostMasterLDb.store (hissDb());
            panel.frame (*view);
        }
        int resting = 0;
        for (int block = 0; block < 60; ++block)
        {
            params.meterPostMasterLDb.store (hissDb());
            resting += view->liveStamp().has_value() ? 1 : 0;
            panel.frame (*view);
        }
        REQUIRE (resting == 60);

        params.meterPostMasterLDb.store (-1.0f);
        for (int frame = 0; frame < 30; ++frame) panel.frame (*view);
        params.meterPostMasterLDb.store (-100.0f);
        int frames = 0;
        for (; frames < 600 && ! view->liveStamp().has_value(); ++frames)
            panel.frame (*view);
        // Half a second at the panel's 60 frames a second.
        REQUIRE (frames <= 30);
    }

    SECTION ("the phase meter")
    {
        PhaseAnalyzer analyzer;
        analyzer.prepare (48000.0, params.phase.snapshot());
        auto view = imgui::makeMasteringPhaseView (params.phase, analyzer, [] { return false; }, [] (bool) {});
        for (int frame = 0; frame < 3; ++frame) panel.frame (*view);
        const auto idle = view->liveStamp();
        REQUIRE (idle.has_value());
        panel.frame (*view);
        REQUIRE (view->liveStamp() == idle);
        primeInvertedPhase (analyzer, params.phase.snapshot());
        const auto analysed = view->liveStamp();
        REQUIRE (analysed != idle);
        params.phase.bands.store (params.phase.bands.load() == 16 ? 24 : 16);
        REQUIRE (view->liveStamp() != analysed);
    }

    SECTION ("the phase meter settling on silence after a mix")
    {
        params.phase.averagingMs.store (50.0f);
        PhaseAnalyzer analyzer;
        primeInvertedPhase (analyzer, params.phase.snapshot());
        auto view = imgui::makeMasteringPhaseView (params.phase, analyzer, [] { return false; }, [] (bool) {});
        std::vector<float> silence ((size_t) analyzer.windowSize(), 0.0f);
        std::optional<std::uint64_t> previous;
        int unchanged = 0;
        for (int chunk = 0; chunk < 400 && unchanged < 16; ++chunk)
        {
            analyzer.push (silence.data(), silence.data(), (int) silence.size());
            const auto stamp = view->liveStamp();
            unchanged = stamp.has_value() && stamp == previous ? unchanged + 1 : 0;
            previous = stamp;
        }
        REQUIRE (previous.has_value());
        REQUIRE (unchanged >= 16);
    }
}

namespace
{
// A press at one point of a control, moves to another in steps and a release, as a
// pointer drags; `then` names the control the drag ends over, when another.
void dragControl (HeadlessPanel& panel, imgui::DuskPanelView& view, const std::string& control, float from,
                  float to, const std::string& then = {})
{
    ImVec2 start, end;
    REQUIRE (view.controlPointForScenario (control, start, from));
    REQUIRE (view.controlPointForScenario (then.empty() ? control : then, end, to));
    panel.movePointer (start);
    panel.frame (view);
    panel.frame (view);
    panel.pressPointer (true);
    panel.frame (view);
    for (int step = 1; step <= 4; ++step)
    {
        const float t = (float) step / 4.0f;
        panel.movePointer ({ start.x + (end.x - start.x) * t, start.y + (end.y - start.y) * t });
        panel.frame (view);
    }
    panel.pressPointer (false);
    panel.frame (view);
    panel.frame (view);
}

CompParams::Values multibandValues (const CompParams& comp)
{
    return comp.values();
}

// Every compressor value is what `before` held.
void requireUnchanged (const CompParams& comp, const CompParams::Values& before)
{
    for (std::size_t i = 0; i < kCompParamCount; ++i)
    {
        CAPTURE (compParamSpec ((CompParamId) i).symbol);
        REQUIRE_THAT (comp.get ((CompParamId) i), Catch::Matchers::WithinAbs (before[i], 0.0));
    }
}

// Every multiband value but `moved` is what `before` held, and `moved` is not.
void requireOnly (const CompParams& comp, const CompParams::Values& before, CompParamId moved)
{
    for (const auto id : compStageParams (CompStage::Multiband))
    {
        CAPTURE (compParamSpec (id).symbol);
        if (id == moved)
            REQUIRE (std::abs (comp.get (id) - before[(std::size_t) id]) > 1.0e-5f);
        else
            REQUIRE_THAT (comp.get (id), Catch::Matchers::WithinAbs (before[(std::size_t) id], 0.0));
    }
}
} // namespace

TEST_CASE ("the mastering multiband view shows every multiband control at once, at every size it is given",
           "[mastering][multiband][imgui]")
{
    Session session;
    auto& params = session.mastering();
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
    REQUIRE_FALSE (view->wantsPlate());

    for (const auto size : { ImVec2 (400.0f, 410.0f), ImVec2 (400.0f, 464.0f), ImVec2 (483.0f, 760.0f),
                             ImVec2 (632.0f, 800.0f), ImVec2 (1376.0f, 400.0f) })
    {
        CAPTURE (size.x, size.y);
        panel.setSize (size);
        panel.frame (*view);
        panel.frame (*view);
        for (const auto id : compStageParams (CompStage::Multiband))
        {
            CAPTURE (compParamSpec (id).symbol);
            ImVec2 point;
            REQUIRE (view->controlPointForScenario (compParamSpec (id).symbol, point));
            REQUIRE (point.x > 0.0f);
            REQUIRE (point.x < size.x);
            REQUIRE (point.y > 0.0f);
            REQUIRE (point.y < size.y);
        }
    }
}

TEST_CASE ("the mastering multiband view draws without writing to its parameters", "[mastering][multiband][imgui]")
{
    Session session;
    auto& params = session.mastering();
    const auto before = multibandValues (params.comp);
    const bool engaged = params.compEnabled.load();
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
    for (int frame = 0; frame < 4; ++frame)
        REQUIRE (panel.frame (*view) > 0);
    requireUnchanged (params.comp, before);
    REQUIRE (params.compEnabled.load() == engaged);
}

TEST_CASE ("the mastering multiband controls each move their own value and leave the compressor as it is",
           "[mastering][multiband][imgui]")
{
    Session session;
    auto& params = session.mastering();
    params.compEnabled.store (false);
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
    panel.frame (*view);
    panel.frame (*view);

    SECTION ("a band dial dragged up")
    {
        const auto before = multibandValues (params.comp);
        const auto id = compBandParam (1, CompBandField::Threshold);
        dragControl (panel, *view, "mb_threshold_1", 0.5f, 1.0f);
        requireOnly (params.comp, before, id);
        REQUIRE (params.comp.get (id) > before[(std::size_t) id]);
    }
    SECTION ("a stage-wide dial dragged down")
    {
        const auto before = multibandValues (params.comp);
        dragControl (panel, *view, "mb_output", 0.5f, 0.0f);
        requireOnly (params.comp, before, CompParamId::MbOutput);
        REQUIRE (params.comp.get (CompParamId::MbOutput) < before[(std::size_t) CompParamId::MbOutput]);
    }
    SECTION ("a band switch and auto makeup clicked")
    {
        auto before = multibandValues (params.comp);
        const auto bypass = compBandParam (2, CompBandField::Bypass);
        panel.clickControl (*view, "mb_bypass_2");
        requireOnly (params.comp, before, bypass);
        REQUIRE (params.comp.get (bypass) > 0.5f);
        before = multibandValues (params.comp);
        panel.clickControl (*view, "auto_makeup");
        requireOnly (params.comp, before, CompParamId::AutoMakeup);
    }
    SECTION ("a crossover dragged across the display")
    {
        const auto before = multibandValues (params.comp);
        dragControl (panel, *view, "mb_crossover_2", 0.5f, 0.75f, "plot");
        requireOnly (params.comp, before, CompParamId::MbCrossover2);
        REQUIRE (params.comp.get (CompParamId::MbCrossover2) > before[(std::size_t) CompParamId::MbCrossover2]);
    }
    SECTION ("a double-click puts a dial back to the plug-in's default")
    {
        const auto id = compBandParam (3, CompBandField::Makeup);
        params.comp.set (id, 6.0f);
        const auto before = multibandValues (params.comp);
        panel.frame (*view);
        ImVec2 point;
        REQUIRE (view->controlPointForScenario ("mb_makeup_3", point));
        panel.movePointer (point);
        panel.frame (*view);
        for (const bool down : { true, false, true })
        {
            panel.pressPointer (down);
            panel.frame (*view);
        }
        // The second press is held for a while, as a hand holds it.
        for (int frame = 0; frame < 6; ++frame)
            panel.frame (*view);
        panel.pressPointer (false);
        panel.frame (*view);
        REQUIRE_THAT (params.comp.get (id), Catch::Matchers::WithinAbs (compParamSpec (id).hostDefault, 0.0));
        requireOnly (params.comp, before, id);
    }
    SECTION ("a double-click puts a crossover back to the plug-in's default")
    {
        const auto id = CompParamId::MbCrossover2;
        float moved = 0.0f;
        REQUIRE (stage.valueFromText (id, "3000", moved));
        params.comp.set (id, moved);
        REQUIRE (std::abs (moved - compParamSpec (id).hostDefault) > 1.0e-3f);
        const auto before = multibandValues (params.comp);
        panel.frame (*view);
        panel.frame (*view);
        ImVec2 point;
        REQUIRE (view->controlPointForScenario ("mb_crossover_2", point));
        panel.movePointer (point);
        panel.frame (*view);
        for (const bool down : { true, false, true })
        {
            panel.pressPointer (down);
            panel.frame (*view);
        }
        for (int frame = 0; frame < 6; ++frame)
            panel.frame (*view);
        panel.pressPointer (false);
        panel.frame (*view);
        REQUIRE_THAT (params.comp.get (id), Catch::Matchers::WithinAbs (compParamSpec (id).hostDefault, 0.0));
        requireOnly (params.comp, before, id);
    }
    SECTION ("the wheel over a dial")
    {
        const auto id = compBandParam (1, CompBandField::Threshold);
        const auto before = multibandValues (params.comp);
        ImVec2 point;
        REQUIRE (view->controlPointForScenario ("mb_threshold_1", point));
        panel.movePointer (point);
        panel.frame (*view);
        panel.frame (*view);
        panel.scroll (1.0f);
        panel.frame (*view);
        panel.frame (*view);
        requireOnly (params.comp, before, id);
        REQUIRE (params.comp.get (id) > before[(std::size_t) id]);
    }
    SECTION ("a value typed after a command-click")
    {
        const auto id = compBandParam (0, CompBandField::Makeup);
        const auto before = multibandValues (params.comp);
        float expected = 0.0f;
        REQUIRE (stage.valueFromText (id, "3", expected));
        ImVec2 point;
        REQUIRE (view->controlPointForScenario ("mb_makeup_0", point));
        panel.movePointer (point);
        panel.frame (*view);
        const auto control = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        panel.key (control, true);
        panel.frame (*view);
        panel.pressPointer (true);
        panel.frame (*view);
        panel.pressPointer (false);
        panel.key (control, false);
        panel.frame (*view);
        panel.frame (*view);
        REQUIRE (view->capturesKeyboard());
        panel.type ("3");
        panel.frame (*view);
        panel.key (ImGuiKey_Enter, true);
        panel.frame (*view);
        panel.key (ImGuiKey_Enter, false);
        panel.frame (*view);
        panel.frame (*view);
        REQUIRE_THAT (params.comp.get (id), Catch::Matchers::WithinAbs (expected, 0.0));
        requireOnly (params.comp, before, id);
        REQUIRE_FALSE (view->capturesKeyboard());
    }
    SECTION ("a value being typed is dropped when the panel shrinks past its controls")
    {
        const auto before = multibandValues (params.comp);
        ImVec2 point;
        REQUIRE (view->controlPointForScenario ("mb_makeup_0", point));
        panel.movePointer (point);
        panel.frame (*view);
        const auto control = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        panel.key (control, true);
        panel.frame (*view);
        panel.pressPointer (true);
        panel.frame (*view);
        panel.pressPointer (false);
        panel.key (control, false);
        panel.frame (*view);
        panel.frame (*view);
        REQUIRE (view->capturesKeyboard());
        panel.setSize ({ 483.0f, 120.0f });
        panel.frame (*view);
        panel.frame (*view);
        REQUIRE_FALSE (view->capturesKeyboard());
        panel.setSize ({ 483.0f, 600.0f });
        panel.frame (*view);
        panel.frame (*view);
        REQUIRE_FALSE (view->capturesKeyboard());
        requireUnchanged (params.comp, before);
    }
    SECTION ("a value being typed is dropped when another session opens")
    {
        const auto id = compBandParam (0, CompBandField::Makeup);
        ImVec2 point;
        REQUIRE (view->controlPointForScenario ("mb_makeup_0", point));
        panel.movePointer (point);
        panel.frame (*view);
        const auto control = ImGui::GetIO().ConfigMacOSXBehaviors ? ImGuiMod_Super : ImGuiMod_Ctrl;
        panel.key (control, true);
        panel.frame (*view);
        panel.pressPointer (true);
        panel.frame (*view);
        panel.pressPointer (false);
        panel.key (control, false);
        panel.frame (*view);
        panel.frame (*view);
        REQUIRE (view->capturesKeyboard());
        panel.type ("3");
        panel.frame (*view);
        view->sessionReplaced();
        params.comp.set (id, 1.5f);
        const auto loaded = multibandValues (params.comp);
        panel.key (ImGuiKey_Enter, true);
        panel.frame (*view);
        panel.key (ImGuiKey_Enter, false);
        panel.frame (*view);
        panel.frame (*view);
        requireUnchanged (params.comp, loaded);
        REQUIRE_FALSE (view->capturesKeyboard());
    }
    REQUIRE_FALSE (params.compEnabled.load());
}

TEST_CASE ("the mastering multiband view keeps two bands on and the crossovers apart, as the plug-in plays them",
           "[mastering][multiband][imgui]")
{
    Session session;
    auto& comp = session.mastering().comp;
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (session.mastering(), stage, presetChoice);

    SECTION ("the last two enabled bands are locked on, and a band switched off drops its solo")
    {
        comp.set (compBandParam (2, CompBandField::Enabled), 0.0f);
        comp.set (compBandParam (3, CompBandField::Enabled), 0.0f);
        comp.set (compBandParam (1, CompBandField::Solo), 1.0f);
        panel.frame (*view);
        panel.clickControl (*view, "mb_enabled_0");
        REQUIRE (comp.get (compBandParam (0, CompBandField::Enabled)) > 0.5f);
        panel.clickControl (*view, "mb_enabled_2");
        REQUIRE (comp.get (compBandParam (2, CompBandField::Enabled)) > 0.5f);
        panel.clickControl (*view, "mb_enabled_1");
        REQUIRE (comp.get (compBandParam (1, CompBandField::Enabled)) < 0.5f);
        REQUIRE (comp.get (compBandParam (1, CompBandField::Solo)) < 0.5f);
    }
    SECTION ("S does nothing on a band that is off, and solos it once it is on")
    {
        const ImU32 dimmedOff = dw::withAlpha (IM_COL32 (0x26, 0x26, 0x2e, 0xff), 0.5f);
        const auto dimmed = [&panel, dimmedOff]
        {
            const auto& colours = panel.lastVertexColours();
            return std::count (colours.begin(), colours.end(), dimmedOff);
        };
        comp.set (compBandParam (2, CompBandField::Enabled), 0.0f);
        panel.frame (*view);
        CHECK (dimmed() > 0);
        const auto before = multibandValues (comp);
        panel.clickControl (*view, "mb_solo_2");
        requireUnchanged (comp, before);
        panel.clickControl (*view, "mb_enabled_2");
        REQUIRE (comp.get (compBandParam (2, CompBandField::Enabled)) > 0.5f);
        REQUIRE (comp.get (compBandParam (2, CompBandField::Solo)) < 0.5f);
        CHECK (dimmed() == 0);
        panel.clickControl (*view, "mb_solo_2");
        REQUIRE (comp.get (compBandParam (2, CompBandField::Solo)) > 0.5f);
    }
    SECTION ("S solos a band that is off but plays to make up two")
    {
        for (int b = 1; b < kCompBandCount; ++b)
            comp.set (compBandParam (b, CompBandField::Enabled), 0.0f);
        panel.frame (*view);
        auto before = multibandValues (comp);
        panel.clickControl (*view, "mb_solo_1");
        requireOnly (comp, before, compBandParam (1, CompBandField::Solo));
        before = multibandValues (comp);
        panel.clickControl (*view, "mb_solo_3");
        requireUnchanged (comp, before);
    }
    SECTION ("a crossover set under the one below it shows where the plug-in plays it")
    {
        comp.set (CompParamId::MbCrossover1, 1.0f);
        comp.set (CompParamId::MbCrossover2, 0.0f);
        panel.frame (*view);
        panel.frame (*view);
        ImVec2 low, mid;
        REQUIRE (view->controlPointForScenario ("mb_crossover_1", low));
        REQUIRE (view->controlPointForScenario ("mb_crossover_2", mid));
        REQUIRE (mid.x > low.x);
        REQUIRE_THAT (comp.get (CompParamId::MbCrossover2), Catch::Matchers::WithinAbs (0.0, 0.0));
    }
}

TEST_CASE ("a press on a crossover's label drags that crossover, however close the next one sits",
           "[mastering][multiband][imgui]")
{
    Session session;
    auto& comp = session.mastering().comp;
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.setSize ({ 400.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (session.mastering(), stage, presetChoice);
    // The plug-in plays the middle split at no less than 1.5 times the low one, which
    // puts each label's inner half over the other split's press area.
    float low = 0.0f;
    REQUIRE (stage.valueFromText (CompParamId::MbCrossover1, "400", low));
    comp.set (CompParamId::MbCrossover1, low);
    comp.set (CompParamId::MbCrossover2, compParamSpec (CompParamId::MbCrossover2).hostMin);
    panel.frame (*view);
    panel.frame (*view);
    ImVec2 lowLabel, middleLabel, plotEnd;
    REQUIRE (view->controlPointForScenario ("mb_crossover_1", lowLabel));
    REQUIRE (view->controlPointForScenario ("mb_crossover_2", middleLabel));
    REQUIRE (middleLabel.y < lowLabel.y);
    REQUIRE (middleLabel.x - lowLabel.x < 25.0f);
    const float between = 0.5f * (lowLabel.x + middleLabel.x);

    const auto drag = [&] (ImVec2 start, float plotFraction)
    {
        REQUIRE (view->controlPointForScenario ("plot", plotEnd, plotFraction));
        panel.movePointer (start);
        panel.frame (*view);
        panel.frame (*view);
        panel.pressPointer (true);
        panel.frame (*view);
        for (int step = 1; step <= 4; ++step)
        {
            const float t = (float) step / 4.0f;
            panel.movePointer ({ start.x + (plotEnd.x - start.x) * t, start.y + (plotEnd.y - start.y) * t });
            panel.frame (*view);
        }
        panel.pressPointer (false);
        panel.frame (*view);
        panel.frame (*view);
    };

    SECTION ("the middle split's label, above the low one")
    {
        const auto before = multibandValues (comp);
        drag ({ between, middleLabel.y }, 0.95f);
        requireOnly (comp, before, CompParamId::MbCrossover2);
        REQUIRE (comp.get (CompParamId::MbCrossover2) > before[(std::size_t) CompParamId::MbCrossover2]);
    }
    SECTION ("the low split's label")
    {
        const auto before = multibandValues (comp);
        drag ({ between, lowLabel.y }, 0.05f);
        requireOnly (comp, before, CompParamId::MbCrossover1);
        REQUIRE (comp.get (CompParamId::MbCrossover1) < before[(std::size_t) CompParamId::MbCrossover1]);
    }
}

TEST_CASE ("the mastering multiband display marks the bands that are bypassed, soloed and off",
           "[mastering][multiband][imgui]")
{
    Session session;
    auto& params = session.mastering();
    auto& comp = params.comp;
    params.compEnabled.store (true);
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);

    const auto hex = [] (unsigned int c)
    {
        return IM_COL32 ((c >> 24) & 0xff, (c >> 16) & 0xff, (c >> 8) & 0xff, c & 0xff);
    };
    const unsigned int bandColours[kCompBandCount] = { 0xf07070ff, 0xe0c850ff, 0x70c870ff, 0x5a9ae8ff };
    const ImU32 bypassText = dw::withAlpha (hex (0xe0a040ff), 0.8f);
    const ImU32 soloText = hex (0xd4d400ff);
    const ImU32 idleCrossover = hex (0x9090a0a0);
    // Vertices of this colour the last frame put in the band display, between its top
    // and the crossover handles' row.
    const auto inDisplay = [&] (ImU32 colour)
    {
        ImVec2 top, handles;
        REQUIRE (view->controlPointForScenario ("meter_0", top, 0.0f));
        REQUIRE (view->controlPointForScenario ("plot", handles, 0.5f));
        int count = 0;
        const auto& positions = panel.lastVertexPositions();
        const auto& colours = panel.lastVertexColours();
        for (std::size_t i = 0; i < positions.size(); ++i)
            if (colours[i] == colour && positions[i].y >= top.y && positions[i].y <= handles.y) ++count;
        return count;
    };
    const auto settle = [&]
    {
        panel.frame (*view);
        panel.frame (*view);
    };

    settle();
    CHECK (inDisplay (bypassText) == 0);
    CHECK (inDisplay (soloText) == 0);
    CHECK (inDisplay (dw::withAlpha (hex (bandColours[1]), 0.14f)) > 0);
    CHECK (std::count (panel.lastVertexColours().begin(), panel.lastVertexColours().end(), idleCrossover) == 0);

    comp.set (compBandParam (1, CompBandField::Bypass), 1.0f);
    settle();
    CHECK (inDisplay (bypassText) > 0);
    CHECK (inDisplay (dw::withAlpha (hex (bandColours[1]), 0.05f)) > 0);
    CHECK (inDisplay (dw::withAlpha (hex (bandColours[1]), 0.14f)) == 0);

    comp.set (compBandParam (1, CompBandField::Bypass), 0.0f);
    comp.set (compBandParam (2, CompBandField::Solo), 1.0f);
    settle();
    CHECK (inDisplay (bypassText) == 0);
    CHECK (inDisplay (soloText) > 0);
    CHECK (inDisplay (dw::withAlpha (hex (bandColours[2]), 0.9f)) > 0);

    comp.set (compBandParam (2, CompBandField::Solo), 0.0f);
    comp.set (compBandParam (3, CompBandField::Enabled), 0.0f);
    settle();
    ImVec2 point;
    CHECK_FALSE (view->controlPointForScenario ("meter_3", point, 0.0f));
    CHECK (view->controlPointForScenario ("meter_2", point, 0.0f));
    CHECK (std::count (panel.lastVertexColours().begin(), panel.lastVertexColours().end(), idleCrossover) > 0);
}

TEST_CASE ("the mastering multiband meters follow the plug-in's band reduction while it is in and rest at 0 while out",
           "[mastering][multiband][imgui]")
{
    Session session;
    auto& params = session.mastering();
    MultiCompStage stage (CompStage::Multiband);
    stage.prepare (48000.0, 256, 2);
    for (int band = 0; band < kCompBandCount; ++band)
        params.comp.set (compBandParam (band, CompBandField::Threshold), -50.0f);
    stage.pushParameters (params.comp, true, 0);
    std::vector<float> left (256), right (256);
    std::uint32_t state = 7;
    for (int block = 0; block < 200; ++block)
    {
        for (std::size_t i = 0; i < left.size(); ++i)
        {
            state = state * 1664525u + 1013904223u;
            left[i] = right[i] = (static_cast<float> (state >> 8) / 8388608.0f - 1.0f) * 0.5f;
        }
        stage.process (left.data(), right.data(), 256);
    }
    for (int band = 0; band < kCompBandCount; ++band)
    {
        CAPTURE (band);
        REQUIRE (stage.bandGainReductionDb (band) < -1.0f);
    }

    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    panel.setDeltaTime (1.0f / 30.0f);
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
    const auto depth = [&view] (int band)
    {
        ImVec2 top, foot;
        REQUIRE (view->controlPointForScenario ("meter_" + std::to_string (band), top, 0.0f));
        REQUIRE (view->controlPointForScenario ("meter_" + std::to_string (band), foot, 1.0f));
        return foot.y - top.y;
    };

    params.compEnabled.store (false);
    for (int frame = 0; frame < 4; ++frame) panel.frame (*view);
    for (int band = 0; band < kCompBandCount; ++band)
        REQUIRE_THAT (depth (band), Catch::Matchers::WithinAbs (0.0, 0.0));

    params.compEnabled.store (true);
    for (int frame = 0; frame < 4; ++frame) panel.frame (*view);
    for (int band = 0; band < kCompBandCount; ++band)
        REQUIRE (depth (band) > 1.0f);

    params.compEnabled.store (false);
    for (int frame = 0; frame < 120; ++frame) panel.frame (*view);
    for (int band = 0; band < kCompBandCount; ++band)
        REQUIRE_THAT (depth (band), Catch::Matchers::WithinAbs (0.0, 0.0));
    REQUIRE (view->liveStamp().has_value());
}

TEST_CASE ("the mastering multiband preset picker opens inside the panel, applies a preset and keeps its name",
           "[mastering][multiband][imgui]")
{
    Session session;
    auto& params = session.mastering();
    params.compEnabled.store (false);
    MultiCompStage stage (CompStage::Multiband);
    CompParams pop;
    REQUIRE (mbpresets::apply (1, stage, pop));
    params.comp.set (CompParamId::MbMix, 60.0f);
    params.comp.set (CompParamId::MbOutput, -3.0f);
    params.comp.set (CompParamId::AutoMakeup, 1.0f);
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
    panel.frame (*view);
    panel.frame (*view);

    // In the smallest panel the page gives the compressor the whole list still fits.
    panel.setSize ({ 400.0f, 410.0f });
    panel.frame (*view);
    panel.frame (*view);
    panel.clickControl (*view, "comp_preset");
    for (int entry = 0; entry <= mbpresets::kNumPresets; ++entry)
    {
        CAPTURE (entry);
        ImVec2 at;
        REQUIRE (view->controlPointForScenario ("list:" + std::to_string (entry), at));
    }
    panel.key (ImGuiKey_Escape, true);
    panel.frame (*view);
    panel.key (ImGuiKey_Escape, false);
    panel.frame (*view);
    REQUIRE_FALSE (view->capturesKeyboard());
    panel.setSize ({ 483.0f, 600.0f });
    panel.frame (*view);
    panel.frame (*view);

    panel.clickControl (*view, "comp_preset");
    REQUIRE (view->capturesKeyboard());
    // Every other control stays drawn while the list is open.
    ImVec2 point;
    REQUIRE (view->controlPointForScenario ("comp_enabled", point));
    // Every entry shows, inside the panel, without scrolling.
    for (int entry = 0; entry <= mbpresets::kNumPresets; ++entry)
    {
        CAPTURE (entry);
        REQUIRE (view->controlPointForScenario ("list:" + std::to_string (entry), point));
    }
    panel.clickControl (*view, "list:2");
    REQUIRE_FALSE (view->capturesKeyboard());
    REQUIRE (presetChoice == 2);
    for (const auto id : compStageParams (CompStage::Multiband))
    {
        CAPTURE (compParamSpec (id).symbol);
        if (id == CompParamId::MbMix || id == CompParamId::MbOutput) continue;
        const bool highMidLevel = id >= compBandParam (2, CompBandField::Threshold)
                               && id <= compBandParam (2, CompBandField::Makeup);
        if (highMidLevel) continue;
        REQUIRE_THAT (params.comp.get (id), Catch::Matchers::WithinAbs (pop.get (id), 0.0));
    }
    REQUIRE_THAT (params.comp.get (CompParamId::MbMix), Catch::Matchers::WithinAbs (60.0, 0.0));
    REQUIRE_THAT (params.comp.get (CompParamId::MbOutput), Catch::Matchers::WithinAbs (-3.0, 0.0));
    REQUIRE (params.comp.get (CompParamId::AutoMakeup) < 0.5f);
    REQUIRE_FALSE (params.compEnabled.load());

    // A band changed afterwards leaves the picked name showing.
    const auto lowThreshold = compBandParam (0, CompBandField::Threshold);
    params.comp.set (lowThreshold, -12.0f);
    panel.frame (*view);
    REQUIRE (presetChoice == 2);

    // Picking the name it shows applies that preset again.
    REQUIRE (std::abs (pop.get (lowThreshold) - (-12.0f)) > 1.0e-3f);
    panel.clickControl (*view, "comp_preset");
    panel.clickControl (*view, "list:2");
    REQUIRE (presetChoice == 2);
    REQUIRE_THAT (params.comp.get (lowThreshold), Catch::Matchers::WithinAbs (pop.get (lowThreshold), 0.0));
}

TEST_CASE ("the mastering multiband header engages the compressor", "[mastering][multiband][imgui]")
{
    Session session;
    auto& params = session.mastering();
    params.compEnabled.store (false);
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
    panel.frame (*view);
    panel.frame (*view);
    panel.clickControl (*view, "comp_enabled");
    REQUIRE (params.compEnabled.load());
    panel.movePointer (kHeaderPoint);
    panel.frame (*view);
    panel.frame (*view);
    panel.pressPointer (true);
    panel.frame (*view);
    panel.pressPointer (false);
    panel.frame (*view);
    REQUIRE_FALSE (params.compEnabled.load());
}

TEST_CASE ("a list open in a mastering panel closes when the user turns away, not when its child loses the keyboard",
           "[mastering][imgui]")
{
    Session session;
    auto& params = session.mastering();
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    const auto closesOnFocusLoss = [&panel] (imgui::DuskPanelView& view, const char* control)
    {
        CAPTURE (control);
        panel.frame (view);
        panel.frame (view);
        panel.clickControl (view, control);
        ImVec2 point;
        REQUIRE (view.capturesKeyboard());
        REQUIRE (view.controlPointForScenario ("list:0", point));
        panel.movePointer ({ -100.0f, -100.0f });
        // An X11 window manager takes the child's focus on a press inside it.
        view.focusLost();
        panel.frame (view);
        panel.frame (view);
        REQUIRE (view.capturesKeyboard());
        REQUIRE (view.controlPointForScenario ("list:0", point));
        view.closeLists();
        panel.frame (view);
        panel.frame (view);
        REQUIRE_FALSE (view.capturesKeyboard());
        REQUIRE_FALSE (view.controlPointForScenario ("list:0", point));
    };

    SECTION ("the multiband presets")
    {
        MultiCompStage stage (CompStage::Multiband);
        int presetChoice = 0;
        const auto before = multibandValues (params.comp);
        auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
        closesOnFocusLoss (*view, "comp_preset");
        REQUIRE (presetChoice == 0);
        requireUnchanged (params.comp, before);
    }
    SECTION ("the limiter's mode")
    {
        BrickwallLimiter limiter;
        const int mode = params.limiterMode.load();
        auto view = imgui::makeMasteringLimiterView (params, limiter);
        closesOnFocusLoss (*view, "limiter_mode");
        REQUIRE (params.limiterMode.load() == mode);
    }
    SECTION ("the Phase display")
    {
        PhaseAnalyzer analyzer;
        bool mono = false;
        auto view = imgui::makeMasteringPhaseView (params.phase, analyzer, [&] { return mono; },
                                                  [&] (bool value) { mono = value; });
        closesOnFocusLoss (*view, "mode");
        ImVec2 point;
        REQUIRE (view->controlPointForScenario ("phase_display:0", point));
    }
}

TEST_CASE ("a mastering panel reports an open list's entries only while it draws them", "[mastering][imgui]")
{
    Session session;
    auto& params = session.mastering();
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
    panel.frame (*view);
    panel.frame (*view);
    panel.clickControl (*view, "comp_preset");
    ImVec2 point;
    REQUIRE (view->controlPointForScenario ("list:1", point));
    REQUIRE (view->controlPointForScenario ("list:" + std::to_string (mbpresets::kNumPresets), point));
    for (const auto* name : { "list:", "list:typo", "list:1x", "list:-1", "list: 1", "list:99", "list:99999999999999999999" })
    {
        CAPTURE (name);
        REQUIRE_FALSE (view->controlPointForScenario (name, point));
    }

    SECTION ("the frame a pick closes the list")
    {
        REQUIRE (view->controlPointForScenario ("list:2", point));
        panel.movePointer (point);
        panel.frame (*view);
        panel.frame (*view);
        panel.pressPointer (true);
        panel.frame (*view);
        panel.pressPointer (false);
        panel.frame (*view);
        REQUIRE (presetChoice == 2);
        REQUIRE_FALSE (view->controlPointForScenario ("list:0", point));
    }
}

TEST_CASE ("the limiter reports no Mode entries from a frame that did not draw its picker", "[mastering][imgui]")
{
    Session session;
    BrickwallLimiter limiter;
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    auto view = imgui::makeMasteringLimiterView (session.mastering(), limiter);
    panel.frame (*view);
    panel.frame (*view);
    panel.clickControl (*view, "limiter_mode");
    ImVec2 point;
    REQUIRE (view->controlPointForScenario ("list:0", point));
    panel.setSize ({ 60.0f, 600.0f });
    panel.frame (*view);
    REQUIRE_FALSE (view->controlPointForScenario ("list:0", point));
}

TEST_CASE ("a scenario control point stays inside the control, however short it is", "[mastering][imgui]")
{
    imgui::ScenarioControlMap map;
    map.begin ({ 0.0f, 0.0f, 100.0f, 100.0f });
    map.note ("tall", { 10.0f, 10.0f, 20.0f, 30.0f });
    map.note ("thin", { 40.0f, 50.0f, 60.0f, 50.4f });
    map.settle();
    for (const float fraction : { 0.0f, 0.5f, 1.0f })
    {
        CAPTURE (fraction);
        float x = 0.0f, y = 0.0f;
        REQUIRE (map.point ("thin", fraction, x, y));
        REQUIRE_THAT (y, Catch::Matchers::WithinAbs (50.2, 1.0e-5));
        REQUIRE (map.point ("tall", fraction, x, y));
        REQUIRE_THAT (y, Catch::Matchers::WithinAbs (29.5 - fraction * 19.0, 1.0e-5));
    }
}

TEST_CASE ("a pick held down in a mastering list takes, whatever asks the lists to close meanwhile", "[mastering][imgui]")
{
    Session session;
    auto& params = session.mastering();
    MultiCompStage stage (CompStage::Multiband);
    HeadlessPanel panel;
    panel.setSize ({ 483.0f, 600.0f });
    panel.movePointer ({ -100.0f, -100.0f });
    int presetChoice = 0;
    auto view = imgui::makeMasteringCompView (params, stage, presetChoice);
    panel.frame (*view);
    panel.frame (*view);
    panel.clickControl (*view, "comp_preset");
    ImVec2 point;
    REQUIRE (view->controlPointForScenario ("list:2", point));
    panel.movePointer (point);
    panel.frame (*view);
    panel.frame (*view);
    panel.pressPointer (true);
    panel.frame (*view);
    // An X11 server can hand the page the same press, and the page asks every panel
    // to close its lists before the release arrives.
    view->closeLists();
    panel.frame (*view);
    panel.pressPointer (false);
    panel.frame (*view);
    panel.frame (*view);
    REQUIRE (presetChoice == 2);
    REQUIRE_FALSE (view->capturesKeyboard());
}
