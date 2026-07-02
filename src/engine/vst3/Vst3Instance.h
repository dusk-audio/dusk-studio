#pragma once

#include "../hosting/INativeInstance.h"

#include <cstdint>
#include <functional>
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
    // One plugin parameter (snapshot of Vst::ParameterInfo at create()).
    // Values are VST3-normalized (0..1).
    struct ParamInfo
    {
        uint32_t    id = 0;   // Vst::ParamID
        std::string name;
        double      defaultValue = 0.0;
        int         stepCount    = 0;      // 0 = continuous
        bool        canAutomate  = true;
        bool        isReadOnly   = false;
    };

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

    bool saveState (std::vector<uint8_t>& out) const override;
    bool loadState (const std::vector<uint8_t>& in) override;

    // Parameters (message thread). Enumerated once at create().
    int              paramCount() const noexcept;
    const ParamInfo* paramInfo (int index) const noexcept;
    bool getParamValue (uint32_t id, double& out) const;
    bool paramValueToText (uint32_t id, double value, std::string& out) const;

    // Queue a normalized parameter change. SINGLE PRODUCER: message thread only.
    // Reaches the processor via IParameterChanges on the next audio block, and the
    // controller immediately so an open editor tracks the move. Editor-originated
    // edits (IComponentHandler::performEdit) feed the same ring — without that a
    // spec-compliant plugin's knob moves never reach its processor.
    void setParamValue (uint32_t id, double value) noexcept;

    // The editor asked to resize through IPlugFrame (message thread). The active
    // Vst3Editor installs a handler returning whether the host honoured it.
    void setResizeViewHandler (std::function<bool (int, int)> fn);

    // MIDI Learn: index (into paramInfo order) of the parameter the user last
    // moved in the plugin's editor (performEdit). -1 when nothing has been
    // touched. Message thread.
    int lastTouchedParamIndex() const noexcept;

    // True once after the plugin signalled kLatencyChanged (restartComponent).
    // The caller reactivates the instance under the engine gate and recomputes
    // PDC — VST3 only re-reads latency across a setActive cycle.
    bool consumeLatencyChanged() noexcept;

    int getLatencySamples() const noexcept override;

    // The host context this instance hands to the plugin (component handler,
    // run loop, plug frame) — the editor layer pumps and extends it.
    Vst3HostContext&       getHost()       noexcept;
    const Vst3HostContext& getHost() const noexcept;

    // Steinberg::Vst::IEditController* (opaque — the editor layer casts back).
    // Null until create() succeeds; rare plugins expose no controller at all.
    void* editController() const noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::vst3
