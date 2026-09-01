#include <catch2/catch_test_macros.hpp>

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
           "[issue-367][issue-369][issue-376]")
{
    const auto mac = definitionBody (
        readSource ("src/ui/PlatformWindowing_Mac.mm"), "bringWindowToFront");
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
    REQUIRE (windows.find ("AllowSetForegroundWindow") != std::string::npos);
    requireInOrder (windows, "SetForegroundWindow", "FlashWindowEx");

    const auto windowsSource = readSource ("src/ui/PlatformWindowing_Windows.cpp");
    const auto layout = definitionBody (windowsSource, "layoutChild");
    REQUIRE (layout.find ("getTopLevelComponent") != std::string::npos);
    REQUIRE (layout.find ("getLocalArea") != std::string::npos);
    REQUIRE (layout.find ("getLocalBounds") != std::string::npos);
    REQUIRE (layout.find ("embedscale::toPhysical") != std::string::npos);
    REQUIRE (layout.find ("getBoundsInParent") == std::string::npos);
    requireInOrder (layout, "getLocalArea", "embedscale::toPhysical");
    requireInOrder (layout, "embedscale::toPhysical", "SetWindowPos");

    const auto moved = definitionBody (windowsSource, "moved");
    REQUIRE (moved.find ("layoutChild") != std::string::npos);
    const auto resized = definitionBody (windowsSource, "resized");
    REQUIRE (resized.find ("layoutChild") != std::string::npos);

    const auto linuxTeardown = definitionBody (
        readSource ("src/ui/PlatformWindowing_Linux.cpp"),
        "prepareForTopLevelDestruction");

    // Xorg and default XWayland peers both take the X11 branch, regardless of
    // WAYLAND_DISPLAY. Only an actual Wayland peer (available to the native
    // development mode) may select the wl_surface roundtrip.
    REQUIRE (linuxTeardown.find ("getenv") == std::string::npos);
    REQUIRE (linuxTeardown.find ("getWaylandWindowForPeer") != std::string::npos);
    requireInOrder (linuxTeardown, "getWaylandWindowForPeer", "if (topLevelUsesWayland)");

    // A wl_surface pointer must never be submitted to Xlib as a Window ID.
    REQUIRE (linuxTeardown.find ("XWithdrawWindow") == std::string::npos);
    REQUIRE (linuxTeardown.find ("requestFocusOnMainWaylandSurface") != std::string::npos);
    REQUIRE (linuxTeardown.find ("XIconifyWindow") != std::string::npos);

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
