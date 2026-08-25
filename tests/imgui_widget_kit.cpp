#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "../tools/gui-spike/ChannelStripView.h"
#include "../tools/gui-spike/StripLayout.h"
#include "ui/imgui/DuskTheme.h"

#include <DuskWidgets.hpp>

#include <cstring>
#include <string>
#include <vector>

// The widget kit and one full console strip drawn through it, with no window, no GL and
// no compositor: what is asserted is the draw list the frame produces. That is where the
// tower's frame-cost work lives, so it is what a regression has to be caught in.

using Catch::Matchers::WithinAbs;
namespace dw = DuskWidgets;

namespace
{
// An ImGui context with a real font atlas and a texture id a renderer would have set.
// Everything the kit does - hit testing, the baked dome, text - works without a backend.
class HeadlessImGui
{
public:
    HeadlessImGui()
    {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext (context);

        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2 (420.0f, 900.0f);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        io.Fonts->AddFontDefault();

        knobAtlas.reserve (*io.Fonts, 64);
        io.Fonts->Build();
        knobAtlas.rasterise (*io.Fonts);
        // What a renderer backend does once it has uploaded the atlas. Without it the kit
        // has no texture to point the dome quads at and falls back to drawing them.
        io.Fonts->SetTexID (static_cast<ImTextureID> (1));

        ImFont* const font = io.Fonts->Fonts.front();
        fonts.caption = fonts.label = fonts.pill = fonts.band = font;
        fonts.title = fonts.value = fonts.valueLarge = fonts.textEntry = font;
    }

    ~HeadlessImGui()
    {
        ImGui::DestroyContext (context);
    }

    // Draws one strip and returns the frame's vertex buffer.
    std::vector<ImDrawVert> drawStrip (duskspike::ChannelStripView& view,
                                       duskspike::StripParams& params, bool bakedDomes)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImGui::GetIO().DisplaySize);
        ImGui::Begin ("##console", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        dw::Context ctx;
        ctx.dl = ImGui::GetWindowDrawList();
        ctx.theme = &duskstudio::imgui::consolePalette().widgets;
        ctx.fonts = &fonts;
        ctx.knobAtlas = bakedDomes ? &knobAtlas : nullptr;
        ctx.drag = &drag;
        ctx.scale = 1.0f;

        view.draw (ctx, ImVec2 (6.0f, 6.0f), duskspike::layout::kStripWidth,
                   duskspike::layout::kMinStripHeight, params);
        widgets = ctx.widgets;

        std::vector<ImDrawVert> vertices (ctx.dl->VtxBuffer.begin(), ctx.dl->VtxBuffer.end());
        ImGui::End();
        ImGui::Render();
        return vertices;
    }

    dw::Fonts fonts;
    dw::KnobAtlas knobAtlas;
    dw::DragState drag;
    int widgets = 0;

private:
    ImGuiContext* context = nullptr;
};
} // namespace

TEST_CASE ("a console strip drawn through the kit is deterministic", "[imgui][widgets]")
{
    HeadlessImGui gui;
    duskspike::ChannelStripView view;
    duskspike::StripParams params;
    view.setStaticMeters (true);

    const auto first = gui.drawStrip (view, params, true);
    const auto second = gui.drawStrip (view, params, true);

    REQUIRE (! first.empty());
    REQUIRE (first.size() == second.size());
    REQUIRE (std::memcmp (first.data(), second.data(), first.size() * sizeof (ImDrawVert)) == 0);
    REQUIRE (gui.widgets > 30);
}

TEST_CASE ("the baked knob dome is what makes a console strip affordable", "[imgui][widgets]")
{
    HeadlessImGui gui;
    duskspike::ChannelStripView view;
    duskspike::StripParams params;
    view.setStaticMeters (true);

    const auto baked = gui.drawStrip (view, params, true).size();
    const auto vector = gui.drawStrip (view, params, false).size();

    // Measured on the gate hardware: 2,930 vertices baked against 25,694 drawn. The bound
    // is deliberately loose - it is here to catch a strip that goes back to drawing its
    // domes, not to pin a vertex count.
    REQUIRE (baked * 4 < vector);
    REQUIRE (baked < 6000);
}

TEST_CASE ("the shortcut rule withholds keys from a view that is taking text",
           "[imgui][widgets]")
{
    HeadlessImGui gui;
    dw::Context ctx;
    ctx.theme = &duskstudio::imgui::consolePalette().widgets;
    ctx.fonts = &gui.fonts;
    ctx.drag = &gui.drag;

    ImGui::NewFrame();
    REQUIRE (dw::shortcutsAvailable (ctx));

    ctx.textFieldOpen = true;
    REQUIRE (! dw::shortcutsAvailable (ctx));
    ImGui::Render();
}

TEST_CASE ("the console glyph set carries the marks the widgets draw", "[imgui][widgets]")
{
    const ImWchar* const ranges = dw::consoleGlyphRanges();
    const auto covers = [ranges] (ImWchar codepoint) {
        for (const ImWchar* p = ranges; p[0] != 0; p += 2)
            if (codepoint >= p[0] && codepoint <= p[1])
                return true;
        return false;
    };

    REQUIRE (covers (0x0041));  // Latin
    REQUIRE (covers (0x00d8));  // the phase button's slashed O
    REQUIRE (covers (0x00b0));  // degree
    REQUIRE (covers (0x221e));  // the fader's floor mark
    REQUIRE (covers (0x266f));  // sharp, for chord names
    REQUIRE (! covers (0x0410)); // Cyrillic belongs to the text entry face alone

    // Every pair ascends, and the set is terminated: Dear ImGui walks it as pairs and
    // reads past the end of a malformed one.
    int pairs = 0;
    for (const ImWchar* p = ranges; p[0] != 0; p += 2, ++pairs)
        REQUIRE (p[1] >= p[0]);
    REQUIRE (pairs > 0);

    const ImWchar* const entry = dw::textEntryGlyphRanges();
    const auto entryCovers = [entry] (ImWchar codepoint) {
        for (const ImWchar* p = entry; p[0] != 0; p += 2)
            if (codepoint >= p[0] && codepoint <= p[1])
                return true;
        return false;
    };
    REQUIRE (entryCovers (0x0410)); // Cyrillic
    REQUIRE (entryCovers (0x05d0)); // Hebrew
    REQUIRE (entryCovers (0x221e));
}

TEST_CASE ("the kit's skewed range matches the shape a JUCE slider has", "[imgui][widgets]")
{
    const auto range = dw::Range::withMidPoint (-90.0f, 6.0f, -12.0f);

    REQUIRE_THAT (range.toNorm (-90.0f), WithinAbs (0.0f, 1.0e-6f));
    REQUIRE_THAT (range.toNorm (6.0f), WithinAbs (1.0f, 1.0e-6f));
    REQUIRE_THAT (range.toNorm (-12.0f), WithinAbs (0.5f, 1.0e-4f));
    REQUIRE_THAT (range.fromNorm (range.toNorm (-24.0f)), WithinAbs (-24.0f, 1.0e-3f));

    // Out of range clamps rather than extrapolating.
    REQUIRE_THAT (range.toNorm (30.0f), WithinAbs (1.0f, 1.0e-6f));
    REQUIRE_THAT (range.fromNorm (-1.0f), WithinAbs (-90.0f, 1.0e-6f));
}

TEST_CASE ("meter ballistics attack instantly and release over time", "[imgui][widgets]")
{
    dw::MeterBallistics meter;

    meter.tick (-6.0f, 2.0f);
    REQUIRE_THAT (meter.displayed, WithinAbs (-6.0f, 1.0e-6f));
    REQUIRE_THAT (meter.peakHold, WithinAbs (-6.0f, 1.0e-6f));

    for (int i = 0; i < 30; ++i)
        meter.tick (-40.0f, 2.0f);

    REQUIRE (meter.displayed < -6.0f);
    REQUIRE (meter.displayed > -40.0f);
    // The peak is still held: the hold is eighteen source ticks.
    REQUIRE_THAT (meter.peakHold, WithinAbs (-6.0f, 1.0e-6f));

    for (int i = 0; i < 200; ++i)
        meter.tick (-40.0f, 2.0f);
    REQUIRE (meter.peakHold < -6.0f);
}

TEST_CASE ("the kit formats values one way for every view", "[imgui][widgets]")
{
    char buffer[32];

    dw::formatFrequency (buffer, sizeof buffer, 8000.0f);
    REQUIRE (std::string (buffer) == "8.0k");
    dw::formatFrequency (buffer, sizeof buffer, 20000.0f);
    REQUIRE (std::string (buffer) == "20k");
    dw::formatFrequency (buffer, sizeof buffer, 600.0f);
    REQUIRE (std::string (buffer) == "600");

    dw::formatGain (buffer, sizeof buffer, 3.0f);
    REQUIRE (std::string (buffer) == "+3");
    dw::formatGain (buffer, sizeof buffer, -1.5f);
    REQUIRE (std::string (buffer) == "-1.5");

    dw::formatDecibels (buffer, sizeof buffer, 0.0f);
    REQUIRE (std::string (buffer) == "0.0");
    dw::formatDecibels (buffer, sizeof buffer, -90.0f);
    REQUIRE (std::string (buffer) == "\xe2\x88\x9e");
}

TEST_CASE ("colour helpers reproduce the look-and-feel's own shades", "[imgui][widgets]")
{
    const ImU32 gold = IM_COL32 (0xd0, 0x90, 0x60, 0xff);

    // JUCE's Colour::brighter(0.25) on the same value, truncation and all.
    REQUIRE (dw::brighter (gold, 0.25f) == IM_COL32 (0xda, 0xa7, 0x80, 0xff));
    // ::darker(0.5) halves each channel.
    REQUIRE (dw::darker (gold, 1.0f) == IM_COL32 (0x68, 0x48, 0x30, 0xff));
    // Alpha scales, colour does not.
    REQUIRE (dw::withAlpha (gold, 0.5f) == IM_COL32 (0xd0, 0x90, 0x60, 0x7f));
    REQUIRE (dw::lerpColour (IM_COL32 (0, 0, 0, 255), IM_COL32 (255, 255, 255, 255), 0.5f)
             == IM_COL32 (127, 127, 127, 255));
}
