#pragma once

#include "ClapHost.h"

#include <string>
#include <vector>

struct clap_plugin;
struct clap_plugin_factory;

namespace duskstudio::clap
{
// One CLAP plugin instance: create from a factory + plugin id, activate at a
// fixed sample rate / max block, and process stereo audio through it. Manages the
// full lifecycle (init → activate → start_processing → process → stop_processing
// → deactivate → destroy). Setup is message-thread; processStereo is the audio
// path. No parameter / event handling yet — that lands in a later increment.
class ClapInstance
{
public:
    ClapInstance() = default;
    ~ClapInstance();
    ClapInstance (const ClapInstance&)            = delete;
    ClapInstance& operator= (const ClapInstance&) = delete;

    // Create + init the plugin `pluginId` from `factory`. False (+ errorOut) on failure.
    bool create (const ::clap_plugin_factory* factory, const std::string& pluginId,
                 std::string& errorOut);

    // Activate at the given config (sizes the process scratch; no later RT alloc).
    bool activate (double sampleRate, int maxBlock, std::string& errorOut);
    void deactivate();

    // Audio thread. Process `numFrames` (<= maxBlock) of stereo through the plugin:
    // inL/inR → outL/outR. A null inR is treated as mono (duplicated). out may not
    // alias in. Starts processing lazily on the first call. No-op if not active or
    // numFrames is out of range.
    void processStereo (const float* inL, const float* inR,
                        float* outL, float* outR, int numFrames) noexcept;

    bool isCreated()      const noexcept { return plugin != nullptr; }
    bool isActive()       const noexcept { return active; }
    int  inputChannels()  const noexcept { return inCh; }
    int  outputChannels() const noexcept { return outCh; }

private:
    ClapHost hostObj;
    const ::clap_plugin* plugin = nullptr;
    bool active     = false;
    bool processing = false;
    int  inCh = 0, outCh = 0;
    int  maxFrames = 0;

    // Separate input + output scratch so we never assume the plugin supports
    // in-place processing. Sized in activate().
    std::vector<float> inScratchL, inScratchR, outScratchL, outScratchR;
};
} // namespace duskstudio::clap
