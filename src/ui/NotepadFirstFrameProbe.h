#pragma once

#include <filesystem>
#include <fstream>
#include <string>
#include <system_error>
#include <utility>

namespace duskstudio::notepad
{
// A graphics driver that ends the host while presenting the notepad's first
// frame leaves nothing to catch: the D3D12 case exits zero through a graceful
// shutdown, so neither the event-pump guard nor any try block sees it.
// Refusing a renderer by name only covers drivers somebody has already lost a
// session to. This marker is what covers the rest: it is written before the
// first frame is pumped and removed once one has completed, so a run that
// never comes back leaves it behind and the next launch declines to open the
// notepad again.
class FirstFrameProbe
{
public:
    explicit FirstFrameProbe (std::filesystem::path markerFile)
        : marker (std::move (markerFile)) {}

    // The renderer recorded by a run that never completed a frame. Empty when
    // the previous run was fine, and empty when there is nowhere to keep the
    // marker - an unusable config directory costs the protection, not the
    // notepad.
    std::string previousFailure() const
    {
        if (marker.empty())
            return {};
        std::ifstream in (marker);
        if (! in)
            return {};
        std::string renderer;
        std::getline (in, renderer);
        // Accept the tag-first layout used during development of v2 as well as
        // the shipped renderer-first layout below.
        if (renderer == markerVersion)
            std::getline (in, renderer);
        return renderer.empty() ? std::string ("an unidentified renderer") : renderer;
    }

    void arm (const std::string& renderer) const
    {
        if (marker.empty())
            return;
        std::error_code ec;
        std::filesystem::create_directories (marker.parent_path(), ec);
        std::ofstream out (marker, std::ios::trunc);
        // Flushed rather than left to the destructor: the process this guards
        // against dies without unwinding.
        // Keep the renderer on line one so an older release still names it
        // correctly when reading a marker written by this release.
        out << renderer << '\n' << markerVersion << '\n';
        out.flush();
    }

    void disarm() const
    {
        std::error_code ec;
        std::filesystem::remove (marker, ec);
    }

    // Releases before the packaged Windows software renderer wrote only the
    // renderer name. Clear that old-format marker once so the fixed stack gets
    // one attempt; every marker written by this release remains sticky,
    // regardless of renderer, and retains the crash guard on later switches.
    bool clearLegacyPreviousFailure() const
    {
        if (marker.empty())
            return false;
        std::ifstream in (marker);
        if (! in)
            return false;
        std::string firstLine, secondLine;
        std::getline (in, firstLine);
        std::getline (in, secondLine);
        if (firstLine == markerVersion || secondLine == markerVersion)
            return false;
        // Windows fstream handles do not grant FILE_SHARE_DELETE, so the
        // marker cannot be removed until this reader is closed.
        in.close();
        std::error_code ec;
        return std::filesystem::remove (marker, ec);
    }

    const std::filesystem::path& path() const noexcept { return marker; }

private:
    static constexpr const char* markerVersion = "dusk-notepad-first-frame-v2";
    std::filesystem::path marker;
};
} // namespace duskstudio::notepad
