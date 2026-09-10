#pragma once

#include "../../foundation/MidiBuffer.h"

#include <algorithm>
#include <atomic>
#include <vector>

namespace duskstudio::builtin
{
// What kind of control a parameter wants. A unit's editor is generic, so the
// shape of each row comes from here rather than from a per-unit panel.
enum class ParamKind
{
    Continuous,   // a slider, formatted with the suffix
    Toggle,       // off / on, stored as 0 or 1
    Choice,       // one of `choices`, stored as the index
};

// One control of a built-in unit. The id is the session-persistent key (the
// index is not: reordering or inserting a parameter must not silently rebind a
// saved value), the name is what a diagnostic, a binding list and the editor
// show. `section` groups rows under a heading; parameters carrying the same
// section must be contiguous.
//
// Aggregate on purpose: a unit declares its whole surface as one static table.
struct ParamInfo
{
    const char* id;
    const char* name;
    const char* section;
    const char* suffix;
    float minValue;
    float maxValue;
    float defaultValue;
    ParamKind kind = ParamKind::Continuous;
    const char* const* choices = nullptr;   // Choice only
    int choiceCount = 0;                    // Choice only
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

    // Audio thread. Stereo, in place. `midi` carries the block's events for a
    // unit that consumes them and is null for an effect insert, which the mixer
    // never routes MIDI to; an effect unit ignores it.
    virtual void process (float* left, float* right, int numFrames,
                          const dusk::MidiBuffer* midi) noexcept = 0;

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
