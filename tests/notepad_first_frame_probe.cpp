#include <catch2/catch_test_macros.hpp>

#include "ui/imgui/FirstFrameProbe.h"

#include <filesystem>
#include <fstream>

using duskstudio::imgui::FirstFrameProbe;

namespace
{
namespace stdfs = std::filesystem;

// Each case gets its own directory so a leftover marker cannot leak between
// them, and so the arm() path exercises directory creation.
stdfs::path scratchDir (const char* name)
{
    const auto dir = stdfs::temp_directory_path() / "dusk-first-frame-probe" / name;
    std::error_code ec;
    stdfs::remove_all (dir, ec);
    return dir;
}
} // namespace

TEST_CASE ("A run that completes a frame leaves nothing behind", "[notepad][probe]")
{
    const auto marker = scratchDir ("clean") / "notepad-first-frame";
    const FirstFrameProbe probe { marker };

    CHECK (probe.previousFailure().empty());
    probe.arm ("Apple M1 Pro");
    CHECK (stdfs::exists (marker));
    probe.disarm();
    CHECK_FALSE (stdfs::exists (marker));

    const FirstFrameProbe nextLaunch { marker };
    CHECK (nextLaunch.previousFailure().empty());
}

TEST_CASE ("A run that never comes back is reported to the next launch",
           "[notepad][probe]")
{
    const auto marker = scratchDir ("died") / "notepad-first-frame";
    const FirstFrameProbe probe { marker };
    probe.arm ("D3D12 (Microsoft Basic Render Driver)");

    // No disarm: the process died presenting the first frame.
    const FirstFrameProbe nextLaunch { marker };
    CHECK (nextLaunch.previousFailure() == "D3D12 (Microsoft Basic Render Driver)");

    // The refusal stands until the marker is removed, so a second bad launch
    // cannot cost another session.
    const FirstFrameProbe launchAfterThat { marker };
    CHECK_FALSE (launchAfterThat.previousFailure().empty());
}

TEST_CASE ("A marker with no renderer recorded still refuses", "[notepad][probe]")
{
    const auto dir = scratchDir ("empty");
    std::error_code ec;
    stdfs::create_directories (dir, ec);
    const auto marker = dir / "notepad-first-frame";
    std::ofstream { marker } << "\n";

    CHECK (FirstFrameProbe { marker }.previousFailure() == "an unidentified renderer");
}

TEST_CASE ("A renderer upgrade clears only a marker from an older release",
           "[notepad][probe]")
{
    const auto marker = scratchDir ("renderer-upgrade") / "notepad-first-frame";
    const FirstFrameProbe probe { marker };

    std::error_code ec;
    stdfs::create_directories (marker.parent_path(), ec);
    std::ofstream { marker } << "D3D12 (Microsoft Basic Render Driver)\n";
    CHECK (probe.clearLegacyPreviousFailure());
    CHECK (probe.previousFailure().empty());

    probe.arm ("llvmpipe (LLVM 22.1.8, 256 bits)");
    CHECK_FALSE (probe.clearLegacyPreviousFailure());
    CHECK (stdfs::exists (marker));

    // A later opt-out to a hardware driver must not disable the guard: this is
    // a new-format marker even though the renderer does not contain llvmpipe.
    probe.arm ("Some Future GPU");
    CHECK_FALSE (probe.clearLegacyPreviousFailure());
    CHECK (stdfs::exists (marker));
    std::ifstream markerContents { marker };
    std::string firstLine;
    std::getline (markerContents, firstLine);
    CHECK (firstLine == "Some Future GPU");
}

TEST_CASE ("Nowhere to keep the marker costs the guard, not the notepad",
           "[notepad][probe]")
{
    const FirstFrameProbe probe { stdfs::path {} };
    probe.arm ("Some renderer");
    CHECK (probe.previousFailure().empty());
    probe.disarm();
}
