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
           "[issue-367][issue-369][issue-376][issue-380][issue-448][issue-449]")
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
    REQUIRE (cachedShow < shellLoad);
    REQUIRE (shellLoad < stateSync);
    REQUIRE (stateSync < shellHost);
    REQUIRE (shellHost < shellHide);
    REQUIRE (shellHide < shellShow);
    REQUIRE (shellShow < fallbackShow);
    REQUIRE (fallbackShow < nextPlatformBranch);

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
