#pragma once

#include "../hosting/INativeInstance.h"

#include <memory>
#include <string>
#include <vector>

namespace duskstudio::vst3
{
class Vst3Bundle;
class Vst3HostContext;

// One VST3 plugin instance driven through the host-agnostic INativeInstance —
// the third implementer after CLAP and LV2, so a plugin slot hosts all formats
// through one pointer. Mirrors ClapInstance/Lv2Instance: create from a bundle +
// class UID, activate at a fixed sample rate / max block, process audio.
// Component + edit controller are both instantiated and connected at create()
// (the controller side feeds state, the editor, and later parameter automation).
//
// Setup/teardown are message-thread and fenced by the caller when live;
// processBlock is the sole audio-thread entry. SDK types stay behind the pImpl.
class Vst3Instance : public hosting::INativeInstance
{
public:
    Vst3Instance();
    ~Vst3Instance() override;
    Vst3Instance (const Vst3Instance&)            = delete;
    Vst3Instance& operator= (const Vst3Instance&) = delete;

    // Resolve `classId` (the factory class UID string) in `bundle`, instantiate
    // component + controller, negotiate buses, and build the PortLayout. The
    // bundle owns the module backing the vtables — it MUST outlive this instance.
    bool create (const Vst3Bundle& bundle, const std::string& classId, std::string& errorOut);

    const hosting::PortLayout& portLayout() const noexcept override;

    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override;

    void processBlock (const hosting::PortBuffers& io) noexcept override;

    // Dual-stream state (component + controller) lands in the next increment.
    bool saveState (std::vector<uint8_t>& out) const override;
    bool loadState (const std::vector<uint8_t>& in) override;

    int getLatencySamples() const noexcept override;

    // The host context this instance hands to the plugin (component handler,
    // run loop, plug frame) — the editor layer pumps and extends it.
    Vst3HostContext&       getHost()       noexcept;
    const Vst3HostContext& getHost() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::vst3
