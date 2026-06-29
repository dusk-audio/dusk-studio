#pragma once

#include "ClapHost.h"

#include <clap/events.h>

#include <array>
#include <atomic>
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
// path. Parameters are enumerated at create(); changes are queued (message thread,
// single producer) and consumed as CLAP events by the next process() block.
class ClapInstance
{
public:
    // One plugin parameter (snapshot of clap_param_info at create()).
    struct ParamInfo
    {
        clap_id id = 0;
        std::string name;
        double minValue = 0.0, maxValue = 1.0, defaultValue = 0.0;
        clap_param_info_flags flags = 0;
        void* cookie = nullptr;
    };

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

    // Re-activate the SAME plugin at a new sample-rate / block-size (device-rate or
    // oversampling-factor change). Deactivate → activate without destroying the plugin,
    // so an open editor's GUI and the plugin's parameter state both survive. Caller
    // must fence the audio thread (engine process gate) around the call.
    bool reactivate (double sampleRate, int maxBlock, std::string& errorOut);

    // Audio thread. Process `numFrames` (<= maxBlock) of stereo through the plugin:
    // inL/inR → outL/outR. A null inR is treated as mono (duplicated). out may not
    // alias in. Starts processing lazily on the first call. No-op if not active or
    // numFrames is out of range.
    void processStereo (const float* inL, const float* inR,
                        float* outL, float* outR, int numFrames) noexcept;

    // Message thread: serialise / restore the plugin's state via the CLAP state
    // extension. saveState REPLACES `out` with the plugin's blob (clears it first);
    // loadState feeds `in` back. False if the plugin has no state extension or the
    // call fails. Used for session persistence of a native CLAP.
    bool saveState (std::vector<uint8_t>& out) const;
    bool loadState (const std::vector<uint8_t>& in);

    // Parameters (message thread). Enumerated once at create().
    int               paramCount() const noexcept { return (int) params.size(); }
    const ParamInfo*  paramInfo (int index) const noexcept
        { return (index >= 0 && index < (int) params.size()) ? &params[(size_t) index] : nullptr; }
    bool getParamValue    (clap_id id, double& out) const;
    bool paramValueToText (clap_id id, double value, std::string& out) const;

    // Queue a parameter change. SINGLE PRODUCER: call from the message thread only
    // (the ring has no inter-producer synchronisation). Applied on the next process()
    // block — i.e. the next audio buffer while the device runs. Silently dropped only
    // if the ring overflows (pathological flood).
    void setParamValue (clap_id id, double value) noexcept;

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

    // Output event list handed to every process() call. We accept and ignore the
    // plugin's outgoing events for now (try_push returns true). Member, not a
    // function-static, so the audio thread's first process() never trips a
    // thread-safe-static-init guard. Wired in the constructor.
    clap_output_events_t emptyOut {};

    // Parameter automation. Single-producer (message thread, setParamValue) /
    // single-consumer (audio thread, processStereo) ring of pending changes, drained
    // into a per-block CLAP event list. inEvents reports eventCount (0 ⇒ no changes),
    // so it doubles as the "empty" list when nothing is queued.
    struct ParamChange { clap_id id; double value; void* cookie; };
    static constexpr int kParamRingCap = 512;
    std::array<ParamChange, (size_t) kParamRingCap> paramRing {};
    std::atomic<uint32_t> ringWrite { 0 };
    std::atomic<uint32_t> ringRead  { 0 };
    std::vector<clap_event_param_value_t> eventScratch;   // sized in activate(), never RT-grown
    uint32_t             eventCount = 0;
    clap_input_events_t  inEvents {};

    const clap_plugin_params_t* paramsExt = nullptr;
    std::vector<ParamInfo>      params;
};
} // namespace duskstudio::clap
