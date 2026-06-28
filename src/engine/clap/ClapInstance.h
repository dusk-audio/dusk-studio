#pragma once

#include "ClapHost.h"

#include <clap/events.h>

#include <string>
#include <vector>

struct clap_plugin;

namespace duskstudio::clap
{
class ClapBundle;

// One CLAP plugin instance: create from a factory + plugin id, activate at a
// fixed sample rate / max block, and process stereo audio through it. Manages the
// full lifecycle (init → activate → start_processing → process → stop_processing
// → deactivate → destroy). Setup is message-thread; processStereo is the audio
// path. No parameter / event handling yet — that lands in a later increment.
class ClapInstance
{
public:
    ClapInstance();
    ~ClapInstance();
    ClapInstance (const ClapInstance&)            = delete;
    ClapInstance& operator= (const ClapInstance&) = delete;

    // Create + init the plugin `pluginId` from `bundle`. The bundle owns the
    // dlopen'd .clap whose code backs the plugin's vtable, so it MUST outlive this
    // instance — we keep a reference to make that dependency explicit. Requires a
    // stereo-in / stereo-out plugin (the aux path; relaxed in a later increment).
    // False (+ errorOut) on failure.
    bool create (const ClapBundle& bundle, const std::string& pluginId, std::string& errorOut);

    // Activate at the given config (sizes the process scratch; no later RT alloc).
    bool activate (double sampleRate, int maxBlock, std::string& errorOut);
    void deactivate();

    // Audio thread. Process `numFrames` (<= maxBlock) of stereo through the plugin:
    // inL/inR → outL/outR. A null inR is treated as mono (duplicated). out may not
    // alias in. Starts processing lazily on the first call. No-op if not active or
    // numFrames is out of range.
    void processStereo (const float* inL, const float* inR,
                        float* outL, float* outR, int numFrames) noexcept;

    // Message thread: serialise / restore the plugin's state via the CLAP state
    // extension. saveState appends the plugin's blob to `out`; loadState feeds `in`
    // back. False if the plugin has no state extension or the call fails. Used for
    // session persistence of a native CLAP.
    bool saveState (std::vector<uint8_t>& out) const;
    bool loadState (const std::vector<uint8_t>& in);

    bool isCreated()      const noexcept { return plugin != nullptr; }
    bool isActive()       const noexcept { return active; }
    int  inputChannels()  const noexcept { return inCh; }
    int  outputChannels() const noexcept { return outCh; }

    // For the editor: the live plugin + the host that owns its callback/pump state.
    const ::clap_plugin* getPlugin() const noexcept { return plugin; }
    ClapHost&            getHost()         noexcept  { return hostObj; }

private:
    ClapHost hostObj;
    const ClapBundle*    owningBundle = nullptr;   // must outlive this instance (non-owning)
    const ::clap_plugin* plugin = nullptr;
    bool active     = false;
    bool processing = false;
    bool startFailed = false;   // plugin refused start_processing — stay asleep, don't retry every block
    int  inCh = 0, outCh = 0;
    int  maxFrames = 0;

    // Separate input + output scratch so we never assume the plugin supports
    // in-place processing. Sized in activate().
    std::vector<float> inScratchL, inScratchR, outScratchL, outScratchR;

    // Empty event lists handed to every process() call (no params/events yet).
    // Members rather than function-statics so the audio thread's first process()
    // never trips a thread-safe-static-init guard. Wired in the constructor.
    clap_input_events_t  emptyIn  {};
    clap_output_events_t emptyOut {};
};
} // namespace duskstudio::clap
