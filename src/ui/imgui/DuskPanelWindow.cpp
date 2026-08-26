#include "DuskPanelWindow.h"
#include "DuskImGuiHost.h"
#include "DuskTheme.h"
#include "../../foundation/Fs.h"
#include "../../foundation/MessageThread.h"

#include <DearImGui.hpp>
#include <OpenGL.hpp>
#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstdlib>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// EmbeddedModal's Backdrop, in the framework's colour order. A ported panel has to
// land in the same plate the JUCE modals draw, or the family stops reading as one.
constexpr float kPlateMargin = 6.0f;
constexpr float kPlateRounding = 8.0f;
constexpr unsigned int kPlateFill = 0x202024ffu;
constexpr unsigned int kPlateBorder = 0x3a3a42ffu;

ImU32 rgba (unsigned int hex)
{
    return IM_COL32 ((hex >> 24) & 0xff, (hex >> 16) & 0xff, (hex >> 8) & 0xff, hex & 0xff);
}

struct ShortcutBinding
{
    ImGuiKey key;
    ShellShortcut shortcut;
};

// The bare keys EmbeddedModal forwards. A modifier chord is never one of them: the
// shell binds Ctrl+R and Cmd+. to entirely different things, and reading a chord as
// its unmodified key would fire the wrong one from behind a panel.
const std::array<ShortcutBinding, 9>& shortcutBindings()
{
    static const std::array<ShortcutBinding, 9> bindings { {
        { ImGuiKey_Space, ShellShortcut::playStop },
        { ImGuiKey_R, ShellShortcut::record },
        { ImGuiKey_Home, ShellShortcut::playheadToZero },
        { ImGuiKey_Period, ShellShortcut::stopAndRewind },
        { ImGuiKey_L, ShellShortcut::toggleLoop },
        { ImGuiKey_P, ShellShortcut::togglePunch },
        { ImGuiKey_LeftBracket, ShellShortcut::setLoopIn },
        { ImGuiKey_RightBracket, ShellShortcut::setLoopOut },
        { ImGuiKey_F11, ShellShortcut::toggleFullscreen },
    } };
    return bindings;
}

// A native panel renders into its own framework child, which the JUCE screenshot
// harness cannot reach through createComponentSnapshot. The application reads its
// own frame back instead, the way the gate spike's --capture does, so the manual's
// figures for a ported panel come from the same run as everything else.
//
// PPM because it needs no encoder; the capture script converts.
bool writePpm (const std::string& path, int width, int height,
               const std::vector<unsigned char>& rgb)
{
    std::FILE* const file = std::fopen (path.c_str(), "wb");
    if (file == nullptr)
        return false;
    std::fprintf (file, "P6\n%d %d\n255\n", width, height);
    const bool ok = std::fwrite (rgb.data(), 1, rgb.size(), file) == rgb.size();
    std::fclose (file);
    return ok;
}

std::filesystem::path firstFrameMarkerPath (const std::string& logTag)
{
    const auto cfg = dusk::fs::userConfigDir();
    if (cfg.empty())
        return {};
    return cfg / "Dusk Studio" / (logTag + "-first-frame");
}
} // namespace

bool operator!= (const DuskPanelWindow::Geometry& a, const DuskPanelWindow::Geometry& b)
{
    return a.x != b.x || a.y != b.y || a.width != b.width || a.height != b.height
        || a.scaleFactor < b.scaleFactor || b.scaleFactor < a.scaleFactor;
}

void AtlasImage::reserve (ImFontAtlas& atlas, const unsigned char* rgba, int width,
                          int height)
{
    source = nullptr;
    rect = -1;
    pixels = rgba;
    imageWidth = width;
    imageHeight = height;
    if (rgba == nullptr || width < 1 || height < 1)
        return;
    rect = atlas.AddCustomRectRegular (width, height);
}

void AtlasImage::rasterise (ImFontAtlas& atlas)
{
    if (rect < 0 || pixels == nullptr)
        return;
    const ImFontAtlasCustomRect* const packed = atlas.GetCustomRectByIndex (rect);
    if (packed == nullptr || ! packed->IsPacked())
        return;

    unsigned char* texturePixels = nullptr;
    int textureWidth = 0, textureHeight = 0;
    atlas.GetTexDataAsRGBA32 (&texturePixels, &textureWidth, &textureHeight);
    if (texturePixels == nullptr)
        return;

    for (int row = 0; row < imageHeight; ++row)
    {
        auto* const destination = reinterpret_cast<unsigned int*> (texturePixels)
                                + (packed->Y + row) * textureWidth + packed->X;
        const unsigned char* sourceRow = pixels
                                       + static_cast<std::size_t> (row) * static_cast<std::size_t> (imageWidth) * 4u;
        for (int column = 0; column < imageWidth; ++column, sourceRow += 4)
            destination[column] = IM_COL32 (sourceRow[0], sourceRow[1], sourceRow[2],
                                            sourceRow[3]);
    }

    atlas.CalcCustomRectUV (packed, &uv[0], &uv[1]);
    source = &atlas;
}

bool AtlasImage::ready() const noexcept
{
    return source != nullptr && source->TexID != ImTextureID();
}

ImTextureID AtlasImage::textureId() const noexcept
{
    return source != nullptr ? source->TexID : ImTextureID();
}

void AtlasImage::draw (ImDrawList& dl, ImVec2 tl, ImVec2 br) const
{
    if (! ready() || imageWidth < 1 || imageHeight < 1)
        return;

    const float boxW = br.x - tl.x;
    const float boxH = br.y - tl.y;
    const float aspect = static_cast<float> (imageWidth) / static_cast<float> (imageHeight);
    float drawW = boxW;
    float drawH = boxW / aspect;
    if (drawH > boxH)
    {
        drawH = boxH;
        drawW = boxH * aspect;
    }
    const ImVec2 at (tl.x + (boxW - drawW) * 0.5f, tl.y + (boxH - drawH) * 0.5f);
    dl.AddImage (textureId(), at, ImVec2 (at.x + drawW, at.y + drawH), uv[0], uv[1]);
}

struct DuskPanelWindow::Impl final : private dusk::Timer
{
    class PanelWidget final : public DGL::ImGuiTopLevelWidget
    {
    public:
        PanelWidget (DGL::Window& window, Impl& ownerRef)
            : DGL::ImGuiTopLevelWidget (window, 13.0f), owner (ownerRef) {}

    protected:
        void onImGuiDisplay() override
        {
            owner.draw (static_cast<float> (getWidth()), static_cast<float> (getHeight()),
                        static_cast<float> (getWindow().getScaleFactor()));
        }

        void onDisplay() override
        {
            DGL::ImGuiTopLevelWidget::onDisplay();
            owner.captureFrameIfAsked (static_cast<int> (getWidth()),
                                       static_cast<int> (getHeight()));
        }

    private:
        Impl& owner;
    };

    Impl (std::string className, std::string logTag, std::string displayName)
        : host ({ std::move (className), logTag, std::move (displayName) },
                firstFrameMarkerPath (logTag))
    {
    }

    ~Impl() override { stopTimer(); }

    void startGeometryPolling() { startTimer (16); }
    void stopGeometryPolling() { stopTimer(); }

    void timerCallback() override
    {
        if (! callbacks.geometry || ! host.isOpen())
            return;
        const auto wanted = callbacks.geometry();
        if (wanted != lastGeometry)
        {
            lastGeometry = wanted;
            host.setGeometry ({ wanted.x, wanted.y, wanted.width, wanted.height,
                                wanted.scaleFactor });
        }
    }

    // Frame 30 rather than the first: the meter ballistics and any smoother have
    // settled by then, so two runs of the same panel produce the same picture.
    void captureFrameIfAsked (int width, int height)
    {
        if (capturePath.empty() || width < 1 || height < 1)
            return;
        if (++framesDrawn < 30)
            return;

        std::vector<unsigned char> rgb (static_cast<std::size_t> (width * height * 3));
        glReadPixels (0, 0, width, height, GL_RGB, GL_UNSIGNED_BYTE, rgb.data());

        // GL reads bottom-up.
        std::vector<unsigned char> flipped (rgb.size());
        const std::size_t stride = static_cast<std::size_t> (width) * 3;
        for (int row = 0; row < height; ++row)
            std::copy (rgb.begin() + static_cast<std::ptrdiff_t> (stride * static_cast<std::size_t> (height - 1 - row)),
                       rgb.begin() + static_cast<std::ptrdiff_t> (stride * static_cast<std::size_t> (height - row)),
                       flipped.begin() + static_cast<std::ptrdiff_t> (stride * static_cast<std::size_t> (row)));

        if (! writePpm (capturePath, width, height, flipped))
            host.log ("could not write the capture to %s", capturePath.c_str());
        capturePath.clear();
    }

    void buildFonts (float scale)
    {
       #ifndef DGL_NO_SHARED_RESOURCES
        auto& io = ImGui::GetIO();
        io.Fonts->Clear();
        fonts = dw::buildFonts (*io.Fonts, daf_resources::dejavusans_ttf,
                                static_cast<int> (daf_resources::dejavusans_ttf_size), scale);
        // Reserved before the pack and filled in after, so a knob costs three quads
        // rather than the thousand vertices a drawn dome does.
        knobAtlas.reserve (*io.Fonts, static_cast<int> (128.0f * std::max (1.0f, scale)));
        if (view != nullptr)
            view->reserveAtlasImages (*io.Fonts);
        io.FontDefault = fonts.band;
        io.Fonts->Build();
        knobAtlas.rasterise (*io.Fonts);
        if (view != nullptr)
            view->rasteriseAtlasImages (*io.Fonts);
       #endif
    }

    void draw (float width, float height, float scale)
    {
        if (view == nullptr)
            return;

        ImGui::SetNextWindowPos (ImVec2 (0.0f, 0.0f));
        ImGui::SetNextWindowSize (ImVec2 (width, height));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowPadding, ImVec2 (0.0f, 0.0f));
        ImGui::PushStyleVar (ImGuiStyleVar_WindowBorderSize, 0.0f);
        ImGui::PushStyleColor (ImGuiCol_WindowBg, ImVec4 (0.0f, 0.0f, 0.0f, 0.0f));
        ImGui::Begin ("##panel", nullptr,
                      ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoResize
                      | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoScrollbar
                      | ImGuiWindowFlags_NoScrollWithMouse
                      | ImGuiWindowFlags_NoBringToFrontOnFocus
                      | ImGuiWindowFlags_NoSavedSettings);

        dw::Context ctx;
        ctx.dl = ImGui::GetWindowDrawList();
        ctx.theme = &consolePalette().widgets;
        ctx.fonts = &fonts;
        ctx.knobAtlas = &knobAtlas;
        ctx.drag = &drag;
        ctx.scale = scale;

        // The child is the plate: the frame runs along its own edges and the body
        // sits inside the margin. The shadow the JUCE backdrop cast has nowhere to
        // fall here - it would land on the plate itself - so the frame carries the
        // panel on its own.
        const float margin = kPlateMargin * scale;
        const float rounding = kPlateRounding * scale;
        ctx.dl->AddRectFilled (ImVec2 (0.0f, 0.0f), ImVec2 (width, height),
                               rgba (kPlateFill), rounding);
        ctx.dl->AddRect (ImVec2 (0.5f, 0.5f), ImVec2 (width - 0.5f, height - 0.5f),
                         rgba (kPlateBorder), rounding, 0, scale);

        const ImVec2 bodyTl (margin, margin);
        const ImVec2 body (std::max (1.0f, width - margin * 2.0f),
                           std::max (1.0f, height - margin * 2.0f));

        view->draw (ctx, bodyTl, body);
        dw::drawDragBubble (ctx);

        // A click outside the panel lands on the host's dim overlay rather than
        // here, because the child is exactly the plate.
        if (view->takeDismissRequest())
            requestDismiss();
        else if (view->escapeDismisses() && dw::shortcutsAvailable (ctx)
                 && ImGui::IsKeyPressed (ImGuiKey_Escape, false))
            requestDismiss();

        forwardShortcuts (ctx);

        ImGui::End();
        ImGui::PopStyleColor();
        ImGui::PopStyleVar (2);
    }

    void forwardShortcuts (const dw::Context& ctx)
    {
        if (! callbacks.shortcut || ! dw::shortcutsAvailable (ctx))
            return;

        const auto& io = ImGui::GetIO();
        if (io.KeyCtrl || io.KeySuper || io.KeyAlt)
            return;

        for (const auto& binding : shortcutBindings())
        {
            if (! ImGui::IsKeyPressed (binding.key, false))
                continue;
            // Shift turns the bracket keys into the punch pair, the way the shell's
            // own bindings read the shifted glyph.
            auto shortcut = binding.shortcut;
            if (io.KeyShift)
            {
                if (shortcut == ShellShortcut::setLoopIn)
                    shortcut = ShellShortcut::setPunchIn;
                else if (shortcut == ShellShortcut::setLoopOut)
                    shortcut = ShellShortcut::setPunchOut;
                else
                    continue;
            }
            if (view != nullptr && view->claimsShortcut (shortcut))
                continue;
            callbacks.shortcut (shortcut);
        }
    }

    void requestDismiss()
    {
        if (auto callback = callbacks.dismissed)
            callback();
    }

    // Declared last so the host - whose teardown reaches back through the callbacks
    // below - is destroyed before the state those callbacks touch.
    Callbacks callbacks;
    std::unique_ptr<DuskPanelView> view;
    dw::Fonts fonts;
    dw::KnobAtlas knobAtlas;
    dw::DragState drag;
    Geometry lastGeometry;
    std::string capturePath;
    int framesDrawn = 0;
    DuskImGuiHost host;
};

DuskPanelWindow::DuskPanelWindow (std::string className, std::string logTag,
                                  std::string displayName)
    : impl (new Impl (std::move (className), std::move (logTag), std::move (displayName)))
{
    DuskImGuiHost::Callbacks callbacks;
    callbacks.createWidget = [this] (DGL::Window& window) -> std::unique_ptr<DGL::TopLevelWidget>
    {
        auto widget = std::unique_ptr<Impl::PanelWidget> (new Impl::PanelWidget (window, *impl));
        impl->buildFonts (static_cast<float> (window.getScaleFactor()));
        return std::unique_ptr<DGL::TopLevelWidget> (widget.release());
    };
    callbacks.checkGraphics = [] (const char*, const char*) { return std::string(); };
    callbacks.widgetReleased = [this]
    {
        // The fonts and the baked dome live in the atlas the widget owned.
        impl->fonts = {};
        impl->knobAtlas = {};
        impl->drag = {};
    };
    callbacks.closed = [this]
    {
        impl->stopGeometryPolling();
        impl->view.reset();
        if (auto callback = impl->callbacks.closed)
            callback();
    };
    impl->host.setCallbacks (std::move (callbacks));
}

DuskPanelWindow::~DuskPanelWindow() = default;

void DuskPanelWindow::setCallbacks (Callbacks callbacks)
{
    impl->callbacks = std::move (callbacks);
}

void DuskPanelWindow::setView (std::unique_ptr<DuskPanelView> view)
{
    impl->view = std::move (view);
}

DuskPanelWindow::PlateSize DuskPanelWindow::plateSize() const
{
    if (impl->view == nullptr)
        return {};
    const auto body = impl->view->preferredSize();
    const auto frame = static_cast<int> (kPlateMargin) * 2;
    return { static_cast<int> (body.x) + frame, static_cast<int> (body.y) + frame };
}

float DuskPanelWindow::dimAlpha() const
{
    return impl->view != nullptr ? impl->view->dimAlpha() : 0.55f;
}

void DuskPanelWindow::captureNextFrameTo (std::string path)
{
    impl->capturePath = std::move (path);
    impl->framesDrawn = 0;
}

bool DuskPanelWindow::open (std::uintptr_t nativeParent, Geometry geometry)
{
    impl->lastGeometry = geometry;
    impl->framesDrawn = 0;
    if (! impl->host.open (nativeParent, { geometry.x, geometry.y, geometry.width,
                                           geometry.height, geometry.scaleFactor }))
        return false;

    impl->startGeometryPolling();
    return true;
}

const std::string& DuskPanelWindow::lastOpenFailure() const noexcept
{
    return impl->host.lastOpenFailure();
}

void DuskPanelWindow::setGeometry (Geometry geometry)
{
    impl->host.setGeometry ({ geometry.x, geometry.y, geometry.width, geometry.height,
                              geometry.scaleFactor });
}

void DuskPanelWindow::close()
{
    impl->host.close();
}

bool DuskPanelWindow::isOpen() const noexcept
{
    return impl->host.isOpen();
}

} // namespace duskstudio::imgui
