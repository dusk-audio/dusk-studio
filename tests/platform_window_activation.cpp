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

        auto afterParameters = source.find_first_not_of (" \t\r\n", closeParen + 1);
        std::size_t openBrace = std::string::npos;
        if (afterParameters != std::string::npos && source[afterParameters] == '{')
            openBrace = afterParameters;
        else if (afterParameters != std::string::npos && source[afterParameters] == ':')
            openBrace = source.find ('{', afterParameters + 1); // constructor initializer list

        if (openBrace == std::string::npos)
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

TEST_CASE ("native window activation restores and raises the platform peer",
           "[windowing][macos][windows][regression][issue-369]")
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
