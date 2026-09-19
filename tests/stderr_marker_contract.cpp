// The regression runner's black-box legs decide PASS/FAIL by grepping stderr
// markers out of the real app. A marker that gets reworded still compiles and
// still runs, so the leg silently stops asserting anything. These cases read
// the sources and pin every grepped string, so a rename fails ctest here
// instead of turning a leg into a no-op.

#include <catch2/catch_test_macros.hpp>

#include <nlohmann/json.hpp>

#include <cctype>
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

std::size_t occurrences (const std::string& text, const std::string& needle)
{
    std::size_t found = 0;
    for (std::size_t at = text.find (needle); at != std::string::npos;
         at = text.find (needle, at + needle.size()))
        ++found;
    return found;
}

// Markers are written as adjacent string literals wrapped across source lines,
// while the runner greps the assembled line. Fusing the literals lets a case
// pin the text the leg actually sees rather than one arbitrary fragment of it.
std::string joinAdjacentLiterals (const std::string& source)
{
    std::string out;
    out.reserve (source.size());

    for (std::size_t at = 0; at < source.size();)
    {
        // A '"' character literal would otherwise read as an opening quote and
        // shift every literal after it.
        if (source[at] == '\'')
        {
            const std::size_t close = source[at + 1] == '\\' ? at + 3 : at + 2;
            if (close < source.size() && source[close] == '\'')
            {
                out.append (source, at, close + 1 - at);
                at = close + 1;
                continue;
            }
        }

        if (source[at] != '"')
        {
            out += source[at++];
            continue;
        }

        out += '"';
        for (;;)
        {
            ++at;
            while (at < source.size() && source[at] != '"')
            {
                if (source[at] == '\\' && at + 1 < source.size())
                    out += source[at++];
                out += source[at++];
            }
            if (at >= source.size())
                break;

            std::size_t next = at + 1;
            while (next < source.size()
                   && std::isspace (static_cast<unsigned char> (source[next])) != 0)
                ++next;

            if (next >= source.size() || source[next] != '"')
            {
                ++at;
                break;
            }
            at = next;
        }
        out += '"';
    }

    return out;
}
} // namespace

TEST_CASE ("Staged shutdown prints every phase marker the quit legs assert on",
           "[markers][shutdown]")
{
    // The tag and the phase names live apart: one fprintf in the engine seam,
    // the names at the call sites that emit them.
    REQUIRE (joinAdjacentLiterals (readSource ("src/engine/ShutdownPhases.cpp"))
                 .find ("[Dusk Studio/shutdown] %s")
             != std::string::npos);

    const auto source = joinAdjacentLiterals (readSource ("src/ui/MainComponent.cpp"));

    for (const char* phase : { "re-entry ignored: shutdown already in progress",
                               "phase 1: stop autosave timer",
                               "phase 1b: close native session notepad",
                               "phase 2: stop transport (commits in-flight recording)",
                               "phase 3: detach audio callback",
                               "phase 3: audio callback already detached (skipping)",
                               "phase 3b: release plugin resources (setActive(false) on each)",
                               "phase 4: drop plugin editor windows",
                               "phase 5: flush window operations",
                               "phase 5b: clear keyboard focus from every top-level window",
                               "phase 6: hide main window",
                               "phase 7: defer systemRequestedQuit to next message-loop tick",
                               "phase 7b: posting systemRequestedQuit",
                               "phase 8: beginSafeShutdown returning to message loop (yield to mutter)" })
    {
        INFO ("shutdown phase marker: " << phase);
        REQUIRE (source.find (phase) != std::string::npos);
    }

    // The Windows session-close phase passes on at least eight phase lines, so
    // collapsing the sequence into fewer markers has to fail here first.
    REQUIRE (occurrences (source, "phase ") >= 8);
}

TEST_CASE ("Session load prints the timing line the load legs match on", "[markers][load]")
{
    const auto source = joinAdjacentLiterals (readSource ("src/ui/MainComponent.cpp"));

    REQUIRE (source.find ("[Dusk Studio/Load] %s: parse=") != std::string::npos);
    REQUIRE (source.find ("total=%dms") != std::string::npos);
}

TEST_CASE ("Every single-instance path that gives up the slot keeps its stderr wording",
           "[markers][single-instance]")
{
    const auto source = joinAdjacentLiterals (readSource ("src/util/SingleInstance.cpp"));

    REQUIRE (source.find ("[Dusk Studio/SingleInstance] ") != std::string::npos);

    SECTION ("POSIX")
    {
        REQUIRE (source.find ("XDG_RUNTIME_DIR is unset or not an absolute path")
                 != std::string::npos);
        REQUIRE (source.find (" - starting without the single-instance slot; this window "
                              "will not receive sessions opened from the desktop")
                 != std::string::npos);
        REQUIRE (source.find ("the runtime directory could not be created") != std::string::npos);
        REQUIRE (source.find ("the socket a crashed instance left behind could not be removed")
                 != std::string::npos);
        REQUIRE (source.find ("the single-instance listener could not start") != std::string::npos);
        REQUIRE (source.find ("could not reach the running instance") != std::string::npos);
    }

    SECTION ("Windows")
    {
        REQUIRE (source.find ("[Dusk Studio/SingleInstance] the single-instance listener could "
                              "not start (Windows error %lu); this window will not receive "
                              "sessions opened from the desktop")
                 != std::string::npos);
        REQUIRE (source.find ("[Dusk Studio/SingleInstance] could not reach the running instance "
                              "(Windows error %lu) - starting without the single-instance slot; "
                              "this window will not receive sessions opened from the desktop")
                 != std::string::npos);
        REQUIRE (source.find ("[Dusk Studio/SingleInstance] the single-instance pipe belongs to "
                              "another user's process - starting without the single-instance "
                              "slot; this window will not receive sessions opened from the desktop")
                 != std::string::npos);
    }
}

TEST_CASE ("Sandboxed plugin slots keep the OOP wording the child-kill legs grep",
           "[markers][oop]")
{
    const auto source = joinAdjacentLiterals (readSource ("src/engine/PluginSlot.cpp"));

    REQUIRE (source.find ("[Dusk Studio/PluginSlot] OOP child process exited; slot "
                          "auto-bypassed. Reload the plugin to recover.")
             != std::string::npos);
    REQUIRE (source.find ("[Dusk Studio/PluginSlot] OOP child crashed; mirror detached")
             != std::string::npos);
    REQUIRE (source.find ("[Dusk Studio/PluginSlot] OOP connect failed (") != std::string::npos);
    REQUIRE (source.find ("[Dusk Studio/PluginSlot] OOP loadPlugin failed (") != std::string::npos);
    REQUIRE (source.find ("[Dusk Studio/PluginSlot] OOP requested but host binary not found")
             != std::string::npos);
    REQUIRE (source.find ("[Dusk Studio/PluginSlot] OOP showEditor failed: ") != std::string::npos);
    REQUIRE (source.find ("[Dusk Studio/PluginSlot] ~PluginSlot: shell wrapper still")
             != std::string::npos);
}

TEST_CASE ("A native editor that refuses to attach reports under its own tag", "[markers][editor]")
{
    const auto source
        = joinAdjacentLiterals (readSource ("src/engine/hosting/NativeRestorePolicy.h"));

    REQUIRE (source.find ("[Dusk Studio/native editor] ") != std::string::npos);
}

TEST_CASE ("Screenshot capture prints the terminal marker the capture leg waits for",
           "[markers][capture]")
{
    const auto source = joinAdjacentLiterals (readSource ("src/ui/ScreenshotCapture.cpp"));

    REQUIRE (source.find ("[Dusk Studio/capture] done") != std::string::npos);
}

TEST_CASE ("The regression runner's minimal session stays loadable by this build",
           "[markers][session]")
{
    const auto serializer = readSource ("src/session/SessionSerializer.cpp");
    const std::string declaration = "constexpr int kFormatVersion = ";
    const auto at = serializer.find (declaration);
    REQUIRE (at != std::string::npos);
    const int formatVersion = std::stoi (serializer.substr (at + declaration.size()));

    const auto session = nlohmann::json::parse (
        readSource ("scripts/regress/sessions/minimal/session.json"), nullptr, false);
    REQUIRE (session.is_object());
    REQUIRE (session.contains ("version"));
    REQUIRE (session["version"].is_number_integer());

    // The loader refuses anything newer than its own format, so the checked-in
    // session has to stay at or below it. Older is fine: migrateSession lifts it.
    REQUIRE (session["version"].get<int>() <= formatVersion);
}
