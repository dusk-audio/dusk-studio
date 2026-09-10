#pragma once

#include "BuiltinBundle.h"
#include "BuiltinUnit.h"
#include "../hosting/INativeInstance.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::builtin
{
// A built-in unit presented to the mixer as a native plug-in instance, so an
// insert slot drives it through exactly the path a scanned CLAP / LV2 / VST3
// takes. There is no shared library and no negotiation: the port layout is the
// insert's own stereo pair, or a stereo source for an instrument unit.
//
// Threading matches INativeInstance: create / activate / deactivate /
// reactivate / saveState / loadState are message-thread, processBlock is the
// sole audio-thread entry, and the slot fences load and unload with the
// engine's process gate.
class BuiltinInstance final : public hosting::INativeInstance
{
public:
    // NativeInsertSlot construction hook. pluginId is the unit id and must
    // match the bundle - the slot's traits resolve it before we are called.
    bool create (const BuiltinBundle& bundle, const std::string& pluginId,
                 std::string& errorOut);

    const hosting::PortLayout& portLayout() const noexcept override { return layout; }
    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override { return active.load (std::memory_order_acquire); }
    void processBlock (const hosting::PortBuffers& io) noexcept override;
    bool saveState (std::vector<std::uint8_t>& out) const override;
    bool loadState (const std::vector<std::uint8_t>& in) override;
    int  getLatencySamples() const noexcept override;

    std::string displayName() const { return info != nullptr ? info->name : std::string(); }

    // Message thread. The parameter surface an editor and the session drive.
    int paramCount() const noexcept { return unit != nullptr ? unit->paramCount() : 0; }
    const ParamInfo* paramInfo (int index) const noexcept;
    float getParamValue (int index) const noexcept
        { return unit != nullptr ? unit->getParam (index) : 0.0f; }
    void setParamValue (int index, float value) noexcept
        { if (unit != nullptr) unit->setParam (index, value); }

private:
    const UnitInfo* info = nullptr;
    std::string id;
    std::unique_ptr<BuiltinUnit> unit;
    hosting::PortLayout layout;
    std::atomic<bool> active { false };
    int preparedBlockFrames = 0;
};
} // namespace duskstudio::builtin
