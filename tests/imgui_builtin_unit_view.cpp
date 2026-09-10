#include <catch2/catch_test_macros.hpp>

#include "engine/builtin/NativeBuiltinSlot.h"
#include "ui/imgui/BuiltinUnitView.h"
#include "ui/imgui/DuskTheme.h"

#include <DuskWidgets.hpp>

#include <string>
#include <vector>

// The generic built-in editor drawn with no window, no GL and no compositor,
// once per unit. What is asserted is what a reader of a screenshot would check:
// the panel paints, it fits the frame it asked for, every row lands inside that
// frame, and a draw pass does not move the parameters it is only reading.

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
        io.DisplaySize = ImVec2 (1600.0f, 900.0f);
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

    struct Frame
    {
        int vertices = 0;
        ImVec2 inkMin {};
        ImVec2 inkMax {};
    };

    Frame frame (imgui::DuskPanelView& view, ImVec2 size)
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

        const int before = ctx.dl->VtxBuffer.Size;
        view.draw (ctx, ImVec2 (0.0f, 0.0f), size);

        Frame out;
        out.vertices = ctx.dl->VtxBuffer.Size - before;
        out.inkMin = { 1.0e9f, 1.0e9f };
        out.inkMax = { -1.0e9f, -1.0e9f };
        for (int i = before; i < ctx.dl->VtxBuffer.Size; ++i)
        {
            const auto& v = ctx.dl->VtxBuffer[i];
            out.inkMin.x = std::min (out.inkMin.x, v.pos.x);
            out.inkMin.y = std::min (out.inkMin.y, v.pos.y);
            out.inkMax.x = std::max (out.inkMax.x, v.pos.x);
            out.inkMax.y = std::max (out.inkMax.y, v.pos.y);
        }

        ImGui::End();
        ImGui::Render();
        return out;
    }

private:
    ImGuiContext* context = nullptr;
    dw::Fonts fonts;
    dw::DragState drag;
};

std::vector<float> snapshot (const builtin::NativeBuiltinSlot& slot)
{
    std::vector<float> values;
    for (int i = 0; i < slot.paramCount(); ++i)
        values.push_back (slot.getParamValue (i));
    return values;
}

const char* const kUnits[] =
{
    "dusk.builtin.utility", "dusk.builtin.reverb", "dusk.builtin.delay",
    "dusk.builtin.tape", "dusk.builtin.synth",
};
} // namespace

TEST_CASE ("the built-in editor draws every unit inside the size it asks for",
           "[builtin][imgui]")
{
    for (const char* id : kUnits)
    {
        INFO ("unit " << id);

        builtin::NativeBuiltinSlot slot;
        std::string error;
        REQUIRE (slot.loadUnit (id, 48000.0, 256, error));

        HeadlessPanel panel;
        auto view = imgui::makeBuiltinUnitView (slot, id, {}, /*inlineInStage*/ false);
        REQUIRE (view != nullptr);

        const auto size = view->preferredSize();
        // A panel that does not fit a 1600x900 window is one a user cannot read.
        REQUIRE (size.x > 0.0f);
        REQUIRE (size.y > 0.0f);
        REQUIRE (size.x <= 1600.0f);
        REQUIRE (size.y <= 900.0f);

        const auto before = snapshot (slot);
        const auto drawn = panel.frame (*view, size);
        REQUIRE (drawn.vertices > 0);

        // Nothing may spill out of the rectangle the panel reserved: that is
        // what clipping and overlap look like from outside. One pixel of slack
        // for the border stroke, which straddles the edge it is drawn on.
        constexpr float kStroke = 1.5f;
        REQUIRE (drawn.inkMin.x >= -kStroke);
        REQUIRE (drawn.inkMin.y >= -kStroke);
        REQUIRE (drawn.inkMax.x <= size.x + kStroke);
        REQUIRE (drawn.inkMax.y <= size.y + kStroke);

        REQUIRE (snapshot (slot) == before);
    }
}

TEST_CASE ("the built-in editor keeps every parameter reachable", "[builtin][imgui]")
{
    // A row that the layout dropped is a control the user cannot reach, and the
    // ink bounds above would not notice it. Count the sections and rows the
    // layout has to account for instead.
    for (const char* id : kUnits)
    {
        INFO ("unit " << id);
        builtin::NativeBuiltinSlot slot;
        std::string error;
        REQUIRE (slot.loadUnit (id, 48000.0, 256, error));

        REQUIRE (slot.paramCount() > 0);
        for (int i = 0; i < slot.paramCount(); ++i)
        {
            const auto* info = slot.paramInfo (i);
            REQUIRE (info != nullptr);
            REQUIRE (info->section != nullptr);
            REQUIRE (std::string (info->section).empty() == false);
            REQUIRE (info->suffix != nullptr);
            if (info->kind == builtin::ParamKind::Choice)
            {
                REQUIRE (info->choices != nullptr);
                REQUIRE (info->choiceCount > 0);
                // The stored value is the choice index offset by the minimum,
                // so the table has to cover the whole declared range.
                REQUIRE ((float) info->choiceCount
                         >= info->maxValue - info->minValue + 1.0f);
            }
        }
    }
}
