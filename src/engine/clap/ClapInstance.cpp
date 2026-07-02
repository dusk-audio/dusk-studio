#include "ClapInstance.h"
#include "ClapBundle.h"

#include <clap/clap.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>

namespace duskstudio::clap
{
ClapInstance::ClapInstance()
{
    // Input event list backed by eventScratch[0..eventCount); reports 0 events when
    // nothing is queued, so it is also the "empty" list. Drained from the ring each
    // process() block. Members (not function-statics) so the audio thread never trips
    // a thread-safe-static-init guard.
    inEvents.ctx  = this;
    inEvents.size = [] (const clap_input_events* l) -> uint32_t
        { return static_cast<const ClapInstance*> (l->ctx)->eventCount; };
    inEvents.get  = [] (const clap_input_events* l, uint32_t idx) -> const clap_event_header_t*
    {
        const auto* self = static_cast<const ClapInstance*> (l->ctx);
        return idx < self->eventCount ? &self->eventScratch[(size_t) idx].header : nullptr;
    };

    emptyOut.ctx      = nullptr;
    emptyOut.try_push = [] (const clap_output_events*, const clap_event_header_t*) -> bool { return true; };
}

ClapInstance::~ClapInstance()
{
    deactivate();
    if (plugin != nullptr)
    {
        plugin->destroy (plugin);
        plugin = nullptr;
    }
}

bool ClapInstance::create (const ClapBundle& bundle, const std::string& pluginId,
                           std::string& errorOut)
{
    const auto* factory = bundle.getFactory();
    if (factory == nullptr || factory->create_plugin == nullptr)
    { errorOut = "no plugin factory"; return false; }
    owningBundle = &bundle;   // the .clap backing the plugin vtable must stay loaded

    plugin = factory->create_plugin (factory, hostObj.get(), pluginId.c_str());
    if (plugin == nullptr) { errorOut = "create_plugin returned null"; return false; }

    if (plugin->init == nullptr || ! plugin->init (plugin))
    {
        errorOut = "plugin init() failed";
        plugin->destroy (plugin);
        plugin = nullptr;
        return false;
    }

    // Channel counts of the first audio port in / out (the aux path is stereo;
    // a full port-config negotiation comes with the multi-port increment).
    int inCh = 0, outCh = 0;
    if (const auto* ap = static_cast<const clap_plugin_audio_ports_t*> (
            plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS));
        ap != nullptr && ap->count != nullptr && ap->get != nullptr)
    {
        clap_audio_port_info_t info {};
        if (ap->count (plugin, true)  > 0 && ap->get (plugin, 0, true,  &info)) inCh  = (int) info.channel_count;
        if (ap->count (plugin, false) > 0 && ap->get (plugin, 0, false, &info)) outCh = (int) info.channel_count;
    }

    // Reject anything but stereo-in / stereo-out for now. The generalized
    // processBlock + InsertAdapter path handles other layouts, but the DSP call
    // sites still assume stereo, so gate non-stereo plugins out here until they migrate.
    if (inCh != 2 || outCh != 2)
    {
        errorOut = "plugin is not stereo-in/stereo-out (got "
                 + std::to_string (inCh) + " in / " + std::to_string (outCh) + " out)";
        plugin->destroy (plugin);
        plugin = nullptr;
        owningBundle = nullptr;
        return false;
    }

    // Record the negotiated shape for INativeInstance consumers (the InsertAdapter
    // reads it to fold the stereo insert onto the plugin's real channel counts).
    layout = {};
    {
        hosting::BusInfo in;
        in.kind = hosting::BusInfo::Kind::Audio; in.dir = hosting::BusInfo::Direction::Input;
        in.role = hosting::BusInfo::Role::Main;  in.channelCount = inCh;  in.active = true; in.name = "Input";
        layout.inputs.push_back (in);
        layout.mainInIndex = 0;

        hosting::BusInfo out;
        out.kind = hosting::BusInfo::Kind::Audio; out.dir = hosting::BusInfo::Direction::Output;
        out.role = hosting::BusInfo::Role::Main;  out.channelCount = outCh; out.active = true; out.name = "Output";
        layout.outputs.push_back (out);
        layout.mainOutIndex = 0;
    }

    // Snapshot the parameter list for automation / display. [main-thread]
    paramsExt = static_cast<const clap_plugin_params_t*> (
        plugin->get_extension (plugin, CLAP_EXT_PARAMS));
    if (paramsExt != nullptr && paramsExt->count != nullptr && paramsExt->get_info != nullptr)
    {
        const uint32_t n = std::min<uint32_t> (paramsExt->count (plugin), 4096u);   // clamp: count is plugin-supplied
        params.reserve (n);
        for (uint32_t i = 0; i < n; ++i)
        {
            clap_param_info_t info {};
            if (! paramsExt->get_info (plugin, i, &info)) continue;
            ParamInfo pi;
            pi.id           = info.id;
            // Bound the copy at the buffer size — don't trust a third-party plugin to
            // NUL-terminate its fixed char[] (an unterminated name would over-read).
            pi.name.assign (info.name, ::strnlen (info.name, sizeof (info.name)));
            pi.minValue     = info.min_value;
            pi.maxValue     = info.max_value;
            pi.defaultValue = info.default_value;
            pi.flags        = info.flags;
            pi.cookie       = info.cookie;
            params.push_back (std::move (pi));
        }
    }
    return true;
}

bool ClapInstance::activate (double sampleRate, int maxBlock, std::string& errorOut)
{
    if (plugin == nullptr) { errorOut = "not created"; return false; }
    if (active) return true;

    maxFrames = std::max (1, maxBlock);
    inScratchL .assign ((size_t) maxFrames, 0.0f);
    inScratchR .assign ((size_t) maxFrames, 0.0f);
    outScratchL.assign ((size_t) maxFrames, 0.0f);
    outScratchR.assign ((size_t) maxFrames, 0.0f);
    eventScratch.assign ((size_t) kParamRingCap, clap_event_param_value_t {});

    if (plugin->activate == nullptr
        || ! plugin->activate (plugin, sampleRate, 1, (uint32_t) maxFrames))
    { errorOut = "activate() failed"; return false; }

    // Plugin latency is valid post-activate; cache it for PDC. Size the CLAP
    // buffer pointer arrays to the negotiated channel counts so processBlock
    // just points them at the caller's scratch each block (no RT alloc).
    latencySamples = 0;
    if (const auto* lat = static_cast<const clap_plugin_latency_t*> (
            plugin->get_extension (plugin, CLAP_EXT_LATENCY));
        lat != nullptr && lat->get != nullptr)
        latencySamples = (int) lat->get (plugin);

    clapInData .assign ((size_t) layout.mainInChannels(),  nullptr);
    clapOutData.assign ((size_t) layout.mainOutChannels(), nullptr);

    startFailed = false;   // a fresh activation may try start_processing again
    active = true;
    return true;
}

bool ClapInstance::reactivate (double sampleRate, int maxBlock, std::string& errorOut)
{
    // deactivate() resets `active`/`processing`/startFailed; activate() re-sizes the
    // scratch + re-arms start_processing. The plugin object (and its open GUI) is never
    // destroyed, so this is safe to run while the editor is showing.
    deactivate();
    return activate (sampleRate, maxBlock, errorOut);
}

void ClapInstance::deactivate()
{
    if (plugin != nullptr && active)
    {
        if (processing && plugin->stop_processing != nullptr)
        {
            // stop_processing is [audio-thread], but deactivate runs on the message
            // thread (the engine process gate has quiesced the real audio thread, so
            // exactly one thread is the "audio thread" right now). Re-designate this
            // thread so the plugin's thread-check inside stop_processing is satisfied.
            hostObj.setAudioThread (std::this_thread::get_id());
            plugin->stop_processing (plugin);
        }
        processing  = false;
        startFailed = false;
        if (plugin->deactivate != nullptr)
            plugin->deactivate (plugin);
    }
    active = false;
}

bool ClapInstance::saveState (std::vector<uint8_t>& out) const
{
    out.clear();
    if (plugin == nullptr) return false;
    const auto* st = static_cast<const clap_plugin_state_t*> (
        plugin->get_extension (plugin, CLAP_EXT_STATE));
    if (st == nullptr || st->save == nullptr) return false;

    clap_ostream_t os {};
    os.ctx   = &out;
    os.write = [] (const clap_ostream_t* s, const void* buffer, uint64_t size) -> int64_t
    {
        // The plugin calls this across a C boundary — an allocation failure in
        // insert() must not throw out of the callback. Report a short write (<0).
        try
        {
            auto* v = static_cast<std::vector<uint8_t>*> (s->ctx);
            const auto* b = static_cast<const uint8_t*> (buffer);
            v->insert (v->end(), b, b + size);
            return (int64_t) size;
        }
        catch (...) { return -1; }
    };
    return st->save (plugin, &os);
}

bool ClapInstance::loadState (const std::vector<uint8_t>& in)
{
    // Don't reject an empty buffer up front — a plugin with zero-byte state is a valid
    // round-trip; let it flow through the stream (read() returns EOF immediately).
    if (plugin == nullptr) return false;
    const auto* st = static_cast<const clap_plugin_state_t*> (
        plugin->get_extension (plugin, CLAP_EXT_STATE));
    if (st == nullptr || st->load == nullptr) return false;

    struct ReadCursor { const uint8_t* data; size_t size; size_t pos; };
    ReadCursor cur { in.data(), in.size(), 0 };

    clap_istream_t is {};
    is.ctx  = &cur;
    is.read = [] (const clap_istream_t* s, void* buffer, uint64_t size) -> int64_t
    {
        auto* c = static_cast<ReadCursor*> (s->ctx);
        const size_t n = std::min ((size_t) size, c->size - c->pos);
        if (n > 0) { std::memcpy (buffer, c->data + c->pos, n); c->pos += n; }
        return (int64_t) n;   // 0 = EOF
    };
    return st->load (plugin, &is);
}

bool ClapInstance::getParamValue (clap_id id, double& out) const
{
    if (plugin == nullptr || paramsExt == nullptr || paramsExt->get_value == nullptr) return false;
    return paramsExt->get_value (plugin, id, &out);
}

bool ClapInstance::paramValueToText (clap_id id, double value, std::string& out) const
{
    if (plugin == nullptr || paramsExt == nullptr || paramsExt->value_to_text == nullptr) return false;
    char buf[256] {};
    if (! paramsExt->value_to_text (plugin, id, value, buf, sizeof (buf))) return false;
    out.assign (buf, ::strnlen (buf, sizeof (buf)));   // bound: don't trust plugin NUL-termination
    return true;
}

void ClapInstance::setParamValue (clap_id id, double value) noexcept
{
    void* cookie = nullptr;
    for (const auto& p : params) if (p.id == id) { cookie = p.cookie; break; }

    const uint32_t w    = ringWrite.load (std::memory_order_relaxed);
    const uint32_t next = (w + 1u) % (uint32_t) kParamRingCap;
    if (next == ringRead.load (std::memory_order_acquire))
        return;   // ring full — drop (only under a pathological flood)
    paramRing[(size_t) w] = { id, value, cookie };
    ringWrite.store (next, std::memory_order_release);
}

void ClapInstance::drainParamRing() noexcept
{
    // Drain queued parameter changes into this block's CLAP event list (audio thread
    // = single consumer). All at time 0 — sample-accurate automation is a later step.
    eventCount = 0;
    uint32_t r = ringRead.load (std::memory_order_relaxed);
    const uint32_t w   = ringWrite.load (std::memory_order_acquire);
    const uint32_t cap = (uint32_t) eventScratch.size();
    while (r != w && eventCount < cap)
    {
        const ParamChange pc = paramRing[(size_t) r];
        auto& ev = eventScratch[(size_t) eventCount++];
        ev.header.size     = sizeof (clap_event_param_value_t);
        ev.header.time     = 0;
        ev.header.space_id = CLAP_CORE_EVENT_SPACE_ID;
        ev.header.type     = CLAP_EVENT_PARAM_VALUE;
        ev.header.flags    = 0;
        ev.param_id        = pc.id;
        ev.cookie          = pc.cookie;
        ev.note_id         = -1;
        ev.port_index      = -1;
        ev.channel         = -1;
        ev.key             = -1;
        ev.value           = pc.value;
        r = (r + 1u) % (uint32_t) kParamRingCap;
    }
    ringRead.store (r, std::memory_order_release);
}

void ClapInstance::processStereo (const float* inL, const float* inR,
                                  float* outL, float* outR, int numFrames) noexcept
{
    if (! active || plugin == nullptr || plugin->process == nullptr
        || numFrames <= 0 || numFrames > maxFrames
        || inL == nullptr || outL == nullptr || outR == nullptr)
    {
        // Clear the outputs so a reused buffer can't leak stale audio on a no-op.
        if (numFrames > 0)
        {
            if (outL != nullptr) std::memset (outL, 0, sizeof (float) * (size_t) numFrames);
            if (outR != nullptr) std::memset (outR, 0, sizeof (float) * (size_t) numFrames);
        }
        return;
    }

    if (startFailed)   // plugin already refused to start — stay silent, don't hammer it every block
    {
        std::memset (outL, 0, sizeof (float) * (size_t) numFrames);
        std::memset (outR, 0, sizeof (float) * (size_t) numFrames);
        return;
    }

    if (! processing)
    {
        // The calling thread is the audio thread for this instance.
        hostObj.setAudioThread (std::this_thread::get_id());
        if (plugin->start_processing != nullptr && ! plugin->start_processing (plugin))
        {
            startFailed = true;
            std::memset (outL, 0, sizeof (float) * (size_t) numFrames);
            std::memset (outR, 0, sizeof (float) * (size_t) numFrames);
            return;
        }
        processing = true;
    }

    drainParamRing();

    const auto n = (size_t) numFrames;
    std::memcpy (inScratchL.data(), inL, sizeof (float) * n);
    std::memcpy (inScratchR.data(), inR != nullptr ? inR : inL, sizeof (float) * n);

    float* inPtrs[2]  = { inScratchL.data(),  inScratchR.data()  };
    float* outPtrs[2] = { outScratchL.data(), outScratchR.data() };

    clap_audio_buffer_t inBuf {};
    inBuf.data32       = inPtrs;
    inBuf.channel_count = 2;

    clap_audio_buffer_t outBuf {};
    outBuf.data32       = outPtrs;
    outBuf.channel_count = 2;

    clap_process_t p {};
    p.steady_time         = -1;          // free-running
    p.frames_count        = (uint32_t) numFrames;
    p.transport           = nullptr;
    p.audio_inputs        = &inBuf;
    p.audio_inputs_count  = 1;
    p.audio_outputs       = &outBuf;
    p.audio_outputs_count = 1;
    p.in_events           = &inEvents;
    p.out_events          = &emptyOut;

    const auto status = plugin->process (plugin, &p);
    if (status == CLAP_PROCESS_ERROR)
    {
        std::fill (outScratchL.begin(), outScratchL.begin() + numFrames, 0.0f);
        std::fill (outScratchR.begin(), outScratchR.begin() + numFrames, 0.0f);
    }

    std::memcpy (outL, outScratchL.data(), sizeof (float) * n);
    std::memcpy (outR, outScratchR.data(), sizeof (float) * n);
}

void ClapInstance::processBlock (const hosting::PortBuffers& io) noexcept
{
    const int numFrames = io.numFrames;

    auto clearOutputs = [&]
    {
        if (io.mainOut == nullptr || numFrames <= 0) return;
        for (int c = 0; c < io.mainOutChannels; ++c)
            if (io.mainOut[c] != nullptr)
                std::memset (io.mainOut[c], 0, sizeof (float) * (size_t) numFrames);
    };

    if (! active || plugin == nullptr || plugin->process == nullptr
        || numFrames <= 0 || numFrames > maxFrames
        || io.mainOut == nullptr || io.mainOutChannels <= 0)
    {
        clearOutputs();
        return;
    }

    if (startFailed)   // plugin already refused to start — stay silent
    {
        clearOutputs();
        return;
    }

    if (! processing)
    {
        hostObj.setAudioThread (std::this_thread::get_id());
        if (plugin->start_processing != nullptr && ! plugin->start_processing (plugin))
        {
            startFailed = true;
            clearOutputs();
            return;
        }
        processing = true;
    }

    drainParamRing();

    // Point the CLAP audio buffers straight at the caller's pre-sized scratch — the
    // plugin writes its output there, no instance-owned copy. Clamp to the channel
    // counts negotiated at activate() so a mismatched block can't over-index; a null
    // mainIn (instrument-style caller) processes as zero input ports.
    const int nin  = io.mainIn != nullptr ? std::min (io.mainInChannels, (int) clapInData.size()) : 0;
    const int nout = std::min (io.mainOutChannels, (int) clapOutData.size());
    for (int c = 0; c < nin;  ++c) clapInData [(size_t) c] = io.mainIn[c];
    for (int c = 0; c < nout; ++c) clapOutData[(size_t) c] = io.mainOut[c];

    clap_audio_buffer_t inBuf {};
    inBuf.data32        = clapInData.data();
    inBuf.channel_count = (uint32_t) nin;

    clap_audio_buffer_t outBuf {};
    outBuf.data32        = clapOutData.data();
    outBuf.channel_count = (uint32_t) nout;

    clap_process_t p {};
    p.steady_time         = -1;          // free-running
    p.frames_count        = (uint32_t) numFrames;
    p.transport           = nullptr;
    p.audio_inputs        = nin > 0 ? &inBuf : nullptr;
    p.audio_inputs_count  = nin > 0 ? 1u : 0u;
    p.audio_outputs       = &outBuf;
    p.audio_outputs_count = 1u;
    p.in_events           = &inEvents;
    p.out_events          = &emptyOut;

    if (plugin->process (plugin, &p) == CLAP_PROCESS_ERROR)
        clearOutputs();
}
} // namespace duskstudio::clap
