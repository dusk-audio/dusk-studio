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
// re-instantiates the plugin, carrying the full state across (control ports + the
// plugin's state:interface blob via saveState/loadState — lilv state serialized
// as Turtle). The suil editor attaches through the opaque accessors below.
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

    // ── Editor support (message thread; opaque so this header stays lilv-free) ──
    // The LilvPlugin*, the live LilvInstance* (null when inactive), and this
    // instance's URID map/unmap LV2_Features — the UI must share the plugin's
    // URID space, so the editor forwards these instead of building its own.
    void*       lilvWorld()        const noexcept;
    const void* lilvPlugin()       const noexcept;
    void*       lilvInstance()     const noexcept;
    void*       uridMapFeature()   const noexcept;
    void*       uridUnmapFeature() const noexcept;

    // UI → plugin control-port write (ui:floatProtocol). Staged into a lock-free
    // ring; the audio thread applies it at the top of its next processBlock so
    // nothing writes portValues while run() reads it.
    void setControlPortValue (uint32_t portIndex, float value) noexcept;
    // Port index for the suil port-index-by-symbol callback; -1 when unknown.
    int portIndexForSymbol (const char* symbol) const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::lv2
