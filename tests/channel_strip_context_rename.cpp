#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <optional>
#include <string>

// The embedded context menu closes before it reports a selection. Its close
// schedules a deferred MainComponent focus restore, so opening a Label editor
// synchronously in the selection callback works for a moment and is dismissed
// on the next message-loop tick. Keep the track-rename action deferred after
// that restore. The menu callback's existing SafePointer keeps the deferred
// action from touching a strip that disappeared first.

#ifndef DUSKSTUDIO_SOURCE_DIR
 #define DUSKSTUDIO_SOURCE_DIR "."
#endif

namespace
{
std::optional<std::string> readEntireFile (const std::string& path)
{
    std::ifstream in (path);
    if (! in.is_open()) return {};
    return std::string { std::istreambuf_iterator<char> (in),
                         std::istreambuf_iterator<char>() };
}
} // namespace

TEST_CASE ("track context-menu rename opens after modal focus restoration",
           "[ui][regression][issue-362]")
{
    const auto source = readEntireFile (
        std::string (DUSKSTUDIO_SOURCE_DIR) + "/src/ui/ChannelStripComponent.cpp");
    REQUIRE (source.has_value());

    const auto begin = source->find ("if (result == 1001)");
    REQUIRE (begin != std::string::npos);
    const auto end = source->find ("if (result == 1010)", begin);
    REQUIRE (end != std::string::npos);
    const auto renameAction = source->substr (begin, end - begin);

    const auto defer = renameAction.find ("dusk::callAsync");
    REQUIRE (defer != std::string::npos);
    const auto lambdaOpen = renameAction.find ('{', defer);
    REQUIRE (lambdaOpen != std::string::npos);

    size_t lambdaClose = std::string::npos;
    int braceDepth = 0;
    for (size_t i = lambdaOpen; i < renameAction.size(); ++i)
    {
        if (renameAction[i] == '{') ++braceDepth;
        if (renameAction[i] == '}' && --braceDepth == 0)
        {
            lambdaClose = i;
            break;
        }
    }
    REQUIRE (lambdaClose != std::string::npos);

    const auto show = renameAction.find ("nameLabel.showEditor()");
    REQUIRE (show > lambdaOpen);
    REQUIRE (show < lambdaClose);
    REQUIRE (renameAction.find ("nameLabel.showEditor()", show + 1) == std::string::npos);
}
