#pragma once

#include <algorithm>
#include <atomic>
#include <vector>

namespace duskstudio::builtin
{
// One control of a built-in unit. The id is the session-persistent key (the
// index is not: reordering or inserting a parameter must not silently rebind a
// saved value), the name is what a diagnostic or a binding list shows.
struct ParamInfo
{
    const char* id;
    const char* name;
    float minValue;
    float maxValue;
    float defaultValue;
};

// A DSP unit compiled into the app and reachable from an insert slot through
// BuiltinInstance. Parameter storage lives here so every unit persists the same
// way; the DSP itself is the subclass's business.
//
// Threading: the parameter accessors are the message-thread surface, process()
// is the sole audio-thread entry, and the two meet only through the relaxed
// atomics behind paramValue(). prepare() sizes everything; process() allocates
// nothing.
class BuiltinUnit
{
public:
    BuiltinUnit (const ParamInfo* infos, int count)
        : paramInfos (infos), numParams (count), paramValues ((size_t) count)
    {
        for (int i = 0; i < numParams; ++i)
            paramValues[(size_t) i].store (paramInfos[i].defaultValue,
                                           std::memory_order_relaxed);
    }

    virtual ~BuiltinUnit() = default;

    int paramCount() const noexcept { return numParams; }

    const ParamInfo& paramInfo (int index) const noexcept { return paramInfos[index]; }

    float getParam (int index) const noexcept
    {
        if (index < 0 || index >= numParams) return 0.0f;
        return paramValues[(size_t) index].load (std::memory_order_relaxed);
    }

    void setParam (int index, float value) noexcept
    {
        if (index < 0 || index >= numParams) return;
        const auto& info = paramInfos[index];
        paramValues[(size_t) index].store (std::clamp (value, info.minValue, info.maxValue),
                                           std::memory_order_relaxed);
    }

    // Message thread. Size every scratch buffer and seed every smoother.
    // Idempotent - a device-rate change calls it again on the live unit.
    virtual void prepare (double sampleRate, int maxBlockFrames) = 0;

    virtual int latencySamples() const noexcept { return 0; }

    // Audio thread. Stereo, in place.
    virtual void process (float* left, float* right, int numFrames) noexcept = 0;

protected:
    // Audio thread.
    float paramValue (int index) const noexcept
    {
        return paramValues[(size_t) index].load (std::memory_order_relaxed);
    }

private:
    const ParamInfo* paramInfos;
    int numParams;
    std::vector<std::atomic<float>> paramValues;
};
} // namespace duskstudio::builtin
