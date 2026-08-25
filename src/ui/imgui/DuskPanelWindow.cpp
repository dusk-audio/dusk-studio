#include "DuskPanelWindow.h"
#include "DuskImGuiHost.h"
#include "DuskTheme.h"
#include "../../foundation/Fs.h"

#include <DearImGui.hpp>
#ifndef DGL_NO_SHARED_RESOURCES
# include "src/Resources.hpp"
#endif

#include <algorithm>
#include <array>
#include <cmath>
#include <utility>

namespace duskstudio::imgui
{
namespace
{
namespace dw = DuskWidgets;

// EmbeddedModal's Backdrop, in the framework's colour order. A ported panel has to
// land in the same plate the JUCE modals draw, or the family stops reading as one.
constexpr float kPlateMargin = 6.0f;
constexpr float kPlateRounding = 8.0f;
constexpr unsigned int kPlateShadow = 0x141418ffu;
constexpr unsigned int kPlateFill = 0x202024ffu;
constexpr unsigned int kPlateBorder = 0x3a3a42ffu;

// EmbeddedModal centres a body inside the host with this much slack at each edge, and
// shrinks it rather than letting it run off. Matching it keeps a panel in the same
// place it opened before the port.
constexpr float kHostSlack = 16.0f;

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

std::filesystem::path firstFrameMarkerPath (const std::string& logTag)
{
    const auto cfg = dusk::fs::userConfigDir();
    if (cfg.empty())
        return {};
    return cfg / "Dusk Studio" / (logTag + "-first-frame");
}
} // namespace

struct DuskPanelWindow::Impl final
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

    private:
        Impl& owner;
    };

    Impl (std::string className, std::string logTag, std::string displayName)
        : host ({ std::move (className), logTag, std::move (displayName) },
                firstFrameMarkerPath (logTag))
    {
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
        io.FontDefault = fonts.band;
        io.Fonts->Build();
        knobAtlas.rasterise (*io.Fonts);
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

        const float dim = std::clamp (view->dimAlpha(), 0.0f, 1.0f);
        ctx.dl->AddRectFilled (ImVec2 (0.0f, 0.0f), ImVec2 (width, height),
                               IM_COL32 (0, 0, 0, static_cast<int> (dim * 255.0f + 0.5f)));

        const auto preferred = view->preferredSize();
        const ImVec2 body (std::min (preferred.x * scale, std::max (1.0f, width - kHostSlack * scale)),
                           std::min (preferred.y * scale, std::max (1.0f, height - kHostSlack * scale)));
        const ImVec2 bodyTl (std::round ((width - body.x) * 0.5f),
                             std::round ((height - body.y) * 0.5f));
        const ImVec2 bodyBr (bodyTl.x + body.x, bodyTl.y + body.y);

        const float margin = kPlateMargin * scale;
        const float rounding = kPlateRounding * scale;
        const ImVec2 plateTl (bodyTl.x - margin, bodyTl.y - margin);
        const ImVec2 plateBr (bodyBr.x + margin, bodyBr.y + margin);
        ctx.dl->AddRectFilled (ImVec2 (plateTl.x, plateTl.y + 4.0f * scale),
                               ImVec2 (plateBr.x, plateBr.y + 4.0f * scale),
                               dw::withAlpha (rgba (kPlateShadow), 0.55f), rounding);
        ctx.dl->AddRectFilled (plateTl, plateBr, rgba (kPlateFill), rounding);
        ctx.dl->AddRect (ImVec2 (plateTl.x + 0.5f, plateTl.y + 0.5f),
                         ImVec2 (plateBr.x - 0.5f, plateBr.y - 0.5f),
                         rgba (kPlateBorder), rounding, 0, scale);

        view->draw (ctx, bodyTl, body);
        dw::drawDragBubble (ctx);

        // The plate frame is part of the panel, not the backdrop: a click on the
        // rounded ring around the body must not read as a dismissal.
        const bool pointerOutside = ! ImGui::IsMouseHoveringRect (plateTl, plateBr, false);
        if (ImGui::IsMouseClicked (ImGuiMouseButton_Left) && pointerOutside)
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

bool DuskPanelWindow::open (std::uintptr_t nativeParent, Geometry geometry)
{
    return impl->host.open (nativeParent, { geometry.x, geometry.y, geometry.width,
                                            geometry.height, geometry.scaleFactor });
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

std::string DuskPanelWindow::portalParentHandle() const
{
    if (auto* const window = impl->host.window())
        if (const auto* const handle = window->getPortalParentHandle())
            return handle;
    return {};
}
} // namespace duskstudio::imgui
