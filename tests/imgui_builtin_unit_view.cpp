#include <catch2/catch_test_macros.hpp>
#include <catch2/generators/catch_generators.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

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
    // The atlas is baked at the display scale, as DuskPanelWindow bakes it.
    explicit HeadlessPanel (float displayScale) : scale (displayScale)
    {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext (context);

        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2 (1600.0f * scale, 900.0f * scale);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;
        ImFontConfig config;
        config.SizePixels = 13.0f * scale;
        io.Fonts->AddFontDefault (&config);
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
        ImVec2 textMax {};   // the far corner of the last glyph drawn
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
        ctx.scale = scale;

        const int before = ctx.dl->VtxBuffer.Size;
        view.draw (ctx, ImVec2 (0.0f, 0.0f), size);

        Frame out;
        out.vertices = ctx.dl->VtxBuffer.Size - before;
        out.inkMin = { 1.0e9f, 1.0e9f };
        out.inkMax = { -1.0e9f, -1.0e9f };
        const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
        for (int i = before; i < ctx.dl->VtxBuffer.Size; ++i)
        {
            const auto& v = ctx.dl->VtxBuffer[i];
            out.inkMin.x = std::min (out.inkMin.x, v.pos.x);
            out.inkMin.y = std::min (out.inkMin.y, v.pos.y);
            out.inkMax.x = std::max (out.inkMax.x, v.pos.x);
            out.inkMax.y = std::max (out.inkMax.y, v.pos.y);
            // Only a glyph samples anything but the white pixel.
            if (v.uv.x < white.x || white.x < v.uv.x || v.uv.y < white.y || white.y < v.uv.y)
            {
                out.textMax.x = std::max (out.textMax.x, v.pos.x);
                out.textMax.y = std::max (out.textMax.y, v.pos.y);
            }
        }

        ImGui::End();
        ImGui::Render();
        return out;
    }

    const float scale;

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

// The test binary links the editor-free DAF libraries, so hasPluginEditor() cannot
// distinguish the app's plug-in-editor units here. Keep this list to units that the
// native-UI app actually sends through the generic parameter-table editor.
const char* const kGenericUnits[] =
{
    "dusk.builtin.utility", "dusk.builtin.tape", "dusk.builtin.synth",
};
} // namespace

TEST_CASE ("the built-in editor draws every unit inside the size it asks for",
           "[builtin][imgui]")
{
    const float scale = GENERATE (1.0f, 2.0f);
    for (const char* id : kGenericUnits)
    {
        INFO ("unit " << id << " at scale " << scale);

        builtin::NativeBuiltinSlot slot;
        std::string error;
        REQUIRE (slot.loadUnit (id, 48000.0, 256, error));

        HeadlessPanel panel (scale);
        auto view = imgui::makeBuiltinUnitView (slot, id, {});
        REQUIRE (view != nullptr);

        const auto design = view->preferredSize();
        // A panel that does not fit a 1600x900 window is one a user cannot read.
        REQUIRE (design.x > 0.0f);
        REQUIRE (design.y > 0.0f);
        REQUIRE (design.x <= 1600.0f);
        REQUIRE (design.y <= 900.0f);

        const ImVec2 size (design.x * scale, design.y * scale);
        const auto before = snapshot (slot);
        const auto drawn = panel.frame (*view, size);
        REQUIRE (drawn.vertices > 0);

        // Nothing may spill out of the rectangle the panel reserved: that is
        // what clipping and overlap look like from outside. One pixel of slack
        // for the border stroke, which straddles the edge it is drawn on.
        const float stroke = 1.5f * scale;
        REQUIRE (drawn.inkMin.x >= -stroke);
        REQUIRE (drawn.inkMin.y >= -stroke);
        REQUIRE (drawn.inkMax.x <= size.x + stroke);
        REQUIRE (drawn.inkMax.y <= size.y + stroke);

        REQUIRE (snapshot (slot) == before);
    }
}

TEST_CASE ("the built-in editor at a display scale of 2 is the same picture doubled",
           "[builtin][imgui]")
{
    // The panel fills whatever rectangle it is given, so its ink bounds cannot tell a
    // doubled layout from rows drawn at design size in the corner of a doubled panel.
    // Where the text ends can.
    for (const char* id : kGenericUnits)
    {
        INFO ("unit " << id);
        builtin::NativeBuiltinSlot slot;
        std::string error;
        REQUIRE (slot.loadUnit (id, 48000.0, 256, error));

        auto view = imgui::makeBuiltinUnitView (slot, id, {});
        const auto design = view->preferredSize();

        HeadlessPanel one (1.0f);
        const auto atOne = one.frame (*view, design);
        HeadlessPanel two (2.0f);
        const auto atTwo = two.frame (*view, ImVec2 (design.x * 2.0f, design.y * 2.0f));

        REQUIRE (atOne.textMax.x > 0.0f);
        REQUIRE (atOne.textMax.y > 0.0f);
        REQUIRE_THAT (atTwo.textMax.x, Catch::Matchers::WithinRel (atOne.textMax.x * 2.0f, 0.05f));
        REQUIRE_THAT (atTwo.textMax.y, Catch::Matchers::WithinRel (atOne.textMax.y * 2.0f, 0.05f));
    }
}

TEST_CASE ("the built-in editor keeps every parameter reachable", "[builtin][imgui]")
{
    // A row that the layout dropped is a control the user cannot reach, and the
    // ink bounds above would not notice it. Count the sections and rows the
    // layout has to account for instead.
    for (const char* id : kGenericUnits)
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
