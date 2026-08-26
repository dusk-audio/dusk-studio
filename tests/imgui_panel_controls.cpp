#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/imgui/DuskTheme.h"
#include "ui/imgui/PanelControls.h"

// FindWindowByName: a tooltip is a window of its own, and its size is the only place
// its wrapping is observable from outside a renderer.
#include <DearImGui/imgui_internal.h>

#include <string>

// The settings panels' form controls, driven with no window, no GL and no compositor:
// a frame is submitted, synthetic pointer events are fed through Dear ImGui's own input
// queue, and what is asserted is the value that came back out. The panel views
// themselves need an engine and a device manager, but this layer - which is where a
// mis-sized row or a mis-wired pick would show up - does not.

using Catch::Matchers::WithinAbs;
namespace dw = DuskWidgets;
using namespace duskstudio::imgui;

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
        io.DisplaySize = ImVec2 (600.0f, 400.0f);
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

    // Runs one frame, handing the caller a context wired the way DuskPanelWindow wires
    // one. `body` submits the controls under test.
    template <typename Body>
    void frame (Body&& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImGui::GetIO().DisplaySize);
        ImGui::Begin ("##panel", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        dw::Context ctx;
        ctx.dl = ImGui::GetWindowDrawList();
        ctx.theme = &consolePalette().widgets;
        ctx.fonts = &fonts;
        ctx.drag = &drag;
        ctx.scale = 1.0f;

        {
            const ScopedFormStyle style (ctx);
            body (ctx);
        }

        ImGui::End();
        ImGui::Render();
    }

private:
    ImGuiContext* context = nullptr;
    dw::Fonts fonts;
    dw::DragState drag;
};

constexpr ImVec2 kRowTl { 20.0f, 40.0f };
constexpr ImVec2 kRowBr { 320.0f, 66.0f };
constexpr ImVec2 kRowCentre { 170.0f, 53.0f };
} // namespace

TEST_CASE ("ComboModel hands formCombo pointers that survive the list growing")
{
    ComboModel model;
    for (int i = 0; i < 64; ++i)
        model.add ("device " + std::to_string (i));
    model.finish (7);

    REQUIRE (model.count() == 64);
    REQUIRE (model.selected == 7);
    for (int i = 0; i < model.count(); ++i)
        REQUIRE (std::string (model.items[static_cast<std::size_t> (i)])
                 == model.label (i));

    // Out of range reads back empty rather than walking off the vector: a combo whose
    // device list shrank under it is the normal hot-plug case.
    REQUIRE (model.label (-1).empty());
    REQUIRE (model.label (64).empty());
}

TEST_CASE ("ComboModel::clear drops the previous list entirely")
{
    ComboModel model;
    model.add ("one");
    model.add ("two");
    model.finish (1);
    model.clear();

    REQUIRE (model.count() == 0);
    REQUIRE (model.selected == -1);
    REQUIRE (model.labels.empty());
}

TEST_CASE ("formCheckbox toggles on a click inside its row")
{
    HeadlessPanel panel;
    bool value = false;
    bool toggled = false;

    const auto submit = [&] (dw::Context& ctx)
    {
        toggled = formCheckbox (ctx, "##toggle", kRowTl, kRowBr.y - kRowTl.y,
                                "Expand tape strip by default", value);
    };

    // Two settling frames: Dear ImGui decides what the pointer is over from the window
    // it hovered on the previous frame, so a click on the very first frame lands nowhere.
    panel.movePointer (ImVec2 (kRowTl.x + 8.0f, kRowCentre.y));
    panel.frame (submit);
    panel.frame (submit);
    REQUIRE_FALSE (toggled);

    panel.pressPointer (true);
    panel.frame (submit);
    panel.pressPointer (false);
    panel.frame (submit);

    REQUIRE (toggled);
    REQUIRE (value);
}

TEST_CASE ("formCheckbox ignores a click outside its row")
{
    HeadlessPanel panel;
    bool value = false;
    bool toggled = false;

    const auto submit = [&] (dw::Context& ctx)
    {
        toggled = formCheckbox (ctx, "##toggle", kRowTl, kRowBr.y - kRowTl.y, "Off",
                                value);
    };

    panel.movePointer (ImVec2 (500.0f, 300.0f));
    panel.frame (submit);
    panel.frame (submit);
    panel.pressPointer (true);
    panel.frame (submit);
    panel.pressPointer (false);
    panel.frame (submit);

    REQUIRE_FALSE (toggled);
    REQUIRE_FALSE (value);
}

TEST_CASE ("formSlider reports the release that persists the value")
{
    HeadlessPanel panel;
    float value = 1.0f;
    FormSliderResult result;

    const auto submit = [&] (dw::Context& ctx)
    {
        result = formSlider (ctx, "##scale", kRowTl, kRowBr, value, 0.5f, 2.0f, "%.2fx");
    };

    panel.movePointer (kRowCentre);
    panel.frame (submit);
    panel.frame (submit);

    panel.pressPointer (true);
    panel.frame (submit);
    REQUIRE (result.changed);
    REQUIRE_FALSE (result.released);
    // Grabbed at the centre of a 0.5..2.0 track, so the value lands mid-range.
    REQUIRE_THAT (value, WithinAbs (1.25f, 0.06f));

    panel.pressPointer (false);
    panel.frame (submit);
    REQUIRE (result.released);
}

TEST_CASE ("formSlider clamps a value dragged past the end of its range")
{
    HeadlessPanel panel;
    float value = 1.0f;

    const auto submit = [&] (dw::Context& ctx)
    {
        formSlider (ctx, "##offset", kRowTl, kRowBr, value, -100.0f, 100.0f, "%.0f smp");
    };

    panel.movePointer (kRowCentre);
    panel.frame (submit);
    panel.frame (submit);
    panel.pressPointer (true);
    panel.frame (submit);
    panel.movePointer (ImVec2 (kRowBr.x + 400.0f, kRowCentre.y));
    panel.frame (submit);
    panel.pressPointer (false);
    panel.frame (submit);

    REQUIRE_THAT (value, WithinAbs (100.0f, 0.001f));
}

TEST_CASE ("formCombo opens on a click and reports the item picked")
{
    HeadlessPanel panel;
    const char* const items[] = { "(none)", "UMC1820", "Built-in Audio" };
    int selected = 0;
    bool picked = false;

    const auto submit = [&] (dw::Context& ctx)
    {
        picked = formCombo (ctx, "##device", kRowTl, kRowBr, items, 3, selected);
    };

    panel.movePointer (kRowCentre);
    panel.frame (submit);
    panel.frame (submit);
    REQUIRE_FALSE (picked);

    panel.pressPointer (true);
    panel.frame (submit);
    panel.pressPointer (false);
    panel.frame (submit);

    // The popup is laid out under the closed combo, so its second row is one item
    // height below the first.
    const float itemHeight = ImGui::GetTextLineHeightWithSpacing();
    panel.movePointer (ImVec2 (kRowCentre.x, kRowBr.y + itemHeight * 1.5f));
    panel.frame (submit);
    panel.pressPointer (true);
    panel.frame (submit);
    panel.pressPointer (false);
    panel.frame (submit);

    REQUIRE (picked);
    REQUIRE (selected == 1);
}

TEST_CASE ("formTooltip wraps the prose a settings row explains itself with")
{
    HeadlessPanel panel;
    bool value = false;
    float unwrappedWidth = 0.0f;

    static const char* const kProse =
        "Samples subtracted from each recorded audio take's timeline start, to "
        "compensate for input round-trip latency (converters, external gear, "
        "unreported plugin delay). Saved per-machine; applies to the next take.";

    const auto submit = [&] (dw::Context& ctx)
    {
        formCheckbox (ctx, "##row", kRowTl, kRowBr.y - kRowTl.y, "A settings row", value);
        formTooltip (kProse);
        unwrappedWidth = ImGui::CalcTextSize (kProse).x;
    };

    // A tooltip's hover test carries a stationary check and a short delay, so it takes
    // a run of frames with the pointer held still before the tip is submitted at all.
    panel.movePointer (ImVec2 (kRowTl.x + 8.0f, kRowCentre.y));
    for (int i = 0; i < 40; ++i)
        panel.frame (submit);

    auto* const tip = ImGui::FindWindowByName ("##Tooltip_00");
    REQUIRE (tip != nullptr);
    // A tooltip window always auto-fits its content, so an unwrapped one is as wide as
    // the whole sentence - several times the display.
    REQUIRE (unwrappedWidth > 0.0f);
    REQUIRE (tip->Size.x < unwrappedWidth * 0.5f);
}

TEST_CASE ("formCombo on a disabled row does not open")
{
    HeadlessPanel panel;
    const char* const items[] = { "44100 Hz", "48000 Hz" };
    int selected = 1;
    bool picked = false;

    const auto submit = [&] (dw::Context& ctx)
    {
        picked = formCombo (ctx, "##rate", kRowTl, kRowBr, items, 2, selected,
                            /*enabled*/ false);
    };

    panel.movePointer (kRowCentre);
    panel.frame (submit);
    panel.frame (submit);
    panel.pressPointer (true);
    panel.frame (submit);
    panel.pressPointer (false);
    panel.frame (submit);

    REQUIRE_FALSE (picked);
    REQUIRE (selected == 1);
}
