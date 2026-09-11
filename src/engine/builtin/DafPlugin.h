#pragma once

#include "../../foundation/TransportPosition.h"

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio::builtin
{
// One parameter of a DAF plug-in, as the plug-in declared it.
struct DafParamDesc
{
    std::string symbol;
    std::string name;
    std::string unit;
    float minValue     = 0.0f;
    float maxValue     = 1.0f;
    float defaultValue = 0.0f;
    bool isOutput  = false;
    bool isHidden  = false;
    bool isBoolean = false;
    bool isInteger = false;
    // A restricted enumeration: the only values the parameter takes, ascending.
    std::vector<float>       enumValues;
    std::vector<std::string> enumLabels;
};

// One of Dusk's own DAF plug-ins compiled into the app, reached through a plain
// interface so that no DAF header reaches past the plug-in's bridge. Each bridge
// is compiled against its plug-in's DafPluginInfo.h inside a DAF namespace of its
// own, which is what lets several plug-ins share one program.
//
// Threading follows DAF's PluginExporter: activate / deactivate are message-thread;
// setParameterValue, setTimePosition and run belong to the audio thread, or to the
// message thread while the audio thread is fenced.
class DafPlugin
{
public:
    virtual ~DafPlugin() = default;

    virtual const std::vector<DafParamDesc>& params() const noexcept = 0;
    virtual int numInputs() const noexcept = 0;
    virtual int numOutputs() const noexcept = 0;

    virtual void activate (double sampleRate, int maxBlockFrames) = 0;
    virtual void deactivate() = 0;

    virtual float getParameterValue (std::uint32_t index) const noexcept = 0;
    virtual void  setParameterValue (std::uint32_t index, float value) noexcept = 0;
    virtual void  setTimePosition (const dusk::TransportPosition& position) noexcept = 0;
    virtual void  run (const float* const* inputs, float* const* outputs,
                       std::uint32_t frames) noexcept = 0;

    virtual int latencySamples() const noexcept = 0;
};

// Defined by each plug-in's bridge, in the builds that compile it.
std::unique_ptr<DafPlugin> createTapeEcho2();
} // namespace duskstudio::builtin
