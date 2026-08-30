#include <catch2/catch_test_macros.hpp>

#include "ui/X11EditorTeardownError.h"

#include <fstream>
#include <optional>
#include <string>

// Regression guard: outside the per-platform PlatformWindowing
// implementations (which deliberately use JUCE's existing Display*
// connection), nothing in Dusk Studio should be calling XOpenDisplay()
// with a nullptr argument to spin up a private X server connection.
//
// Why this matters:
//   • Two Display* connections from the same process to the same
//     X server are visible as separate clients to the compositor.
//   • Mutter's focus-stealing prevention, USER_TIME tracking, and
//     window-manager-hint state are keyed per-connection, so an
//     XSync / property-set on a private connection isn't seen on
//     JUCE's connection - and vice versa.
//   • Under XWayland the second-connection state can corrupt the
//     compositor's per-client tables and trigger a Mutter fault
//     that takes down the desktop session.
//
// PlatformWindowing_Linux.cpp deliberately reuses JUCE's existing
// Display* via juce::XWindowSystem::getInstanceWithoutCreating(),
// which is the cross-connection-safe path. Any callsite outside
// that file that opens a private display is a regression.

namespace
{
// Nullopt on failure so the caller can tell "audited file is gone /
// unreadable" apart from "audited file is clean" - the guard has to fail on
// the former, or renaming an audited path silently retires it.
std::optional<std::string> readEntireFile (const std::string& path)
{
    std::ifstream in (path);
    if (! in.is_open()) return {};

    std::string contents;
    char buffer[4096];
    for (;;)
    {
        in.read (buffer, sizeof buffer);
        const auto count = in.gcount();
        if (count > 0) contents.append (buffer, (std::size_t) count);

        if (in.bad()) return {};
        if (in.eof()) return contents;
        if (in.fail()) return {};
    }
}

// DUSKSTUDIO_SOURCE_DIR is defined by the test target's CMake so the
// test runs from the build directory but still finds the source
// tree.
#ifndef DUSKSTUDIO_SOURCE_DIR
 #define DUSKSTUDIO_SOURCE_DIR "."
#endif

bool containsXOpenDisplayNull (const std::string& contents)
{
    // Match `XOpenDisplay(nullptr)` and `XOpenDisplay (nullptr)`.
    // We don't try to be clever about comments — a literal in a
    // doc comment would also flag, which is fine: the comment
    // should be reworded to refer to the bug, not call the API.
    return contents.find ("XOpenDisplay(nullptr)") != std::string::npos
        || contents.find ("XOpenDisplay (nullptr)") != std::string::npos;
}
} // namespace

TEST_CASE ("XOpenDisplay(nullptr) is confined to PlatformWindowing impls",
            "[x11][anti-pattern]")
{
    // Files that historically held the antipattern - the regression
    // would be one of these growing it back, or a new file picking it
    // up. Add new candidates here as the codebase grows.
    const char* shouldBeClean[] = {
        "src/ui/MainComponent.cpp",
        "src/ui/ChannelStripComponent.cpp",
        "src/ui/AuxLaneComponent.cpp",
        "src/DuskStudioApp.cpp",
    };

    for (const auto* path : shouldBeClean)
    {
        INFO ("file under audit: " << path);
        const auto contents = readEntireFile (std::string (DUSKSTUDIO_SOURCE_DIR) + "/" + path);
        REQUIRE (contents.has_value());
        REQUIRE_FALSE (contents->empty());
        REQUIRE_FALSE (containsXOpenDisplayNull (*contents));
    }
}

TEST_CASE ("X11 editor teardown suppression is request and resource scoped",
           "[x11][issue-375]")
{
    using namespace duskstudio::platform::x11;
    constexpr std::uint64_t editorWindow = 0x1234;

    REQUIRE (shouldSuppressEditorTeardownError (
        kBadWindow, kChangeWindowAttributes, editorWindow, editorWindow));
    REQUIRE (shouldSuppressEditorTeardownError (
        kBadWindow, kReparentWindow, editorWindow, editorWindow));
    REQUIRE (shouldSuppressEditorTeardownError (
        kBadWindow, kUnmapWindow, editorWindow, editorWindow));

    REQUIRE_FALSE (shouldSuppressEditorTeardownError (
        kBadWindow, kUnmapWindow, 0x5678, editorWindow));
    REQUIRE_FALSE (shouldSuppressEditorTeardownError (
        kBadWindow, 42, editorWindow, editorWindow));
    REQUIRE_FALSE (shouldSuppressEditorTeardownError (
        2, kUnmapWindow, editorWindow, editorWindow)); // BadValue
    REQUIRE_FALSE (shouldSuppressEditorTeardownError (
        8, kUnmapWindow, editorWindow, editorWindow)); // BadMatch
}
