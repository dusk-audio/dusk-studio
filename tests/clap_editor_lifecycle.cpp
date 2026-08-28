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
           "[clap][editor][regression][issue-361]")
{
    const auto source = readSource ("src/engine/clap/ClapEditor.cpp");

    const auto embed = source.find ("bool ClapEditor::embed");
    const auto setParent = source.find ("gui->set_parent", embed);
    const auto mapChildren = source.find ("mapPluginChildren", setParent);
    REQUIRE (embed != std::string::npos);
    REQUIRE (setParent != std::string::npos);
    REQUIRE (mapChildren != std::string::npos);

    // A plugin that builds its window off the message thread parents it after
    // embed() returns, so the one map at embed time is not enough.
    REQUIRE (source.find ("mapPluginChildren", mapChildren + 1) != std::string::npos);
    REQUIRE (source.find ("XMapWindow (dpy, children[i])") != std::string::npos);
}

TEST_CASE ("a late X11 CLAP child revives a container previously judged empty",
           "[clap][editor][regression][issue-361]")
{
    const auto source = readSource ("src/engine/clap/ClapEditor.cpp");
    const auto mapChildren = source.find ("void ClapEditor::mapPluginChildren");
    const auto nextMethod = source.find ("ClapEditor::ContainerContent", mapChildren);
    const auto clearEmpty = source.find ("containerEmpty = false", mapChildren);
    const auto clearPolls = source.find ("untouchedPolls = 0", mapChildren);
    const auto reveal = source.find ("reveal()", mapChildren);

    REQUIRE (mapChildren != std::string::npos);
    REQUIRE (nextMethod != std::string::npos);
    REQUIRE (clearEmpty < nextMethod);
    REQUIRE (clearPolls < nextMethod);
    REQUIRE (reveal < nextMethod);
    REQUIRE (clearEmpty < reveal);
    REQUIRE (clearPolls < reveal);
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
           "[clap][editor][regression][issue-361]")
{
    const auto source = readSource ("src/engine/clap/ClapEditor.cpp");
    const auto embed = source.find ("bool ClapEditor::embed");
    const auto show = source.find ("gui->show", embed);
    REQUIRE (show != std::string::npos);

    // Everything from the show call to the end of embed(): a close() there would
    // destroy a GUI that may well be about to draw, leaving a blank editor.
    const auto embedEnd = source.find ("\n}", show);
    REQUIRE (embedEnd != std::string::npos);
    REQUIRE (source.find ("close()", show) > embedEnd);
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
