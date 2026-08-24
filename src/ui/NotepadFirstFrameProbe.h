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
        out << renderer << '\n';
        out.flush();
    }

    void disarm() const
    {
        std::error_code ec;
        std::filesystem::remove (marker, ec);
    }

    const std::filesystem::path& path() const noexcept { return marker; }

private:
    std::filesystem::path marker;
};
} // namespace duskstudio::notepad
