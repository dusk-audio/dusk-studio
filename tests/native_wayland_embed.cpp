#define JUCE_GUI_BASICS_INCLUDE_XHEADERS 1
#include <juce_gui_basics/juce_gui_basics.h>
#include "../src/ui/imgui/DuskImGuiHost.h"
#include "../src/ui/imgui/SharedWaylandApplication.h"
#include "TestTempDirectory.h"

#include <OpenGL.hpp>

#include <chrono>
#include <cmath>
#include <cstdio>
#include <stdexcept>

namespace
{
using duskstudio::imgui::DuskImGuiHost;
using duskstudio::imgui::SharedWaylandApplication;
namespace platform = duskstudio::platform;

void require (bool ok, const char* message)
{
    if (! ok) throw std::runtime_error (message);
}

struct Painter final : DGL::TopLevelWidget, DGL::IdleCallback
{
    explicit Painter (DGL::Window& window) : TopLevelWidget (window)
    {
        getWindow().addIdleCallback (this);
    }
    ~Painter() override { getWindow().removeIdleCallback (this); }
    void idleCallback() override { repaint(); }
    void onDisplay() override
    {
        glClearColor (0.2f, 0.4f, 0.6f, 1.0f);
        glClear (GL_COLOR_BUFFER_BIT);
    }
};

struct Shell final : juce::Component
{
    ~Shell() override
    {
        if (frame != nullptr) juce::WaylandSymbols::getInstance()->callbackDestroy (frame);
    }

    void paint (juce::Graphics& graphics) override { graphics.fillAll (juce::Colours::darkgrey); }

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
        shell.setBounds (50, 50, 800, 600);
        shell.setOpaque (true);
        shell.addToDesktop (0);
        shell.setVisible (true);
        shell.setVisible (false);
        shell.setVisible (true);
        shell.watchFrame();
        DuskImGuiHost::Callbacks callbacks;
        callbacks.createWidget = [] (DGL::Window& window) { return std::make_unique<Painter> (window); };
        callbacks.closed = [this] { ++closed; };
        a.setCallbacks (callbacks);
        b.setCallbacks (callbacks);
        c.setCallbacks (callbacks);
        d.setCallbacks (callbacks);
        startTimer (20);
    }

    ~Probe() override { stopTimer(); }

private:
    enum class Stage { open, rendered, firstClosed, resized, remapped, secondWindow, parentGone, failureReady, failed };

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
                    finish (0);
                    break;
                }
            }
        }
        catch (const std::exception& error)
        {
            std::fprintf (stderr, "FAIL native panel stage %d: %s\n", static_cast<int> (stage), error.what());
            finish (1);
        }
    }

    void finish (int result)
    {
        stopTimer();
        juce::JUCEApplication::getInstance()->setApplicationReturnValue (result);
        juce::JUCEApplication::quit();
    }

    duskstudio::test::TempDirectory root { "dusk-native-embed-" };
    Shell shell;
    Shell otherShell;
    DuskImGuiHost a { { "probe-a", "probe-a", "Probe A" }, root.path() / "a-frame" };
    DuskImGuiHost b { { "probe-b", "probe-b", "Probe B" }, root.path() / "b-frame" };
    DuskImGuiHost c { { "probe-c", "probe-c", "Probe C" }, root.path() / "c-frame" };
    DuskImGuiHost d { { "probe-d", "probe-d", "Probe D" }, root.path() / "d-frame" };
    std::shared_ptr<SharedWaylandApplication> failedApplication;
    std::unique_ptr<SharedWaylandApplication::Registration> fault;
    const std::chrono::steady_clock::time_point deadline = std::chrono::steady_clock::now() + std::chrono::seconds (20);
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
