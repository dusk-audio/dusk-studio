#pragma once

#include "BuiltinBundle.h"
#include "BuiltinUnit.h"
#include "DafUnitInstance.h"
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
// insert's own stereo pair, or a stereo source for an instrument unit. A unit
// that is one of Dusk's DAF plug-ins runs through a DafUnitInstance, which this
// forwards to; everything else here is the knob-unit path.
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

    const hosting::PortLayout& portLayout() const noexcept override
        { return dafUnit != nullptr ? dafUnit->portLayout() : layout; }
    bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    void deactivate() override;
    bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) override;
    bool isActive() const noexcept override
        { return dafUnit != nullptr ? dafUnit->isActive() : active.load (std::memory_order_acquire); }
    void processBlock (const hosting::PortBuffers& io) noexcept override;
    bool saveState (std::vector<std::uint8_t>& out) const override;
    bool loadState (const std::vector<std::uint8_t>& in) override;
    int  getLatencySamples() const noexcept override;

    std::string displayName() const { return info != nullptr ? info->name : std::string(); }

    // Message thread. The parameter surface an editor and the session drive.
    // The plug-in's own editor, for a unit that is one of Dusk's DAF plug-ins.
    // A knob unit has none and is drawn from its parameter table instead.
    bool hasPluginEditor() const noexcept
        { return dafUnit != nullptr && dafUnit->hasEditor(); }
    std::uint32_t pluginEditorWidth() const noexcept
        { return dafUnit != nullptr ? dafUnit->editorWidth() : 0; }
    std::uint32_t pluginEditorHeight() const noexcept
        { return dafUnit != nullptr ? dafUnit->editorHeight() : 0; }
    std::unique_ptr<DafEditor> createPluginEditor (std::uintptr_t nativeParent,
                                                   std::uint32_t width, std::uint32_t height,
                                                   double scaleFactor,
                                                   DafEditorCallbacks callbacks,
                                                   std::string& errorOut)
    {
        if (dafUnit == nullptr)
        {
            errorOut = "this unit has no plug-in editor.";
            return nullptr;
        }
        return dafUnit->createEditor (nativeParent, width, height, scaleFactor,
                                      std::move (callbacks), errorOut);
    }

    int paramCount() const noexcept;
    const ParamInfo* paramInfo (int index) const noexcept;
    float getParamValue (int index) const noexcept;
    void setParamValue (int index, float value) noexcept;

private:
    const UnitInfo* info = nullptr;
    std::string id;
    std::unique_ptr<BuiltinUnit> unit;
    std::unique_ptr<DafUnitInstance> dafUnit;
    hosting::PortLayout layout;
    std::atomic<bool> active { false };
    int preparedBlockFrames = 0;
};
} // namespace duskstudio::builtin
