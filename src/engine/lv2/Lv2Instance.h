#pragma once

#include "../hosting/INativeInstance.h"

#include <memory>
#include <string>
#include <vector>

namespace duskstudio::lv2
{
class Lv2Bundle;

// One LV2 plugin instance driven through the host-agnostic INativeInstance, so a
// plugin slot hosts CLAP, VST3 and LV2 alike. Mirrors ClapInstance: create + init
// from a bundle + URI, activate at a fixed sample rate / block, process audio.
// lilv/lv2 stay behind a pImpl so this header pulls in no LV2 headers.
//
// LV2 fixes the sample rate at instantiate (unlike CLAP's activate), so reactivate
// re-instantiates the plugin; preserving parameter state across that re-instantiate
// is not yet wired. Audio processing only — no state persistence, no editor yet.
class Lv2Instance : public hosting::INativeInstance
{
public:
    Lv2Instance();
    ~Lv2Instance() override;
    Lv2Instance (const Lv2Instance&)            = delete;
    Lv2Instance& operator= (const Lv2Instance&) = delete;

    // Resolve `uri` in `bundle`, classify its ports, and build the PortLayout. The
    // bundle owns the LilvWorld backing the plugin, so it MUST outlive this instance.
    // False (+ errorOut) on failure.
    bool create (const Lv2Bundle& bundle, const std::string& uri, std::string& errorOut);

    const hosting::PortLayout& portLayout() const noexcept override;

    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override;

    void processBlock (const hosting::PortBuffers& io) noexcept override;

    bool saveState (std::vector<uint8_t>& out) const override;
    bool loadState (const std::vector<uint8_t>& in) override;

    int getLatencySamples() const noexcept override;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::lv2
