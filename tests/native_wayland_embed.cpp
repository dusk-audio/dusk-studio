#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_gui_extra/juce_gui_extra.h>
#include "../src/ui/imgui/DuskImGuiHost.h"
#include "../src/ui/imgui/DafEditorHost.h"
#include "../src/ui/imgui/DuskPanelWindow.h"
#include "../src/ui/imgui/SharedWaylandApplication.h"
#include "../src/ui/PlatformWindowing.h"
#include "../src/ui/EmbeddedModal.h"
#include "TestTempDirectory.h"

#include <OpenGL.hpp>

#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <functional>
#include <iterator>
#include <stdexcept>
#include <utility>

namespace
{
using duskstudio::imgui::DuskImGuiHost;
using duskstudio::imgui::DafEditorHost;
namespace builtin = duskstudio::builtin;
using duskstudio::imgui::SharedWaylandApplication;
namespace platform = duskstudio::platform;

void require (bool ok, const char* message)
{
    if (! ok) throw std::runtime_error (message);
}

class ForeignEditorProbe
{
public:
    explicit ForeignEditorProbe (juce::Component& parent) : shell (parent), shellBounds (parent.getBounds())
    {
        if (std::getenv ("DUSK_NATIVE_PROBE_X11") == nullptr)
        {
            juce::Component refused;
            require (! modal.showBorrowed (shell, refused, {}, {}, presentation), "foreign editor opened without X11");
            require (! modal.isOpen() && refused.getParentComponent() == nullptr, "refused foreign editor changed the layout");
            std::puts ("PASS foreign editor refusal: native-only session remains usable");
            return;
        }
        client = std::make_unique<ForeignClient>();
        platform::withX11PeerCreation ([&]
        {
            body = std::make_unique<juce::XEmbedComponent> (client->window, true, false);
        });
        body->setSize (240, 160);
        try
        {
            show();
            window = body->getTopLevelComponent();
            peer = window->getPeer();
            require (peer != nullptr && peer != shell.getPeer(), "foreign editor reused the Wayland peer");
            const auto native = platform::describeEmbeddedParent (reinterpret_cast<std::uintptr_t> (peer->getNativeHandle()));
            require (native.backend == platform::NativeEmbeddedParent::Backend::platform, "foreign editor parent is not X11");
        }
        catch (...)
        {
            modal.closeAndDeleteBodyNow();
            throw;
        }
    }

    ~ForeignEditorProbe()
    {
        cover.closeAndDeleteBodyNow();
        modal.closeAndDeleteBodyNow();
    }

    bool advance()
    {
        if (client == nullptr) return true;
        switch (stage)
        {
            case 0:
            {
                if (! client->isViewable()) return false;
                const auto bounds = window->getBounds();
                shell.setSize (shell.getWidth() + 10, shell.getHeight() + 10);
                require (window->getBounds() == bounds && body->getPeer() == peer, "main resize moved the separate editor");
                for (auto* child : shell.getChildren())
                    if (dynamic_cast<duskstudio::DimOverlay*> (child) != nullptr)
                        require (child->getBounds() == shell.getLocalBounds(), "foreign editor dim missed the main resize");
                body->setSize (320, 200);
                require (window->getWidth() == 332 && window->getHeight() == 212, "foreign editor resize missed its shell");
                auto coverBody = std::make_unique<juce::Component>();
                coverBody->setSize (300, 180);
                cover.show (shell, std::move (coverBody));
                require (! window->isVisible(), "foreign editor covered a main-window dialog");
                break;
            }
            case 1:
                if (client->isViewable()) return false;
                cover.close();
                require (window->isVisible(), "foreign editor did not return after the dialog");
                break;
            case 2:
                if (! client->isViewable()) return false;
                modal.close();
                require (! window->isVisible() && body->getPeer() == peer && dismissals == 0,
                         "closing the foreign editor lost its peer or dispatched dismissal");
                break;
            case 3:
                if (client->isViewable()) return false;
                show();
                require (body->getPeer() == peer, "reopening replaced the foreign editor parent");
                break;
            case 4:
                if (! client->isViewable()) return false;
                window->userTriedToCloseWindow();
                require (! modal.isOpen() && dismissals == 1, "native close did not dismiss the foreign editor");
                break;
            case 5:
                if (client->isViewable()) return false;
                shell.setBounds (shellBounds);
                std::puts ("PASS foreign X11 editors: separate parent, resize, dialog cover, retained peer, reopen, native close");
                return true;
        }
        ++stage;
        return false;
    }

private:
    struct ForeignClient
    {
        ::Display* display = ::XOpenDisplay (nullptr);
        ::Window window = 0;
        ForeignClient()
        {
            require (display != nullptr, "Xwayland test connection failed");
            window = ::XCreateSimpleWindow (display, DefaultRootWindow (display), 0, 0, 240, 160, 0, 0, 0x4080a0);
            ::XMapWindow (display, window);
            ::XSync (display, False);
        }
        ~ForeignClient()
        {
            ::XDestroyWindow (display, window);
            ::XCloseDisplay (display);
        }
        bool isViewable()
        {
            platform::flushWindowOperations();
            ::XWindowAttributes attributes {};
            require (::XGetWindowAttributes (display, window, &attributes) != 0, "foreign X11 child was destroyed");
            return attributes.map_state == IsViewable;
        }
    };

    void show()
    {
        require (modal.showBorrowed (shell, *body, [this] { ++dismissals; modal.close(); }, {}, presentation),
                 "foreign editor presentation failed");
    }

    static constexpr auto presentation = duskstudio::EmbeddedModal::BorrowedPresentation::foreignPlugin;
    juce::Component& shell;
    juce::Rectangle<int> shellBounds;
    duskstudio::EmbeddedModal modal;
    std::unique_ptr<ForeignClient> client;
    std::unique_ptr<juce::XEmbedComponent> body;
    duskstudio::EmbeddedModal cover;
    juce::Component* window = nullptr;
    juce::ComponentPeer* peer = nullptr;
    int dismissals = 0, stage = 0;
};

struct Painter final : DGL::TopLevelWidget, DGL::IdleCallback
{
    Painter (DGL::Window& window, std::function<void (DGL::Window&, unsigned)> key,
             std::function<void()> mouse)
        : TopLevelWidget (window), keyPressed (std::move (key)), mousePressed (std::move (mouse))
    {
        getWindow().addIdleCallback (this);
    }
    ~Painter() override { getWindow().removeIdleCallback (this); }
    void idleCallback() override { repaint(); }
    bool onKeyboard (const KeyboardEvent& event) override
    {
        if (event.press) keyPressed (getWindow(), event.key);
        return true;
    }
    bool onMouse (const MouseEvent& event) override
    {
        if (event.press) mousePressed();
        return true;
    }
    void onDisplay() override
    {
        glClearColor (0.2f, 0.4f, 0.6f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
    }
    std::function<void (DGL::Window&, unsigned)> keyPressed;
    std::function<void()> mousePressed;
};

struct FontSnapshot
{
    float scale = 0.0f, pixels = 0.0f;
    unsigned frames = 0;
};

struct FontView final : duskstudio::imgui::DuskPanelView
{
    explicit FontView (FontSnapshot& result) : snapshot (result) {}
    ImVec2 preferredSize() const override { return { 300, 200 }; }
    bool wantsPlate() const override { return false; }
    void draw (DuskWidgets::Context& context, ImVec2 origin, ImVec2) override
    {
        auto* const font = context.fonts->textEntry;
        snapshot.scale = context.scale;
        snapshot.pixels = font != nullptr ? font->FontSize : 0.0f;
        ++snapshot.frames;
        if (font != nullptr)
            context.dl->AddText (font, font->FontSize, origin, IM_COL32_WHITE, "Native font scaling");
    }
    FontSnapshot& snapshot;
};

struct Shell final : juce::Component
{
    ~Shell() override
    {
        if (frame != nullptr) juce::WaylandSymbols::getInstance()->callbackDestroy (frame);
    }

    void paint (juce::Graphics& graphics) override { graphics.fillAll (juce::Colours::darkgrey); }

    bool mapped() const
    {
        return juce::WaylandWindowSystem::getInstance()->isSurfaceMapped (
            static_cast<wl_surface*> (getPeer()->getNativeHandle()));
    }

    void watchFrame()
    {
        require (frame == nullptr, "shell frame callback is already pending");
        auto* host = juce::WaylandWindowSystem::getInstance();
        host->updateConstraints (host->getWaylandWindowForPeer (getPeer()));
        static const wl_callback_listener listener {
            [] (void* data, wl_callback* callback, std::uint32_t)
            {
                auto& shell = *static_cast<Shell*> (data);
                juce::WaylandSymbols::getInstance()->callbackDestroy (callback);
                shell.frame = nullptr;
                ++shell.frames;
            }
        };
        frame = juce::WaylandSymbols::getInstance()->surfaceFrame (
            static_cast<wl_surface*> (getPeer()->getNativeHandle()));
        juce::WaylandSymbols::getInstance()->callbackAddListener (frame, &listener, this);
        repaint();
    }

    std::uint64_t frames = 0;
    wl_callback* frame = nullptr;
};

class Probe final : private juce::Timer
{
public:
    Probe()
    {
        juce::WaylandWindowSystem::setSkipForPeerCreation (false);
        platform::withX11PeerCreation ([]
        {
            require (juce::WaylandWindowSystem::shouldSkipForPeerCreation(), "X11 scope did not set the backend");
            platform::withX11PeerCreation ([] {});
            require (juce::WaylandWindowSystem::shouldSkipForPeerCreation(), "nested X11 scope lost the backend");
        });
        require (! juce::WaylandWindowSystem::shouldSkipForPeerCreation(), "X11 scope did not restore the backend");
        try
        {
            platform::withX11PeerCreation ([] { throw 1; });
        }
        catch (int) {}
        require (! juce::WaylandWindowSystem::shouldSkipForPeerCreation(), "failed X11 construction changed the backend");
        shell.setBounds (50, 50, 800, 600);
        shell.setOpaque (true);
        shell.addToDesktop (0);
        shell.setVisible (true);
        shell.setVisible (false);
        require (! shell.mapped(), "hidden shell is reported as mapped");
        require (! platform::bringWindowToFront (*shell.getPeer(), "probe-token"),
                 "activation consumed a token before mapping");
        require (platform::bringWindowToFront (*shell.getPeer()),
                 "activation without a token waits for mapping");
        require (! juce::WaylandWindowSystem::getInstance()->isSurfaceMapped (nullptr),
                 "unknown surface is reported as mapped");
        shell.setVisible (true);
        shell.watchFrame();
        DuskImGuiHost::Callbacks callbacks;
        callbacks.createWidget = [this] (DGL::Window& window) {
            return std::make_unique<Painter> (window,
                [this] (DGL::Window& target, unsigned key) { clipboardKey (target, key); },
                [this] { ++mousePresses; });
        };
        callbacks.closed = [this] { ++closed; };
        a.setCallbacks (callbacks);
        b.setCallbacks (callbacks);
        c.setCallbacks (callbacks);
        d.setCallbacks (callbacks);
        startTimer (20);
    }

    ~Probe() override
    {
        stopTimer();
       #if DUSK_NATIVE_PROBE_DAF
        clearDonors();
       #endif
    }

private:
    enum class Stage
    {
        open, rendered, firstClosed, resized, remapped, secondWindow, parentGone, failureReady, failed,
        fontMapping, fontRendered, fontScaled, fontRestored, fontClosed,
       #if DUSK_NATIVE_PROBE_DAF
        donorMapping, donorRendered, donorClosed, donorReopened, donorParentGone,
       #endif
        clipboardMapping, clipboardReady
    };

    std::uintptr_t parent() const
    {
        return reinterpret_cast<std::uintptr_t> (shell.getPeer()->getNativeHandle());
    }

    bool bothRendered() const
    {
        require (a.window() != nullptr && b.window() != nullptr, "a panel disappeared before rendering");
        return a.window()->getCompletedDisplayCount() > 0 && b.window()->getCompletedDisplayCount() > 0;
    }

    void timerCallback() override
    {
        try
        {
            require (std::chrono::steady_clock::now() < deadline, "native probe timed out");
           #if DUSK_NATIVE_PROBE_DAF
            for (auto& host : donorHosts) if (host) host->tick();
           #endif
            switch (stage)
            {
                case Stage::open:
                {
                    const auto descriptor = platform::describeEmbeddedParent (parent());
                    require (descriptor.backend == platform::NativeEmbeddedParent::Backend::wayland,
                             "shell is not native Wayland");
                    require (a.open (parent(), { 20, 20, 280, 160, 1.0 }), "first panel open failed");
                    require (b.open (parent(), { 320, 20, 280, 160, 1.0 }), "second panel open failed");
                    require (SharedWaylandApplication::acquire (descriptor)
                                 == SharedWaylandApplication::acquire (descriptor),
                             "panels did not reuse one application");
                    stage = Stage::rendered;
                    break;
                }
                case Stage::rendered:
                    if (! bothRendered() || shell.frames == 0) return;
                    require (shell.mapped(), "presented shell is not reported as mapped");
                    require (platform::bringWindowToFront (*shell.getPeer(), "probe-token"),
                             "activation retained a token after mapping");
                    require (! std::filesystem::exists (root.path() / "a-frame")
                                 && ! std::filesystem::exists (root.path() / "b-frame"),
                             "rendered panel retained its crash marker");
                    shell.setSize (850, 625);
                    shell.watchFrame();
                    siblingFrame = b.window()->getCompletedDisplayCount();
                    a.close();
                    require (! a.window()->isVisible(), "deferred close did not immediately hide the panel");
                    stage = Stage::firstClosed;
                    break;
                case Stage::firstClosed:
                {
                    require (b.isOpen(), "closing one panel closed its sibling");
                    if (a.isOpen() || b.window()->getCompletedDisplayCount() <= siblingFrame || shell.frames < 2) return;
                    require (shell.getWidth() == 850 && shell.getHeight() == 625, "mapped resize lost its requested size");
                    require (a.open (parent(), { 20, 220, 280, 160, 1.0 }), "panel reopen failed");
                    const auto handle = a.window()->getNativeWindowHandle();
                    a.setGeometry ({ 37, 220, 250, 180, 1.25 });
                    require (a.window()->getNativeWindowHandle() == handle, "native resize recreated the surface");
                    require (c.open (parent(), { 320, 220, 200, 180, 1.0 }), "third panel open failed");
                    require (c.window()->getCompletedDisplayCount() == 0, "third panel drew before cancellation check");
                    c.close();
                    stage = Stage::resized;
                    break;
                }
                case Stage::resized:
                {
                    require (a.window() != nullptr, "resizing closed the panel");
                    if (a.window()->getWidth() != 250 || a.window()->getHeight() != 180
                        || std::abs (a.window()->getScaleFactor() - 1.25) > 0.001 || c.isOpen()) return;
                    require (! std::filesystem::exists (root.path() / "c-frame"),
                             "cancel before first frame retained a crash marker");
                    originalParent = parent();
                    shell.setVisible (false);
                    shell.setVisible (false);
                    shell.setSize (900, 650);
                    shell.setVisible (true);
                    shell.setVisible (true);
                    shell.watchFrame();
                    stage = Stage::remapped;
                    break;
                }
                case Stage::remapped:
                    if (shell.frames < static_cast<std::uint64_t> (3 + visibilityCycles)) return;
                    require (parent() == originalParent, "hide/show recreated the parent surface");
                    require (shell.getWidth() == 900 + visibilityCycles * 10 && shell.getHeight() == 650,
                             "remapping lost a resize made while hidden");
                    if (++visibilityCycles < 3)
                    {
                        shell.setVisible (false);
                        shell.setSize (900 + visibilityCycles * 10, 650);
                        shell.setVisible (true);
                        shell.watchFrame();
                        return;
                    }
                    otherShell.setBounds (50, 50, 400, 300);
                    otherShell.setOpaque (true);
                    otherShell.addToDesktop (0);
                    otherShell.setVisible (true);
                    otherShell.watchFrame();
                    require (d.open (reinterpret_cast<std::uintptr_t> (otherShell.getPeer()->getNativeHandle()),
                                     { 20, 20, 280, 160, 1.0 }), "second shell panel open failed");
                    require (SharedWaylandApplication::acquire (platform::describeEmbeddedParent (parent()))
                                 == SharedWaylandApplication::acquire (platform::describeEmbeddedParent (
                                     reinterpret_cast<std::uintptr_t> (otherShell.getPeer()->getNativeHandle()))),
                             "two shells did not share one application");
                    stage = Stage::secondWindow;
                    break;
                case Stage::secondWindow:
                {
                    require (d.window() != nullptr, "second shell lost its panel");
                    if (otherShell.frames == 0 || d.window()->getCompletedDisplayCount() == 0) return;
                    const auto old = platform::describeEmbeddedParent (parent());
                    shell.removeFromDesktop();
                    shell.addToDesktop (0);
                    shell.setVisible (true);
                    require (! platform::isEmbeddedParentAlive (old), "old parent identity survived recreation");
                    stage = Stage::parentGone;
                    break;
                }
                case Stage::parentGone:
                    if (a.isOpen() || b.isOpen()) return;
                    require (d.isOpen(), "parent recreation closed another shell panel");
                    require (closed == 4, "unexpected parent-loss close notification count");
                    require (a.open (parent(), { 20, 20, 280, 160, 1.0 }), "first panel did not reopen on the new peer");
                    require (b.open (parent(), { 320, 20, 280, 160, 1.0 }), "second panel did not reopen on the new peer");
                    stage = Stage::failureReady;
                    break;
                case Stage::failureReady:
                    if (! bothRendered()) return;
                    failedApplication = SharedWaylandApplication::acquire (platform::describeEmbeddedParent (parent()));
                    fault = failedApplication->registerClient (platform::describeEmbeddedParent (parent()), {
                        [] { throw std::runtime_error ("injected shared pump failure"); }, {}, {},
                        [this] (const char*) { failureReported = true; }
                    });
                    stage = Stage::failed;
                    break;
                case Stage::failed:
                {
                    if (! failureReported) return;
                    require (! a.isOpen() && ! b.isOpen() && ! d.isOpen(), "shared pump failure left an editor alive");
                    require (closed == 7, "shared pump failure duplicated close notifications");
                    const auto replacement = SharedWaylandApplication::acquire (platform::describeEmbeddedParent (parent()));
                    require (replacement != nullptr && replacement != failedApplication, "failed world was reused");
                    fault.reset();
                    failedApplication.reset();
                    std::puts ("PASS native shared panels: render, hide/show, hidden resize, two shells, sibling lifetime, cancellation, parent recreation, pump failure");
                    if (exchange.empty())
                    {
                        finish (0);
                        break;
                    }
                    otherShell.setVisible (false);
                    shell.removeFromDesktop();
                    shell.addToDesktop (juce::ComponentPeer::windowIsResizable);
                    shell.setVisible (true);
                    shell.getPeer()->setFullScreen (true);
                    stage = Stage::fontMapping;
                    break;
                }
                case Stage::fontMapping:
                    if (shell.getWidth() < 1900 || shell.getHeight() < 1000) return;
                    fontPanel.setView (std::make_unique<FontView> (fontSnapshot));
                    require (fontPanel.open (parent(), { 50, 50, 300, 200, 1.0 }), "font panel open failed");
                    stage = Stage::fontRendered;
                    break;
                case Stage::fontRendered:
                    if (fontSnapshot.frames == 0) return;
                    require (std::abs (fontSnapshot.pixels - DuskWidgets::FontSizes {}.textEntry) < 0.01f,
                             "initial panel font size is incorrect");
                    fontHandle = fontPanel.nativeWindowHandleForScenario();
                    fontPanel.setGeometry ({ 50, 50, 400, 250, 1.25 });
                    stage = Stage::fontScaled;
                    break;
                case Stage::fontScaled:
                    if (std::abs (fontSnapshot.scale - 1.25f) > 0.001f) return;
                    require (fontPanel.nativeWindowHandleForScenario() == fontHandle, "font scale recreated the native panel");
                    require (std::abs (fontSnapshot.pixels - std::floor (DuskWidgets::FontSizes {}.textEntry * 1.25f)) < 0.01f,
                             "panel fonts did not follow the native scale change");
                    fontPanel.setGeometry ({ 50, 50, 300, 200, 1.0 });
                    stage = Stage::fontRestored;
                    break;
                case Stage::fontRestored:
                    if (std::abs (fontSnapshot.scale - 1.0f) > 0.001f) return;
                    require (fontPanel.nativeWindowHandleForScenario() == fontHandle, "font scale restoration recreated the panel");
                    require (std::abs (fontSnapshot.pixels - DuskWidgets::FontSizes {}.textEntry) < 0.01f,
                             "panel fonts did not restore their original size");
                    fontPanel.close();
                    stage = Stage::fontClosed;
                    break;
                case Stage::fontClosed:
                    if (fontPanel.isOpen()) return;
                    std::puts ("PASS native panel fonts: scale up, scale down, same native window");
                   #if DUSK_NATIVE_PROBE_DAF
                    stage = Stage::donorMapping;
                   #else
                    stage = Stage::clipboardMapping;
                   #endif
                    break;
               #if DUSK_NATIVE_PROBE_DAF
                case Stage::donorMapping:
                    if (shell.getWidth() < 1900 || shell.getHeight() < 1000) return;
                    startDonors();
                    stage = Stage::donorRendered;
                    break;
                case Stage::donorRendered:
                    for (const auto& host : donorHosts) if (! host->hasRenderedFrame()) return;
                    for (std::size_t i = 0; i < donorHosts.size(); ++i)
                        require (! std::filesystem::exists (donorMarker (i)), "rendered donor retained its crash marker");
                    donorHandle = donorHosts[1]->nativeWindow();
                    donorHosts[1]->setGeometry ({ 50, 60, 700, 420, 1.25 });
                    require (donorHosts[1]->nativeWindow() == donorHandle, "native donor scale change recreated its window");
                    donorHosts[0]->close();
                    donorHosts[2]->shutdown();
                    require (donorHosts[2]->open (parent(), { 80, 90, 640, 360, 1.0 }), "donor cancellation open failed");
                    require (! donorHosts[2]->hasRenderedFrame(), "donor claimed a frame before pumping");
                    donorHosts[2]->close();
                    stage = Stage::donorClosed;
                    break;
                case Stage::donorClosed:
                    if (donorHosts[0]->isOpen() || donorHosts[2]->isOpen()) return;
                    require (donorCloses[0] == 1 && donorCloses[2] == 1, "donor close was not reported exactly once");
                    require (! std::filesystem::exists (donorMarker (2)), "cancelled donor retained its crash marker");
                    require (donorHosts[1]->isOpen() && donorHosts[3]->isOpen(), "closing a donor lost a sibling");
                    require (donorHosts[0]->open (parent(), { 20, 20, 640, 360, 1.0 }), "donor reopen failed");
                    stage = Stage::donorReopened;
                    break;
                case Stage::donorReopened:
                    if (! donorHosts[0]->hasRenderedFrame()) return;
                    donorSiblingFrame = donorViews[3]->completedDisplayCount();
                    shell.setVisible (false);
                    shell.removeFromDesktop();
                    shell.addToDesktop (juce::ComponentPeer::windowIsResizable);
                    stage = Stage::donorParentGone;
                    break;
                case Stage::donorParentGone:
                    if (donorHosts[0]->isOpen() || donorHosts[1]->isOpen() || a.isOpen()) return;
                    require (donorHosts[3]->isOpen(), "replacing the parent closed a donor on another shell");
                    if (donorViews[3]->completedDisplayCount() <= donorSiblingFrame) return;
                    require (donorCloses[0] == 2 && donorCloses[1] == 1, "parent loss duplicated donor close notifications");
                    clearDonors();
                    otherShell.setVisible (false);
                    shell.setVisible (true);
                    shell.getPeer()->setFullScreen (true);
                    std::puts ("PASS native donor editors: four plug-ins, shared panels, resize, cancellation, reopen, parent lifetime");
                    stage = Stage::clipboardMapping;
                    break;
               #endif
                case Stage::clipboardMapping:
                    if (shell.getWidth() < 1900 || shell.getHeight() < 1000) return;
                    if (foreignEditors == nullptr) foreignEditors = std::make_unique<ForeignEditorProbe> (shell);
                    if (! foreignEditors->advance()) return;
                    foreignEditors.reset();
                    require (a.open (parent(), { 0, 0, 960, 1080, 1.0 }), "clipboard panel A open failed");
                    require (b.open (parent(), { 960, 0, 960, 1080, 1.0 }), "clipboard panel B open failed");
                    stage = Stage::clipboardReady;
                    break;
                case Stage::clipboardReady:
                    if (! clipboardAnnounced)
                    {
                        if (! bothRendered()) return;
                        writeExchange ("ready", "ready");
                        clipboardAnnounced = true;
                    }
                    if (std::filesystem::exists (exchange / "close-first"))
                    {
                        std::filesystem::remove (exchange / "close-first");
                        a.close();
                        closingFirst = true;
                    }
                    if (closingFirst && ! a.isOpen())
                    {
                        writeExchange ("first-closed", "closed");
                        closingFirst = false;
                    }
                    if (std::filesystem::exists (exchange / "done"))
                    {
                        require (mousePresses >= 2, "native child pointer input was not delivered");
                        std::puts ("PASS native clipboard: external Unicode, large self-copy, empty publication, sibling replacement");
                        finish (0);
                    }
                    break;
            }
        }
        catch (const std::exception& error)
        {
            std::fprintf (stderr, "FAIL native panel stage %d: %s\n", static_cast<int> (stage), error.what());
            finish (1);
        }
    }

   #if DUSK_NATIVE_PROBE_DAF
    std::filesystem::path donorMarker (std::size_t index) const
    {
        return root.path() / ("donor-" + std::to_string (index));
    }

    void clearDonors()
    {
        for (auto& host : donorHosts) host.reset();
        for (auto& plugin : donorPlugins)
        {
            if (plugin) plugin->deactivate();
            plugin.reset();
        }
    }

    void startDonors()
    {
        donorPlugins = { builtin::createTapeEcho2(), builtin::createDuskVerb2(),
                         builtin::createTapeMachine2(), builtin::createSunset() };
        otherShell.setVisible (true);
        require (a.open (parent(), { 10, 800, 300, 160, 1.0 }), "donor sibling panel open failed");
        const auto shared = SharedWaylandApplication::acquire (platform::describeEmbeddedParent (parent()));
        for (std::size_t i = 0; i < donorHosts.size(); ++i)
        {
            auto* plugin = donorPlugins[i].get();
            require (plugin != nullptr && plugin->hasEditor(), "donor plug-in editor is unavailable");
            plugin->activate (48000.0, 64);
            donorHosts[i] = std::make_unique<DafEditorHost> ("donor-" + std::to_string (i), "Donor", donorMarker (i), 0);
            DafEditorHost::Unit unit;
            unit.createEditor = [this, plugin, i, expected = &shared->application()] (
                std::uintptr_t nativeParent, std::uint32_t width, std::uint32_t height, double scale,
                builtin::DafEditorCallbacks callbacks, std::string& error)
            {
                require (callbacks.hostApplication.get() == expected, "donor did not borrow the shared application");
                auto editor = plugin->createEditor (nativeParent, width, height, scale, std::move (callbacks), error);
                donorViews[i] = editor.get();
                return editor;
            };
            unit.paramCount = [plugin] { return static_cast<int> (plugin->params().size()); };
            unit.paramValue = [plugin] (int index) { return plugin->getParameterValue (static_cast<std::uint32_t> (index)); };
            unit.setParam = [plugin] (int index, float value) { plugin->setParameterValue (static_cast<std::uint32_t> (index), value); };
            donorHosts[i]->setUnit (std::move (unit));
            DafEditorHost::Callbacks callbacks;
            callbacks.closed = [this, i] { ++donorCloses[i]; donorViews[i] = nullptr; };
            donorHosts[i]->setCallbacks (std::move (callbacks));
            const auto nativeParent = i == 3 ? reinterpret_cast<std::uintptr_t> (otherShell.getPeer()->getNativeHandle()) : parent();
            require (donorHosts[i]->open (nativeParent, { 20, 20, 640, 360, 1.0 }), "native donor editor open failed");
            require (! donorHosts[i]->hasRenderedFrame(), "donor frame was confirmed at construction");
        }
    }
   #endif

    void writeExchange (const char* name, const std::string& value)
    {
        std::ofstream stream (exchange / name, std::ios::binary);
        stream << value;
        require (stream.good(), "could not write clipboard probe response");
    }

    std::string readExchange (const char* name)
    {
        std::ifstream stream (exchange / name, std::ios::binary);
        require (stream.good(), "could not read clipboard probe request");
        return { std::istreambuf_iterator<char> (stream), std::istreambuf_iterator<char>() };
    }

    void clipboardKey (DGL::Window& target, unsigned key)
    {
        if (stage != Stage::clipboardReady) return;
        try
        {
            if (key == 'c' || key == 'e')
            {
                const auto value = key == 'c' ? readExchange ("payload") : std::string();
                require (target.setClipboard ("text/plain", value.c_str(), value.size() + 1),
                         "native clipboard publication was refused");
                writeExchange ("published", std::to_string (++publications));
            }
            else if (key == 'p')
            {
                std::size_t size = 0;
                const auto* bytes = static_cast<const char*> (target.getClipboard (size));
                require (bytes != nullptr, "native clipboard read returned no text");
                if (size > 0 && bytes[size - 1] == '\0') --size;
                const auto expected = readExchange ("expected");
                if (std::string (bytes, size) != expected)
                {
                    writeExchange ("actual", std::string (bytes, size));
                    throw std::runtime_error ("native clipboard read returned " + std::to_string (size)
                        + " bytes instead of the expected " + std::to_string (expected.size()) + " bytes");
                }
                writeExchange ("pasted", std::to_string (++pastes));
            }
        }
        catch (const std::exception& error)
        {
            std::fprintf (stderr, "FAIL native clipboard: %s\n", error.what());
            finish (1);
        }
    }

    void finish (int result)
    {
        stopTimer();
        juce::JUCEApplication::getInstance()->setApplicationReturnValue (result);
        juce::JUCEApplication::quit();
    }

    const std::filesystem::path exchange = std::getenv ("DUSK_NATIVE_PROBE_WORK") != nullptr
        ? std::getenv ("DUSK_NATIVE_PROBE_WORK") : "";
    bool clipboardAnnounced = false;
    bool closingFirst = false;
    int publications = 0, pastes = 0, mousePresses = 0;
    duskstudio::test::TempDirectory root { "dusk-native-embed-" };
    Shell shell;
    Shell otherShell;
    std::unique_ptr<ForeignEditorProbe> foreignEditors;
    DuskImGuiHost a { { "probe-a", "probe-a", "Probe A" }, root.path() / "a-frame" };
    DuskImGuiHost b { { "probe-b", "probe-b", "Probe B" }, root.path() / "b-frame" };
    DuskImGuiHost c { { "probe-c", "probe-c", "Probe C" }, root.path() / "c-frame" };
    DuskImGuiHost d { { "probe-d", "probe-d", "Probe D" }, root.path() / "d-frame" };
   #if DUSK_NATIVE_PROBE_DAF
    std::array<std::unique_ptr<builtin::DafPlugin>, 4> donorPlugins;
    std::array<std::unique_ptr<DafEditorHost>, 4> donorHosts;
    std::array<builtin::DafEditor*, 4> donorViews {};
    std::array<int, 4> donorCloses {};
    std::uintptr_t donorHandle = 0;
    std::uint64_t donorSiblingFrame = 0;
   #endif
    FontSnapshot fontSnapshot;
    std::uintptr_t fontHandle = 0;
    duskstudio::imgui::DuskPanelWindow fontPanel { "probe-fonts", "probe-fonts", "Font Probe" };
    std::shared_ptr<SharedWaylandApplication> failedApplication;
    std::unique_ptr<SharedWaylandApplication::Registration> fault;
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds (60);
    Stage stage = Stage::open;
    int closed = 0;
    int visibilityCycles = 0;
    std::uintptr_t originalParent = 0;
    std::uint64_t siblingFrame = 0;
    bool failureReported = false;
};

class App final : public juce::JUCEApplication
{
public:
    const juce::String getApplicationName() override { return "Dusk Native Probe"; }
    const juce::String getApplicationVersion() override { return "1"; }
    bool moreThanOneInstanceAllowed() override { return true; }
    void initialise (const juce::String&) override
    {
        setApplicationReturnValue (1);
        probe = std::make_unique<Probe>();
    }
    void shutdown() override { probe.reset(); }
    void anotherInstanceStarted (const juce::String&) override {}

private:
    std::unique_ptr<Probe> probe;
};
}

START_JUCE_APPLICATION (App)
