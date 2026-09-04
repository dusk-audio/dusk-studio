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
}

TEST_CASE ("CLAP editors size the GUI before parenting it",
           "[clap][editor][regression][issue-361]")
{
    for (const char* path : { "src/engine/clap/ClapEditor.cpp",
                              "src/engine/clap/ClapEditor_Win.cpp",
                              "src/engine/clap/ClapEditor_Mac.mm" })
    {
        const auto source = readSource (path);
        const auto embed = source.find ("bool ClapEditor::embed");
        const auto setSize = source.find ("gui->set_size", embed);
        const auto setParent = source.find ("gui->set_parent", embed);
        const auto show = source.find ("gui->show", embed);

        INFO (path);
        REQUIRE (embed != std::string::npos);
        REQUIRE (setSize != std::string::npos);
        REQUIRE (setParent != std::string::npos);
        REQUIRE (show != std::string::npos);
        REQUIRE (setSize < setParent);
        REQUIRE (setParent < show);
    }
}

TEST_CASE ("the X11 CLAP editor maps whatever window the plugin parents into it",
           "[clap][editor][regression][issue-361][issue-397]")
{
    const auto source = readSource ("src/engine/clap/ClapEditor.cpp");

    const auto embed = source.find ("bool ClapEditor::embed");
    const auto setParent = source.find ("gui->set_parent", embed);
    const auto mapDefinition = source.find ("void ClapEditor::mapPluginChildren", setParent);
    const auto mapChildren = source.find ("mapPluginChildren()", setParent);
    REQUIRE (embed != std::string::npos);
    REQUIRE (setParent != std::string::npos);
    REQUIRE (mapDefinition != std::string::npos);
    REQUIRE (mapChildren != std::string::npos);
    REQUIRE (mapChildren < mapDefinition);

    // A plugin that builds its window off the message thread parents it after
    // embed() returns, so pump() itself must retry rather than merely leaving a
    // second occurrence such as the method definition elsewhere in the file.
    const auto pump = source.find ("ClapEditor::pump");
    REQUIRE (pump != std::string::npos);
    const auto pumpBody = source.find ('{', pump);
    REQUIRE (pumpBody != std::string::npos);
    const auto nextMethod = source.find ("ClapEditor::", pumpBody + 1);
    const auto retryMap = source.find ("mapPluginChildren()", pumpBody);
    REQUIRE (nextMethod != std::string::npos);
    REQUIRE (retryMap < nextMethod);
    REQUIRE (source.find ("XMapWindow (dpy, children[i])") != std::string::npos);
}

TEST_CASE ("a late X11 CLAP child revives the empty latch without mapping a hidden container",
           "[clap][editor][regression][issue-361]")
{
    const auto source = readSource ("src/engine/clap/ClapEditor.cpp");
    const auto mapChildren = source.find ("void ClapEditor::mapPluginChildren");
    const auto nextMethod = source.find ("ClapEditor::ContainerContent", mapChildren);
    const auto clearEmpty = source.find ("containerEmpty = false", mapChildren);
    const auto clearPolls = source.find ("untouchedPolls = 0", mapChildren);

    REQUIRE (mapChildren != std::string::npos);
    REQUIRE (nextMethod != std::string::npos);
    REQUIRE (clearEmpty < nextMethod);
    REQUIRE (clearPolls < nextMethod);
    REQUIRE (source.substr (mapChildren, nextMethod - mapChildren).find ("reveal()")
             == std::string::npos);

    const auto pump = source.find ("void ClapEditor::pump");
    const auto lateMap = source.find ("mapPluginChildren()", pump);
    const auto contentCheck = source.find ("readContainerContent()", lateMap);
    REQUIRE (pump != std::string::npos);
    REQUIRE (lateMap != std::string::npos);
    REQUIRE (contentCheck != std::string::npos);
    REQUIRE (source.substr (lateMap, contentCheck - lateMap).find ("reveal()")
             == std::string::npos);
}

TEST_CASE ("an X11 CLAP editor judged empty retries when explicitly revealed",
           "[clap][editor][regression][issue-361]")
{
    const auto editorSource = readSource ("src/engine/clap/ClapEditor.cpp");
    const auto reveal = editorSource.find ("void ClapEditor::reveal()");
    const auto hide = editorSource.find ("void ClapEditor::hide()", reveal);
    const auto clearEmpty = editorSource.find ("containerEmpty = false", reveal);
    const auto clearPolls = editorSource.find ("untouchedPolls = 0", reveal);
    const auto mapWindow = editorSource.find ("XMapWindow", reveal);

    REQUIRE (reveal != std::string::npos);
    REQUIRE (hide != std::string::npos);
    REQUIRE (clearEmpty < hide);
    REQUIRE (clearPolls < hide);
    REQUIRE (mapWindow < hide);
    REQUIRE (clearEmpty < mapWindow);
    REQUIRE (clearPolls < mapWindow);

    const auto componentSource = readSource ("src/ui/ClapPluginEditorComponent.cpp");
    const auto timer = componentSource.find ("void ClapPluginEditorComponent::timerCallback()");
    const auto pump = componentSource.find ("editor.pump", timer);
    const auto missingGuard = componentSource.find ("! editor.pluginWindowMissing()", timer);
    const auto timerReveal = componentSource.find ("editor.reveal()", timer);

    REQUIRE (timer != std::string::npos);
    REQUIRE (pump != std::string::npos);
    REQUIRE (missingGuard < pump);
    REQUIRE (timerReveal < pump);
    REQUIRE (missingGuard < timerReveal);
}

TEST_CASE ("the X11 empty-container check ignores pixels outside the visual depth",
           "[clap][editor][regression][issue-361]")
{
    const auto source = readSource ("src/engine/clap/ClapEditor.cpp");
    REQUIRE (source.find ("#include <X11/Xutil.h>") != std::string::npos);

    const auto readContent = source.find ("ClapEditor::ContainerContent ClapEditor::readContainerContent");
    const auto colourMask = source.find ("red_mask | attr.visual->green_mask | attr.visual->blue_mask",
                                         readContent);
    const auto getPixel = source.find ("XGetPixel", readContent);
    const auto compareFill = source.find ("containerFill & colourMask", getPixel);
    REQUIRE (readContent != std::string::npos);
    REQUIRE (colourMask != std::string::npos);
    REQUIRE (getPixel != std::string::npos);
    REQUIRE (compareFill != std::string::npos);
}

TEST_CASE ("a CLAP gui show() that reports failure keeps the editor",
           "[clap][editor][regression][issue-361][issue-391]")
{
    for (const char* path : { "src/engine/clap/ClapEditor.cpp",
                              "src/engine/clap/ClapEditor_Win.cpp",
                              "src/engine/clap/ClapEditor_Mac.mm" })
    {
        const auto source = readSource (path);
        const auto embed = source.find ("bool ClapEditor::embed");
        const auto body = source.find ('{', embed);
        const auto nextMethod = source.find ("ClapEditor::", body + 1);
        const auto show = source.find ("gui->show", body);
        const auto result = source.find ("const bool shown", body);
        const auto castDiagnostic = source.find ("(int) shown", show);
        const auto branchDiagnostic = source.find ("shown ?", show);

        INFO (path);
        REQUIRE (embed != std::string::npos);
        REQUIRE (body != std::string::npos);
        REQUIRE (nextMethod != std::string::npos);
        REQUIRE (show < nextMethod);
        REQUIRE (result < show);
        REQUIRE ((castDiagnostic < nextMethod || branchDiagnostic < nextMethod));

        // Tearing down after show() would destroy a GUI that may still draw.
        REQUIRE (source.substr (show, nextMethod - show).find ("close()")
                 == std::string::npos);
    }
}

TEST_CASE ("a failed CLAP embed repaints its error",
           "[clap][editor][regression][issue-361]")
{
    const auto source = readSource ("src/ui/ClapPluginEditorComponent.cpp");
    const auto tryEmbed = source.find ("void ClapPluginEditorComponent::tryEmbed");
    const auto failure = source.find ("lastError = err", tryEmbed);
    const auto repaint = source.find ("repaint()", failure);
    const auto stopTimer = source.find ("stopTimer()", failure);

    REQUIRE (tryEmbed != std::string::npos);
    REQUIRE (failure != std::string::npos);
    REQUIRE (repaint < stopTimer);
}
