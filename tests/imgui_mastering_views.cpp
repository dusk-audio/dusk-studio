#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "dsp/BrickwallLimiter.h"
#include "session/Session.h"
#include "ui/imgui/DuskTheme.h"
#include "ui/imgui/MasteringEqView.h"
#include "ui/imgui/MasteringLimiterView.h"

#include <DuskWidgets.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

// The mastering stage's two native panels drawn with no window, no GL and no
// compositor. What is asserted is the contract a draw pass has to keep: it paints
// something, it does not write to the parameters it is only reading, and a click on the
// section header - the one control whose position is a fixed design constant rather than
// a share of the panel - engages the section.

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

    const std::vector<ImVec2>& lastVertexPositions() const { return vertexPositions; }

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
        return vertices;
    }

private:
    ImGuiContext* context = nullptr;
    dw::Fonts fonts;
    dw::DragState drag;
    std::vector<ImVec2> vertexPositions;
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
