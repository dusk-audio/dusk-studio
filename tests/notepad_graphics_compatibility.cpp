#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include "ui/NotepadGraphicsCompatibility.h"
#include "ui/NativeEditorEmbedScale.h"
#include "ui/imgui/DuskImGuiScale.h"

#include <fstream>
#include <iterator>
#include <string>

using duskstudio::notepad::GraphicsCompatibility;
using Catch::Matchers::WithinRel;

TEST_CASE ("Notepad Cocoa embedding uses the display backing scale",
           "[notepad][scale]")
{
    CHECK_THAT (duskstudio::embedscale::factorFromSources (1.0, 1.0, 2.0, true),
                WithinRel (2.0, 1e-12));
    CHECK_THAT (duskstudio::embedscale::factorFromSources (1.25, 1.0, 2.0, true),
                WithinRel (2.5, 1e-12));
}

TEST_CASE ("Native embeds retain the peer platform scale outside Cocoa",
           "[notepad][scale][windows][issue-367]")
{
    CHECK_THAT (duskstudio::embedscale::factorFromSources (1.0, 1.5, 2.0, false),
                WithinRel (1.5, 1e-12));
    CHECK_THAT (duskstudio::embedscale::factorFromSources (1.25, 1.5, 2.0, false),
                WithinRel (1.875, 1e-12));

    constexpr int hostX = 91;
    constexpr int hostY = 47;
    constexpr int modalX = 22;
    constexpr int modalY = 18;
    const auto nested = duskstudio::embedscale::toPhysicalBounds (
        hostX + modalX, hostY + modalY, 641, 359, 1.5);
    CHECK (nested.x == 170);
    CHECK (nested.y == 98);
    CHECK (nested.width == 961);
    CHECK (nested.height == 538);
}

TEST_CASE ("Open notepad geometry responds to display scale changes",
           "[notepad][scale][issue-338]")
{
    const auto standard = duskstudio::embedscale::toPhysicalBounds (20, 15, 940, 700, 1.0);
    const auto retina = duskstudio::embedscale::toPhysicalBounds (20, 15, 940, 700, 2.0);

    CHECK (standard.width == 940);
    CHECK (standard.height == 700);
    CHECK (retina.width == 1880);
    CHECK (retina.height == 1400);
    CHECK_FALSE (duskstudio::imgui::requiresScaleRecreation (2.0, 2.0));
    CHECK (duskstudio::imgui::requiresScaleRecreation (1.0, 2.0));
    CHECK (duskstudio::imgui::requiresScaleRecreation (2.0, 1.0));
}
using duskstudio::notepad::assessGraphicsCompatibility;

TEST_CASE ("Notepad rejects graphics stacks known to end the host process",
           "[notepad][windows]")
{
    CHECK (assessGraphicsCompatibility ("1.1.0", "GDI Generic")
           == GraphicsCompatibility::noOpenGL3);
    CHECK (assessGraphicsCompatibility (
               "4.6 (Compatibility Profile) Mesa 26.2.0",
               "D3D12 (Microsoft Basic Render Driver)")
           == GraphicsCompatibility::unsafeMesaD3D12);
}

TEST_CASE ("Notepad keeps supported hardware and software OpenGL renderers",
           "[notepad][windows]")
{
    CHECK (assessGraphicsCompatibility ("4.1 Metal - 88.1", "Apple M1 Pro")
           == GraphicsCompatibility::supported);
    CHECK (assessGraphicsCompatibility (
               "4.6 (Compatibility Profile) Mesa 26.2.0",
               "llvmpipe (LLVM 20.1.8, 256 bits)")
           == GraphicsCompatibility::supported);
    CHECK (assessGraphicsCompatibility ("4.6.0 NVIDIA 581.80", "NVIDIA GeForce RTX 4070")
           == GraphicsCompatibility::supported);
    // A two-digit major must not read as its first digit, which would refuse a
    // conforming driver as pre-OpenGL-3.
    CHECK (assessGraphicsCompatibility ("10.1", "Some Future GPU")
           == GraphicsCompatibility::supported);
    CHECK (assessGraphicsCompatibility ("OpenGL ES 3.2", "Mali-G78")
           == GraphicsCompatibility::supported);
}

TEST_CASE ("Release notepad source contains no temporary stage tracing",
           "[notepad][release]")
{
    const std::string path = std::string (DUSKSTUDIO_SOURCE_DIR)
                           + "/src/ui/NativeNotepadWindow.cpp";
    std::ifstream input (path);
    REQUIRE (input.good());
    const std::string source { std::istreambuf_iterator<char> (input),
                               std::istreambuf_iterator<char>() };

    CHECK (source.find ("notepadLog (\"stage:") == std::string::npos);
    CHECK (source.find ("loggedFirstIdle") == std::string::npos);
    CHECK (source.find ("loggedFirstDraw") == std::string::npos);
    CHECK (source.find ("tracedFirstDraw") == std::string::npos);
}
