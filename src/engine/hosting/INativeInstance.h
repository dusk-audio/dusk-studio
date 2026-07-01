#pragma once

#include "PortBuffers.h"
#include "PortLayout.h"

#include <cstdint>
#include <string>
#include <vector>

namespace duskstudio::hosting
{
// The format-agnostic contract a plugin slot drives, generalising the
// lifecycle ClapInstance already implements so VST3 / LV2 / CLAP instances are
// interchangeable behind one pointer. Construction stays concrete (each format's
// create() takes its own bundle + id / URI); this interface begins once the
// plugin exists. Parameter enumeration + automation are added by a later phase
// (they pull in NativeParamInfo); this base is the audio + lifecycle + state
// contract that the DSP spine needs first.
//
// Threading, matching the CLAP host: activate / deactivate / reactivate /
// saveState / loadState are message-thread; processBlock is the sole audio-thread
// entry. The slot fences load/unload with the engine process gate.
class INativeInstance
{
public:
    virtual ~INativeInstance() = default;

    // The negotiated bus/port shape. Valid after the concrete create(); stable
    // until deactivate(). Message-thread read — the audio thread uses the slot's
    // cached copy, not this vtable.
    virtual const PortLayout& portLayout() const noexcept = 0;

    // Message thread. Size every process-scratch buffer here; no RT allocation
    // afterwards. False (+ errorOut) on failure.
    virtual bool activate (double sampleRate, int maxBlockFrames, std::string& errorOut) = 0;
    virtual void deactivate() = 0;

    // Re-activate the SAME instance at a new sample-rate / block-size (device-rate
    // or oversampling change) without destroying it, so an open editor survives.
    // The caller fences the audio thread. LV2 re-instantiates internally (state
    // saved + restored) because lilv fixes the rate at instantiate.
    virtual bool reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut) = 0;

    virtual bool isActive() const noexcept = 0;

    // Audio thread. Process one block. Every buffer in `io` is pre-sized; no
    // allocation or locking here. No-op if inactive or io.numFrames is out of range.
    virtual void processBlock (const PortBuffers& io) noexcept = 0;

    // Message thread. Opaque state blob for session persistence. saveState
    // REPLACES out. Formats with more than one state stream (VST3 component +
    // controller) pack them length-prefixed into this single blob.
    virtual bool saveState (std::vector<uint8_t>& out) const = 0;
    virtual bool loadState (const std::vector<uint8_t>& in) = 0;

    // Plugin-reported processing latency in samples at the active config; 0 until
    // activate(). Fed into the engine's plugin-delay-compensation aggregator.
    virtual int getLatencySamples() const noexcept = 0;
};
} // namespace duskstudio::hosting
