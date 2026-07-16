#pragma once

#include "ClapHost.h"
#include "../hosting/INativeInstance.h"
#include "../hosting/SpscRing.h"

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
// fixed sample rate / max block, and process audio through it. Manages the
// full lifecycle (init -> activate -> start_processing -> process -> stop_processing
// -> deactivate -> destroy). Setup is message-thread; processBlock is the audio
// path. Parameters are enumerated at create(); changes are queued (message thread,
// single producer) and consumed as CLAP events by the next process() block.
//
// Implements the host-agnostic INativeInstance so one plugin slot can drive
// CLAP, VST3 and LV2 through a single pointer. processStereo() is the legacy
// stereo entry, retained as the reference for verifying processBlock's output.
class ClapInstance : public hosting::INativeInstance
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
    // instance - we keep a reference to make that dependency explicit. Requires a
    // stereo-in / stereo-out plugin (the aux path; relaxed in a later increment).
    // False (+ errorOut) on failure.
    bool create (const ClapBundle& bundle, const std::string& pluginId, std::string& errorOut);

    // The negotiated bus/port shape, populated at create(). [INativeInstance]
    const hosting::PortLayout& portLayout() const noexcept override { return layout; }

    // Activate at the given config (sizes the process scratch; no later RT alloc).
    bool activate (double sampleRate, int maxBlock, std::string& errorOut) override;
    void deactivate() override;

    // Re-activate the SAME plugin at a new sample-rate / block-size (device-rate or
    // oversampling-factor change). Deactivate -> activate without destroying the plugin,
    // so an open editor's GUI and the plugin's parameter state both survive. Caller
    // must fence the audio thread (engine process gate) around the call.
    bool reactivate (double sampleRate, int maxBlock, std::string& errorOut) override;

    bool isActive() const noexcept override { return active; }

    // Audio thread. Bus-aware process entry [INativeInstance]: reads io.mainIn,
    // writes io.mainOut (pointers into the caller's pre-sized scratch - the plugin
    // writes CLAP output directly there, no instance-owned copy). Starts processing
    // lazily on the first call. No-op if not active or io.numFrames is out of range.
    void processBlock (const hosting::PortBuffers& io) noexcept override;

    // Plugin-reported latency in samples at the active config (CLAP_EXT_LATENCY);
    // 0 until activate() or if the plugin has no latency extension. [INativeInstance]
    int getLatencySamples() const noexcept override { return latencySamples; }

    // Audio thread. Legacy stereo entry - inL/inR -> outL/outR through the plugin.
    // Superseded by processBlock (the slot routes production audio through that);
    // retained as the reference implementation for the processBlock equivalence test.
    void processStereo (const float* inL, const float* inR,
                        float* outL, float* outR, int numFrames) noexcept;

    // Message thread: serialise / restore the plugin's state via the CLAP state
    // extension. saveState REPLACES `out` with the plugin's blob (clears it first);
    // loadState feeds `in` back. False if the plugin has no state extension or the
    // call fails. Used for session persistence of a native CLAP.
    bool saveState (std::vector<uint8_t>& out) const override;
    bool loadState (const std::vector<uint8_t>& in) override;

    // Parameters (message thread). Enumerated once at create().
    int               paramCount() const noexcept { return (int) params.size(); }
    const ParamInfo*  paramInfo (int index) const noexcept
        { return (index >= 0 && index < (int) params.size()) ? &params[(size_t) index] : nullptr; }
    bool getParamValue    (clap_id id, double& out) const;
    bool paramValueToText (clap_id id, double value, std::string& out) const;

    // Queue a parameter change. SINGLE PRODUCER: call from the message thread only
    // (the ring has no inter-producer synchronisation). Applied on the next process()
    // block - i.e. the next audio buffer while the device runs. Silently dropped only
    // if the ring overflows (pathological flood).
    void setParamValue (clap_id id, double value) noexcept;

    // MIDI Learn: index (into paramInfo order) of the parameter the user last moved
    // in the plugin's own GUI, stamped from the plugin's outgoing param events.
    // -1 when nothing has been touched. Message thread (scans the param snapshot).
    int lastTouchedParamIndex() const noexcept
    {
        const auto id = lastTouchedParamId.load (std::memory_order_relaxed);
        if (id < 0) return -1;
        for (size_t i = 0; i < params.size(); ++i)
            if ((int64_t) params[i].id == id) return (int) i;
        return -1;
    }

    bool isCreated()      const noexcept { return plugin != nullptr; }
    int  inputChannels()  const noexcept { return layout.mainInChannels(); }
    int  outputChannels() const noexcept { return layout.mainOutChannels(); }

    // For the editor: the live plugin + the host that owns its callback/pump state.
    const ::clap_plugin* getPlugin() const noexcept { return plugin; }
    ClapHost&            getHost()         noexcept  { return hostObj; }

private:
    ClapHost hostObj;
    const ClapBundle*    owningBundle = nullptr;   // must outlive this instance (non-owning)
    const ::clap_plugin* plugin = nullptr;
    bool active     = false;
    bool processing = false;
    bool startFailed = false;   // plugin refused start_processing - stay asleep, don't retry every block
    int  maxFrames = 0;

    hosting::PortLayout layout;   // negotiated at create()
    int latencySamples = 0;       // cached at activate() from CLAP_EXT_LATENCY

    // Separate input + output scratch so we never assume the plugin supports
    // in-place processing. Sized in activate(). Used by the legacy processStereo.
    std::vector<float> inScratchL, inScratchR, outScratchL, outScratchR;

    // Per-channel pointer arrays the CLAP process buffers point at. processBlock
    // fills these from PortBuffers each block (pointers into the caller's scratch),
    // so the plugin writes its output straight there - no instance-owned audio copy.
    // Sized in activate() to the negotiated channel counts.
    std::vector<float*> clapInData, clapOutData;

    // Output event list handed to every process() call. We accept and ignore the
    // plugin's outgoing events for now (try_push returns true). Member, not a
    // function-static, so the audio thread's first process() never trips a
    // thread-safe-static-init guard. Wired in the constructor.
    clap_output_events_t emptyOut {};

    // Parameter automation. UI->RT ring of pending changes (message-thread
    // setParamValue -> audio-thread drain), turned into a per-block CLAP event
    // list. inEvents reports eventCount (0 => no changes), so it doubles as the
    // "empty" list when nothing is queued.
    struct ParamChange { clap_id id; double value; void* cookie; };
    static constexpr uint32_t kParamRingCap = 512;
    hosting::SpscRing<ParamChange, kParamRingCap> paramRing;

    // One slot per staged input event: params from the ring, then the block's
    // notes / raw MIDI. The union keeps inEvents.get's header-pointer contract
    // for every kind. Sized in activate(), never RT-grown.
    union AnyEvent
    {
        clap_event_header_t      header;
        clap_event_param_value_t param;
        clap_event_note_t        note;
        clap_event_midi_t        midi;
    };
    std::vector<AnyEvent> eventScratch;

    // Note-input negotiation (CLAP_EXT_NOTE_PORTS), recorded at create().
    bool noteInPort = false, noteDialectClap = false, noteDialectMidi = false;

    // Audio thread. Convert the block's MidiBuffer into note / raw-MIDI events
    // appended after the drained param changes.
    void appendMidiEvents (const dusk::MidiBuffer& midi) noexcept;
    uint32_t             eventCount = 0;
    clap_input_events_t  inEvents {};

    // Audio thread. Refills eventScratch[0..eventCount) from the param ring; shared
    // by processStereo and processBlock.
    void drainParamRing() noexcept;

    const clap_plugin_params_t* paramsExt = nullptr;
    std::vector<ParamInfo>      params;

    // Last param id the plugin's GUI touched (audio-thread store from the output
    // event sink, message-thread read). -1 = none since create().
    std::atomic<int64_t> lastTouchedParamId { -1 };
};
} // namespace duskstudio::clap
