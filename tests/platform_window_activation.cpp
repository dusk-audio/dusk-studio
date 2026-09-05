#include <catch2/catch_test_macros.hpp>

#include "ui/LinuxPeerTeardown.h"

#include <fstream>
#include <iterator>
#include <string>

#ifndef DUSKSTUDIO_SOURCE_DIR
#define DUSKSTUDIO_SOURCE_DIR "."
#endif

namespace
{
std::string readSource (const char* relativePath)
{
    std::ifstream input (std::string (DUSKSTUDIO_SOURCE_DIR) + "/" + relativePath);
    REQUIRE (input.good());
    return { std::istreambuf_iterator<char> (input),
             std::istreambuf_iterator<char>() };
}

std::string definitionBody (const std::string& source, const std::string& methodName)
{
    std::size_t searchFrom = 0;
    for (;;)
    {
        const auto method = source.find (methodName, searchFrom);
        if (method == std::string::npos) break;

        const auto afterName = method + methodName.size();
        const auto openParen = source.find_first_not_of (" \t\r\n", afterName);
        if (openParen == std::string::npos || source[openParen] != '(')
        {
            searchFrom = afterName;
            continue;
        }

        std::size_t closeParen = openParen;
        int parenDepth = 0;
        for (; closeParen < source.size(); ++closeParen)
        {
            if (source[closeParen] == '(') ++parenDepth;
            if (source[closeParen] == ')' && --parenDepth == 0) break;
        }
        REQUIRE (closeParen < source.size());

        const auto openBrace = source.find ('{', closeParen + 1);
        const auto declarationEnd = source.find (';', closeParen + 1);

        if (openBrace == std::string::npos
            || (declarationEnd != std::string::npos && declarationEnd < openBrace))
        {
            searchFrom = afterName;
            continue;
        }

        int braceDepth = 1;
        for (auto cursor = openBrace + 1; cursor < source.size(); ++cursor)
        {
            if (source[cursor] == '{') ++braceDepth;
            if (source[cursor] == '}' && --braceDepth == 0)
                return source.substr (openBrace + 1, cursor - openBrace - 1);
        }
        FAIL ("unterminated method body for " << methodName);
        return {};
    }

    FAIL ("method definition not found: " << methodName);
    return {};
}

void requireInOrder (const std::string& source,
                     const std::string& first,
                     const std::string& second)
{
    const auto firstAt = source.find (first);
    REQUIRE (firstAt != std::string::npos);
    const auto secondAt = source.find (second, firstAt + first.size());
    REQUIRE (secondAt != std::string::npos);
}
}

TEST_CASE ("native window operations preserve activation and embedded editor geometry",
           "[windowing][linux][macos][windows][wayland][regression]"
           "[issue-367][issue-369][issue-376][issue-380][issue-448][issue-449]"
           "[issue-453]")
{
    using duskstudio::platform::LinuxPeerTeardown;
    using duskstudio::platform::linuxPeerTeardownForMapping;

    const auto macSource = readSource ("src/ui/PlatformWindowing_Mac.mm");
    const auto mac = definitionBody (macSource, "bringWindowToFront");
    REQUIRE (mac.find ("getNativeHandle") != std::string::npos);
    REQUIRE (mac.find ("@available(macOS 14.0") != std::string::npos);
    REQUIRE (mac.find ("[NSApp activate]") != std::string::npos);
    REQUIRE (mac.find ("activateIgnoringOtherApps") != std::string::npos);
    REQUIRE (mac.find ("isMiniaturized") != std::string::npos);
    REQUIRE (mac.find ("deminiaturize") != std::string::npos);
    requireInOrder (mac, "[NSApp activate]", "makeKeyAndOrderFront");

    const auto windows = definitionBody (
        readSource ("src/ui/PlatformWindowing_Windows.cpp"), "bringWindowToFront");
    REQUIRE (windows.find ("getNativeHandle") != std::string::npos);
    REQUIRE (windows.find ("IsWindow") != std::string::npos);
    REQUIRE (windows.find ("IsIconic") != std::string::npos);
    REQUIRE (windows.find ("SW_RESTORE") != std::string::npos);
    REQUIRE (windows.find ("AllowSetForegroundWindow") == std::string::npos);
    requireInOrder (windows, "SetForegroundWindow", "FlashWindowEx");

    const auto windowsSource = readSource ("src/ui/PlatformWindowing_Windows.cpp");
    const auto windowsFlush = definitionBody (windowsSource, "flushWindowOperations");
    REQUIRE (windowsFlush.find ("PeekMessageW") != std::string::npos);
    REQUIRE (windowsFlush.find ("TranslateMessage") != std::string::npos);
    REQUIRE (windowsFlush.find ("DispatchMessageW") != std::string::npos);
    REQUIRE (windowsFlush.find ("PostQuitMessage") != std::string::npos);

    const auto macFlush = definitionBody (macSource, "flushWindowOperations");
    REQUIRE (macFlush.find ("NSRunLoop") != std::string::npos);
    REQUIRE (macFlush.find ("runMode") != std::string::npos);
    REQUIRE (macFlush.find ("beforeDate") != std::string::npos);
    REQUIRE (macFlush.find ("kMaxDrainIterations") != std::string::npos);
    REQUIRE (macFlush.find ("for (") != std::string::npos);

    const auto singleInstance = readSource ("src/util/SingleInstance.cpp");
    const auto windowsBranch = singleInstance.rfind ("#elif defined (_WIN32)");
    REQUIRE (windowsBranch != std::string::npos);
    const auto handOver = definitionBody (
        singleInstance.substr (windowsBranch), "handOver");
    requireInOrder (handOver, "GetNamedPipeServerProcessId",
                    "AllowSetForegroundWindow");
    requireInOrder (handOver, "AllowSetForegroundWindow", "transferExact");

    const auto layout = definitionBody (windowsSource, "layoutChild");
    REQUIRE (layout.find ("getTopLevelComponent") != std::string::npos);
    REQUIRE (layout.find ("getLocalArea") != std::string::npos);
    REQUIRE (layout.find ("getLocalBounds") != std::string::npos);
    REQUIRE (layout.find ("embedscale::toPhysical") != std::string::npos);
    REQUIRE (layout.find ("getBoundsInParent") == std::string::npos);
    REQUIRE (layout.find ("IsWindow") != std::string::npos);
    REQUIRE (layout.find ("SWP_SHOWWINDOW") == std::string::npos);
    requireInOrder (layout, "getLocalArea", "embedscale::toPhysical");
    requireInOrder (layout, "embedscale::toPhysical", "SetWindowPos");

    const auto setWindowParent = definitionBody (windowsSource, "setWindowParent");
    REQUIRE (setWindowParent.find ("IsWindow") != std::string::npos);
    REQUIRE (setWindowParent.find ("SetLastError") != std::string::npos);
    REQUIRE (setWindowParent.find ("SetParent") != std::string::npos);
    REQUIRE (setWindowParent.find ("GetLastError") != std::string::npos);

    const auto parentChanged = definitionBody (windowsSource, "parentHierarchyChanged");
    REQUIRE (parentChanged.find ("attachedParent") != std::string::npos);
    REQUIRE (parentChanged.find ("setWindowParent") != std::string::npos);
    REQUIRE (parentChanged.find ("WS_VISIBLE") == std::string::npos);

    // The host shows its editor window before handing the handle over, so a
    // reparent that fails has to restore the original style AND hide the
    // window; leaving it mapped strands a titlebarred plugin window on the
    // desktop. The hide belongs to the failure branch, ahead of the success
    // path's attachedParent publish.
    requireInOrder (parentChanged, "! setWindowParent", "originalStyle");
    requireInOrder (parentChanged, "originalStyle", "SWP_HIDEWINDOW");
    requireInOrder (parentChanged, "SWP_HIDEWINDOW", "attachedParent = nextParent");

    const auto detachChild = definitionBody (windowsSource, "detachChild");
    REQUIRE (detachChild.find ("IsWindow") != std::string::npos);
    REQUIRE (detachChild.find ("setWindowParent") != std::string::npos);
    REQUIRE (detachChild.find ("SetWindowLongPtr") != std::string::npos);
    REQUIRE (detachChild.find ("SetWindowPos") != std::string::npos);

    const auto visibility = definitionBody (windowsSource, "visibilityChanged");
    REQUIRE (visibility.find ("IsWindow") != std::string::npos);
    REQUIRE (visibility.find ("ShowWindow") != std::string::npos);
    REQUIRE (visibility.find ("SW_HIDE") != std::string::npos);
    REQUIRE (visibility.find ("SW_SHOWNA") != std::string::npos);
    // Visibility only follows a child this component actually holds: an
    // unattached window is top-level and owned by the plugin host, and showing
    // it from here floats it loose over the desktop.
    requireInOrder (visibility, "attachedParent", "ShowWindow");

    const auto moved = definitionBody (windowsSource, "moved");
    REQUIRE (moved.find ("layoutChild") != std::string::npos);
    const auto resized = definitionBody (windowsSource, "resized");
    REQUIRE (resized.find ("layoutChild") != std::string::npos);

    struct LinuxTeardownCase
    {
        const char* configuration;
        bool peerHasWaylandMapping;
        LinuxPeerTeardown expected;
    };

    const LinuxTeardownCase linuxCases[] {
        { "Xorg", false, LinuxPeerTeardown::x11 },
        { "Wayland desktop, default XWayland peer", false, LinuxPeerTeardown::x11 },
        { "Wayland desktop, DUSKSTUDIO_NATIVE_WAYLAND=1 peer", true,
          LinuxPeerTeardown::wayland },
    };

    for (const auto& linuxCase : linuxCases)
    {
        INFO ("Linux configuration: " << linuxCase.configuration);
        CHECK (linuxPeerTeardownForMapping (linuxCase.peerHasWaylandMapping)
               == linuxCase.expected);
    }

    const auto linuxTeardown = definitionBody (
        readSource ("src/ui/PlatformWindowing_Linux.cpp"),
        "prepareForTopLevelDestruction");

    // Xorg and default XWayland peers both take the X11 branch, regardless of
    // WAYLAND_DISPLAY. Only an actual Wayland peer (available to the native
    // development mode) may select the wl_surface roundtrip.
    REQUIRE (linuxTeardown.find ("getenv") == std::string::npos);
    REQUIRE (linuxTeardown.find ("getWaylandWindowForPeer") != std::string::npos);
    requireInOrder (linuxTeardown, "getWaylandWindowForPeer",
                    "if (teardown == LinuxPeerTeardown::wayland)");

    // A wl_surface pointer must never be submitted to Xlib as a Window ID.
    REQUIRE (linuxTeardown.find ("XWithdrawWindow") == std::string::npos);
    REQUIRE (linuxTeardown.find ("requestFocusOnMainWaylandSurface") != std::string::npos);
    REQUIRE (linuxTeardown.find ("XIconifyWindow") != std::string::npos);

    const auto remoteEditor = definitionBody (
        readSource ("src/ui/ChannelStripComponent.cpp"),
        "ChannelStripComponent::openPluginEditor");
    const auto remoteBranch = remoteEditor.find ("if (pluginSlot.isRemote())");
    const auto nonMacGuard = remoteEditor.find ("#if ! JUCE_MAC", remoteBranch);
    const auto nonMacShow = remoteEditor.find ("showRemoteEditor", nonMacGuard);
    const auto macBranch = remoteEditor.find ("#elif JUCE_MAC", nonMacShow);
    const auto cachedShell = remoteEditor.find ("remoteForeignEmbed != nullptr", macBranch);
    const auto cachedHide = remoteEditor.find ("hideRemoteEditor", cachedShell);
    const auto cachedShow = remoteEditor.find ("pluginEditorModal.showBorrowed", cachedHide);
    const auto shellLoad = remoteEditor.find ("ensureShellInstanceForEditor", macBranch);
    const auto stateSync = remoteEditor.find ("syncShellStateFromChild", shellLoad);
    const auto shellHost = remoteEditor.find ("createInProcessEditorHost", stateSync);
    const auto shellHide = remoteEditor.find ("hideRemoteEditor", shellHost);
    const auto shellShow = remoteEditor.find ("pluginEditorModal.showBorrowed", shellHide);
    const auto fallbackShow = remoteEditor.find ("showRemoteEditor", shellShow);
    const auto nextPlatformBranch = remoteEditor.find ("#else", fallbackShow);

    // On macOS the child window is a fallback, not a prerequisite for the
    // parent-side shell editor. State arrives through GetState before the shell
    // is hosted; ShowEditor occurs only after shell hosting has failed.
    REQUIRE (remoteBranch != std::string::npos);
    REQUIRE (nonMacGuard < nonMacShow);
    REQUIRE (nonMacShow < macBranch);
    REQUIRE (macBranch < cachedShell);
    REQUIRE (cachedShell < cachedHide);
    REQUIRE (cachedHide < cachedShow);
    REQUIRE (remoteEditor.substr (cachedHide, cachedShow - cachedHide)
                 .find ("return") == std::string::npos);
    REQUIRE (cachedShow < shellLoad);
    REQUIRE (shellLoad < stateSync);
    REQUIRE (stateSync < shellHost);
    REQUIRE (shellHost < shellHide);
    REQUIRE (shellHide < shellShow);
    REQUIRE (remoteEditor.substr (shellHide, shellShow - shellHide)
                 .find ("embed.reset") == std::string::npos);
    REQUIRE (shellShow < fallbackShow);
    REQUIRE (fallbackShow < nextPlatformBranch);

    const auto closeEditor = definitionBody (
        readSource ("src/ui/ChannelStripComponent.cpp"),
        "ChannelStripComponent::closePluginEditor");
    REQUIRE (closeEditor.find ("hasRemoteEditorEmbed") != std::string::npos);
    REQUIRE (closeEditor.find ("pluginSlot.isRemote() || hasRemoteEditorEmbed")
             != std::string::npos);

    const auto stripTimer = definitionBody (
        readSource ("src/ui/ChannelStripComponent.cpp"),
        "ChannelStripComponent::timerCallback");
    const auto crashCleanup = stripTimer.find ("pluginSlot.wasCrashed()");
    const auto closeStaleEditor = stripTimer.find ("closePluginEditor", crashCleanup);
    const auto refreshSlot = stripTimer.find ("refreshPluginSlotButton");
    REQUIRE (crashCleanup != std::string::npos);
    REQUIRE (crashCleanup < closeStaleEditor);
    REQUIRE (closeStaleEditor < refreshSlot);

    const auto modal = definitionBody (
        readSource ("src/ui/EmbeddedModal.h"), "componentMovedOrResized");
    REQUIRE (modal.find ("wasMoved") != std::string::npos);
    REQUIRE (modal.find ("hostChanged && wasMoved") != std::string::npos);
    REQUIRE (modal.find ("borrowedBody_->resized") != std::string::npos);
}

TEST_CASE ("launch session load and instance handoff retain native activation",
           "[windowing][regression][issue-369]")
{
    const auto app = readSource ("src/DuskStudioApp.cpp");
    const auto initialLaunch = definitionBody (app, "MainWindow");
    requireInOrder (initialLaunch, "self->setVisible (true)", "bringWindowToFront");

    const auto handoff = definitionBody (app, "anotherInstanceStarted");
    requireInOrder (handoff, "mainWindow->toFront (true)", "bringWindowToFront");
    requireInOrder (handoff, "bringWindowToFront", "openSessionPath");

    const auto sessionLoad = definitionBody (
        readSource ("src/ui/MainComponent.cpp"), "finishLoadingSessionFrom");
    REQUIRE (sessionLoad.find ("bringWindowToFront") != std::string::npos);
}

TEST_CASE ("OOP editor window RPCs dispatch through the child message thread",
           "[windowing][macos][windows][regression][issue-447]")
{
    const auto hostSource = readSource ("src/engine/ipc/PluginHostMain.cpp");
    const auto dispatch = definitionBody (hostSource, "runOnHostMessageThreadAndWait");
    REQUIRE (dispatch.find ("dusk::callAsync") != std::string::npos);
    REQUIRE (dispatch.find ("dusk::AutoResetEvent") != std::string::npos);
    REQUIRE (dispatch.find ("completion.wait") != std::string::npos);

    for (const auto* handler : { "handleShowEditor", "handleHideEditor",
                                 "handleResizeEditor" })
    {
        INFO ("handler: " << handler);
        const auto body = definitionBody (hostSource, handler);
        REQUIRE (body.find ("runOnHostMessageThreadAndWait") != std::string::npos);
        REQUIRE (body.find ("MessageManagerLock") == std::string::npos);
    }
}

TEST_CASE ("X11 editor teardown trap is unwind safe, lock free and backend checked",
           "[windowing][linux][x11][wayland][regression][issue-375][issue-376]"
           "[issue-452]")
{
    const auto linuxSource = readSource ("src/ui/PlatformWindowing_Linux.cpp");

    // The Xlib error handler is process-global while the trap it reads lives on
    // the teardown stack frame, so install and restore have to be tied to a
    // scope rather than to straight-line code that an exception can skip.
    REQUIRE (linuxSource.find ("struct ScopedEditorTeardownTrap") != std::string::npos);

    const auto guardCtor = definitionBody (linuxSource, "ScopedEditorTeardownTrap");
    requireInOrder (guardCtor, "XSetErrorHandler", "activeEditorTeardownTrap.store");

    const auto guardDtor = definitionBody (linuxSource, "~ScopedEditorTeardownTrap");
    requireInOrder (guardDtor, "XSync", "XSetErrorHandler");
    requireInOrder (guardDtor, "XSetErrorHandler", "activeEditorTeardownTrap.store");
    REQUIRE (guardDtor.find ("store (nullptr") != std::string::npos);

    const auto teardownRun = definitionBody (linuxSource, "runX11EditorTeardown");
    const auto lockAt = teardownRun.find ("editorTeardownTrapMutex");
    const auto guardAt = teardownRun.find ("ScopedEditorTeardownTrap");
    const auto teardownAt = teardownRun.find ("teardown();", guardAt);
    REQUIRE (lockAt != std::string::npos);
    REQUIRE (guardAt != std::string::npos);
    REQUIRE (teardownAt != std::string::npos);
    REQUIRE (lockAt < guardAt);
    REQUIRE (teardownRun.find ("XSetErrorHandler") == std::string::npos);
    REQUIRE (teardownRun.find ("activeEditorTeardownTrap") == std::string::npos);

    // The handler runs on whichever thread made the failing Xlib call, and the
    // installer holds the mutex across teardown(), so the pointer cannot be
    // read under that same mutex without deadlocking.
    REQUIRE (linuxSource.find ("std::atomic<EditorTeardownErrorTrap*> activeEditorTeardownTrap")
             != std::string::npos);
    const auto handler = definitionBody (linuxSource, "editorTeardownXErrorHandler");
    REQUIRE (handler.find ("activeEditorTeardownTrap.load") != std::string::npos);

    // bringWindowToFront casts the chosen peer's handle to an X11 Window, so a
    // real wl_surface peer must never be offered as the focus target.
    const auto sibling = definitionBody (linuxSource, "pickSiblingFocusTargetPeer");
    REQUIRE (sibling.find ("DUSKSTUDIO_JUCE_HAS_WAYLAND") != std::string::npos);
    requireInOrder (sibling, "getWaylandWindowForPeer", "return peer");
}
