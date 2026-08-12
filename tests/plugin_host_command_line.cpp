#include <catch2/catch_test_macros.hpp>

#include "engine/ipc/Utf8CommandLine.h"

#if defined (_WIN32)

#include <string>

// The scan child reads its arguments as UTF-8. Windows builds a narrow argv by
// converting the UTF-16 command line through the process ANSI code page, which
// drops every character that page cannot represent - the plugin path arrives
// mangled, the scan finds nothing, and the parent reports "0 added". These pin
// the re-derivation that avoids it.

TEST_CASE ("Utf8CommandLine keeps a non-ANSI plugin path intact", "[ipc][windows]")
{
    const duskstudio::ipc::Utf8CommandLine cmd (
        L"host.exe --scan \"C:\\Users\\J\u00FCrgen\\VST3\\Bo\u00EEte \u30B7.vst3\"");

    REQUIRE (cmd.argc() == 3);
    REQUIRE (std::string (cmd.argv()[0]) == "host.exe");
    REQUIRE (std::string (cmd.argv()[1]) == "--scan");
    // Quoted argument arrives whole (spaces preserved), in UTF-8 bytes.
    REQUIRE (std::string (cmd.argv()[2])
                 == "C:\\Users\\J\xC3\xBCrgen\\VST3\\Bo\xC3\xAE""te \xE3\x82\xB7.vst3");
}

TEST_CASE ("Utf8CommandLine reports nothing for an empty command line", "[ipc][windows]")
{
    // main() falls back to its own argv when this happens, so the count matters.
    const duskstudio::ipc::Utf8CommandLine cmd (nullptr);
    REQUIRE (cmd.argc() == 0);
}

#endif
