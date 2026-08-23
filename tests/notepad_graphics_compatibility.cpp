#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include "ui/NotepadGraphicsCompatibility.h"
#include "ui/NativeEditorEmbedScale.h"

#include <fstream>
#include <iterator>
#include <string>

using duskstudio::notepad::GraphicsCompatibility;
using Catch::Approx;

TEST_CASE ("Notepad Cocoa embedding uses the display backing scale",
           "[notepad][scale]")
{
    CHECK (duskstudio::embedscale::factorFromSources (1.0, 1.0, 2.0, true) == Approx (2.0));
    CHECK (duskstudio::embedscale::factorFromSources (1.25, 1.0, 2.0, true) == Approx (2.5));
}

TEST_CASE ("Native embeds retain the peer platform scale outside Cocoa",
           "[notepad][scale]")
{
    CHECK (duskstudio::embedscale::factorFromSources (1.0, 1.5, 2.0, false) == Approx (1.5));
    CHECK (duskstudio::embedscale::factorFromSources (1.25, 1.5, 2.0, false) == Approx (1.875));
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
