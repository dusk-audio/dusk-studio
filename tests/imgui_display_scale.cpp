#include <catch2/catch_test_macros.hpp>

#include "ui/imgui/DuskTheme.h"
#include "ui/imgui/PanelControls.h"
#include "ui/imgui/StartupView.h"

#include <DuskWidgets.hpp>

#include <algorithm>
#include <functional>
#include <string>
#include <vector>

// Text drawn through the widget kit on a Retina display, with no window, no GL and no
// compositor. The kit scales a style's font size itself, so a caller that hands it
// pixels gets the size squared: twice as large at a backing scale of 2. What is
// asserted is the height of the glyph quads a frame emits, against the size the text
// was designed at.

namespace dw = DuskWidgets;
using namespace duskstudio::imgui;

namespace
{
// The atlas is baked at the display scale, the way DuskPanelWindow bakes it, so a
// face's FontSize is in physical pixels exactly as it is in the app.
class ScaledImGui
{
public:
    explicit ScaledImGui (float displayScale) : scale (displayScale)
    {
        context = ImGui::CreateContext();
        ImGui::SetCurrentContext (context);

        auto& io = ImGui::GetIO();
        io.DisplaySize = ImVec2 (900.0f * scale, 600.0f * scale);
        io.DeltaTime = 1.0f / 60.0f;
        io.IniFilename = nullptr;
        io.LogFilename = nullptr;
        // Anti-aliased lines would otherwise sample the atlas too, and only glyphs may
        // carry a UV other than the white pixel for the measurement below to hold.
        io.Fonts->Flags |= ImFontAtlasFlags_NoBakedLines;

        ImFontConfig config;
        config.SizePixels = kDesignFontSize * scale;
        io.Fonts->AddFontDefault (&config);
        io.Fonts->Build();
        io.Fonts->SetTexID (static_cast<ImTextureID> (1));

        ImFont* const font = io.Fonts->Fonts.front();
        fonts.caption = fonts.label = fonts.pill = fonts.band = font;
        fonts.title = fonts.value = fonts.valueLarge = fonts.textEntry = font;
    }

    ~ScaledImGui() { ImGui::DestroyContext (context); }

    struct Glyph
    {
        ImVec2 tl;
        ImVec2 br;
    };

    // One frame; returns every glyph quad `body` drew.
    std::vector<Glyph> frame (const std::function<void (dw::Context&)>& body)
    {
        ImGui::NewFrame();
        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImGui::GetIO().DisplaySize);
        ImGui::Begin ("##scaled", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar);

        dw::Context ctx;
        ctx.dl = ImGui::GetWindowDrawList();
        ctx.theme = &consolePalette().widgets;
        ctx.fonts = &fonts;
        ctx.drag = &drag;
        ctx.scale = scale;

        const int before = ctx.dl->VtxBuffer.Size;
        body (ctx);

        // A glyph is written as four consecutive vertices, and nothing else in these
        // frames samples anything but the white pixel.
        const ImVec2 white = ImGui::GetIO().Fonts->TexUvWhitePixel;
        std::vector<ImDrawVert> textured;
        for (int i = before; i < ctx.dl->VtxBuffer.Size; ++i)
        {
            const auto& v = ctx.dl->VtxBuffer[i];
            if (v.uv.x < white.x || white.x < v.uv.x || v.uv.y < white.y || white.y < v.uv.y)
                textured.push_back (v);
        }

        std::vector<Glyph> glyphs;
        for (std::size_t q = 0; q + 3 < textured.size(); q += 4)
        {
            Glyph g { textured[q].pos, textured[q].pos };
            for (std::size_t k = q; k < q + 4; ++k)
            {
                g.tl.x = std::min (g.tl.x, textured[k].pos.x);
                g.tl.y = std::min (g.tl.y, textured[k].pos.y);
                g.br.x = std::max (g.br.x, textured[k].pos.x);
                g.br.y = std::max (g.br.y, textured[k].pos.y);
            }
            glyphs.push_back (g);
        }

        ImGui::End();
        ImGui::Render();
        return glyphs;
    }

    static constexpr float kDesignFontSize = 13.0f;
    const float scale;

private:
    ImGuiContext* context = nullptr;
    dw::Fonts fonts;
    dw::DragState drag;
};

float tallest (const std::vector<ScaledImGui::Glyph>& glyphs)
{
    float height = 0.0f;
    for (const auto& g : glyphs)
        height = std::max (height, g.br.y - g.tl.y);
    return height;
}

float formButtonGlyphHeight (float scale)
{
    ScaledImGui gui (scale);
    const auto glyphs = gui.frame ([scale] (dw::Context& ctx)
    {
        const ScopedFormStyle style (ctx);
        formButton (ctx, "##rescan", ImVec2 (20.0f * scale, 20.0f * scale),
                    ImVec2 (180.0f * scale, 46.0f * scale), "Rescan devices");
    });
    REQUIRE (! glyphs.empty());
    return tallest (glyphs);
}

// The sidebar's tallest text is a tab label; the wordmark above the tabs is smaller.
float startupTabGlyphHeight (float scale)
{
    ScaledImGui gui (scale);
    auto view = makeStartupView ({}, { "Blank" }, nullptr, 0, 0, {});
    const auto size = view->preferredSize();
    const auto glyphs = gui.frame ([&] (dw::Context& ctx)
    {
        view->draw (ctx, ImVec2 (0.0f, 0.0f), ImVec2 (size.x * scale, size.y * scale));
    });

    constexpr float kSidebarW = 100.0f;
    std::vector<ScaledImGui::Glyph> sidebar;
    for (const auto& g : glyphs)
        if (g.br.x <= kSidebarW * scale)
            sidebar.push_back (g);
    REQUIRE (! sidebar.empty());
    return tallest (sidebar);
}
} // namespace

TEST_CASE ("a form button's label is its font's size on a Retina display", "[imgui][scale]")
{
    const float atOne = formButtonGlyphHeight (1.0f);
    const float atTwo = formButtonGlyphHeight (2.0f);

    // No glyph is taller than the face it was drawn with. Drawn at the size squared,
    // the tallest one is about twice that.
    REQUIRE (atTwo <= ScaledImGui::kDesignFontSize * 2.0f * 1.05f);
    REQUIRE (atTwo >= atOne * 1.8f);
    REQUIRE (atTwo <= atOne * 2.2f);
}

TEST_CASE ("a startup tab label is its design size on a Retina display", "[imgui][scale]")
{
    constexpr float kTabFontSize = 11.0f;
    const float atOne = startupTabGlyphHeight (1.0f);
    const float atTwo = startupTabGlyphHeight (2.0f);

    REQUIRE (atTwo <= kTabFontSize * 2.0f * 1.05f);
    REQUIRE (atTwo >= atOne * 1.8f);
    REQUIRE (atTwo <= atOne * 2.2f);
}
