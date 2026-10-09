#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

// The keyboard reference and the zoom-fit tooltips are static text written beside
// the key handling, so nothing ties them to the key that actually fits the view.

#ifndef DUSKSTUDIO_SOURCE_DIR
 #define DUSKSTUDIO_SOURCE_DIR "."
#endif

namespace
{
std::string readSource (const char* relative)
{
    std::ifstream in (std::string (DUSKSTUDIO_SOURCE_DIR) + "/" + relative);
    return { std::istreambuf_iterator<char> (in), std::istreambuf_iterator<char>() };
}
} // namespace

TEST_CASE ("The shortcut list and the tooltips name the plain 0 key that zooms to fit",
           "[ui][regression]")
{
    const auto shell = readSource ("src/ui/MainComponent.cpp");
    REQUIRE_FALSE (shell.empty());
    const auto binding = shell.find ("noMods && ch == '0'");
    REQUIRE (binding != std::string::npos);
    CHECK (shell.find ("tapeStrip->zoomFit()", binding) - binding < 80);

    const auto panel = readSource ("src/ui/ShortcutsPanel.h");
    REQUIRE_FALSE (panel.empty());
    int rows = 0;
    for (auto at = panel.find ("\"Zoom to fit\""); at != std::string::npos; at = panel.find ("\"Zoom to fit\"", at + 1))
    {
        ++rows;
        const auto row = panel.rfind ('{', at);
        REQUIRE (row != std::string::npos);
        CHECK (panel.substr (row, at - row) == "{ \"0\", ");
    }
    CHECK (rows == 2);

    for (const char* file : { "src/ui/MainComponent.cpp", "src/ui/TapeStrip.cpp" })
    {
        INFO (file);
        CHECK (readSource (file).find ("Zoom to fit (Cmd+0)") == std::string::npos);
    }
}
