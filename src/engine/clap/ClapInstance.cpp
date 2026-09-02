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

    // Output sink: events are accepted and dropped, EXCEPT that parameter
    // value/gesture events stamp the last-touched id (audio thread, relaxed) so
    // MIDI Learn can bind "the knob the user just moved in the plugin's GUI".
    emptyOut.ctx      = this;
    emptyOut.try_push = [] (const clap_output_events* list, const clap_event_header_t* ev) -> bool
    {
        if (ev != nullptr && ev->space_id == CLAP_CORE_EVENT_SPACE_ID
            && (ev->type == CLAP_EVENT_PARAM_VALUE || ev->type == CLAP_EVENT_PARAM_GESTURE_BEGIN))
        {
            auto* self = static_cast<ClapInstance*> (const_cast<void*> (list->ctx));
            const auto id = ev->type == CLAP_EVENT_PARAM_VALUE
                              ? reinterpret_cast<const clap_event_param_value_t*> (ev)->param_id
                              : reinterpret_cast<const clap_event_param_gesture_t*> (ev)->param_id;
            self->lastTouchedParamId.store ((int64_t) id, std::memory_order_relaxed);
        }
        return true;
    };
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

    // Snapshot every advertised audio port while the plugin is deactivated.
    // The mixer exposes one main insert bus, but CLAP process() still requires
    // the complete port array. Complex modular plugins commonly advertise
    // several stereo buses and may dereference all of them.
    audioInPorts.clear();
    audioOutPorts.clear();
    mainAudioInPort = mainAudioOutPort = -1;
    bool audioPortsOk = true;
    if (const auto* ap = static_cast<const clap_plugin_audio_ports_t*> (
            plugin->get_extension (plugin, CLAP_EXT_AUDIO_PORTS));
        ap != nullptr && ap->count != nullptr && ap->get != nullptr)
    {
        auto readPorts = [&] (bool isInput, std::vector<AudioPort>& ports,
                              int& mainPort)
        {
            constexpr uint32_t kMaxPorts = 64;
            constexpr uint32_t kMaxChannelsPerPort = 64;
            constexpr uint32_t kMaxTotalChannels = 256;
            const uint32_t count = ap->count (plugin, isInput);
            if (count > kMaxPorts)
            {
                errorOut = "plugin advertises too many audio ports";
                return false;
            }

            uint32_t totalChannels = 0;
            ports.reserve (count);
            for (uint32_t i = 0; i < count; ++i)
            {
                clap_audio_port_info_t info {};
                if (! ap->get (plugin, i, isInput, &info)
                    || info.channel_count == 0
                    || info.channel_count > kMaxChannelsPerPort
                    || totalChannels > kMaxTotalChannels - info.channel_count)
                {
                    errorOut = "plugin advertises an invalid audio-port layout";
                    return false;
                }

                // Some plugins incorrectly mark more than one port as main.
                // Preserve the historical first-port selection and keep every
                // later port connected as an auxiliary bus.
                const bool isMain = (info.flags & CLAP_AUDIO_PORT_IS_MAIN) != 0
                                 && mainPort < 0;
                if (isMain)
                    mainPort = (int) i;

                AudioPort port;
                port.channelCount = info.channel_count;
                port.channelOffset = totalChannels;
                port.isMain = isMain;
                port.name.assign (info.name, ::strnlen (info.name, sizeof (info.name)));
                ports.push_back (std::move (port));
                totalChannels += info.channel_count;
            }

            // Older plugins sometimes omit the main flag. Preserve the host's
            // historical first-port behavior while still connecting all buses.
            if (! ports.empty() && mainPort < 0)
            {
                mainPort = 0;
                ports.front().isMain = true;
            }
            return true;
        };

        audioPortsOk = readPorts (true, audioInPorts, mainAudioInPort)
                    && readPorts (false, audioOutPorts, mainAudioOutPort);
    }

    if (! audioPortsOk)
    {
        plugin->destroy (plugin);
        plugin = nullptr;
        owningBundle = nullptr;
        return false;
    }

    // Note input (instruments / MIDI-driven effects) via CLAP_EXT_NOTE_PORTS.
    // Prefer the plugin's CLAP note dialect for note on/off; raw MIDI covers
    // everything else when the port accepts it.
    noteInPort      = false;
    noteDialectClap = false;
    noteDialectMidi = false;
    primaryNoteInputPort = 0;
    clapNoteInputPorts.clear();
    if (const auto* np = static_cast<const clap_plugin_note_ports_t*> (
            plugin->get_extension (plugin, CLAP_EXT_NOTE_PORTS));
        np != nullptr && np->count != nullptr && np->get != nullptr
        && np->count (plugin, true) > 0)
    {
        const uint32_t count = std::min<uint32_t> (np->count (plugin, true), 32768u);
        for (uint32_t index = 0; index < count; ++index)
        {
            clap_note_port_info_t info {};
            if (! np->get (plugin, index, true, &info)) continue;
            if (! noteInPort)
            {
                noteInPort = true;
                primaryNoteInputPort = (int16_t) index;
                noteDialectClap = (info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0;
                noteDialectMidi = (info.supported_dialects & CLAP_NOTE_DIALECT_MIDI) != 0;
            }
            if ((info.supported_dialects & CLAP_NOTE_DIALECT_CLAP) != 0)
                clapNoteInputPorts.push_back ((int16_t) index);
        }
    }

    // The InsertAdapter folds any layout onto the stereo insert; the only hard
    // requirements are an audio output and, for input-less plugins, a note port
    // to drive them (a plugin with neither is unhostable in a mixer slot).
    if (mainAudioOutPort < 0 || (mainAudioInPort < 0 && ! noteInPort))
    {
        errorOut = mainAudioOutPort < 0 ? "plugin has no audio output"
                                        : "plugin has no audio input and no note input";
        plugin->destroy (plugin);
        plugin = nullptr;
        owningBundle = nullptr;
        return false;
    }

    // Record the negotiated shape for INativeInstance consumers (the InsertAdapter
    // reads it to fold the stereo insert onto the plugin's real channel counts).
    layout = {};
    {
        for (size_t i = 0; i < audioInPorts.size(); ++i)
        {
            const auto& port = audioInPorts[i];
            hosting::BusInfo in;
            in.kind = hosting::BusInfo::Kind::Audio; in.dir = hosting::BusInfo::Direction::Input;
            in.role = (int) i == mainAudioInPort ? hosting::BusInfo::Role::Main
                                                 : hosting::BusInfo::Role::Aux;
            in.channelCount = (int) port.channelCount;
            in.active = true;
            in.name = port.name.empty() ? "Input" : port.name;
            if ((int) i == mainAudioInPort)
                layout.mainInIndex = (int) layout.inputs.size();
            layout.inputs.push_back (in);
        }
        if (noteInPort)
        {
            hosting::BusInfo ev;
            ev.kind = hosting::BusInfo::Kind::Event; ev.dir = hosting::BusInfo::Direction::Input;
            ev.role = hosting::BusInfo::Role::Main;  ev.carriesMidi = true; ev.active = true; ev.name = "Notes";
            layout.eventInIndex = (int) layout.inputs.size();
            layout.inputs.push_back (ev);
        }

        for (size_t i = 0; i < audioOutPorts.size(); ++i)
        {
            const auto& port = audioOutPorts[i];
            hosting::BusInfo out;
            out.kind = hosting::BusInfo::Kind::Audio; out.dir = hosting::BusInfo::Direction::Output;
            out.role = (int) i == mainAudioOutPort ? hosting::BusInfo::Role::Main
                                                   : hosting::BusInfo::Role::Aux;
            out.channelCount = (int) port.channelCount;
            out.active = true;
            out.name = port.name.empty() ? "Output" : port.name;
            if ((int) i == mainAudioOutPort)
                layout.mainOutIndex = (int) layout.outputs.size();
            layout.outputs.push_back (out);
        }
        layout.isInstrument = mainAudioInPort < 0;
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
            // Bound the copy at the buffer size - don't trust a third-party plugin to
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
    // Params, a block's ordinary MIDI, and the extra fan-out required when the
    // transport's 16-channel panic becomes one NOTE_CHOKE per CLAP note port.
    constexpr size_t kMidiEventCapacity = 512;
    constexpr size_t kMidiChannelCount = 16;
    const size_t panicFanOut = kMidiChannelCount * clapNoteInputPorts.size();
    eventScratch.assign ((size_t) kParamRingCap + kMidiEventCapacity + panicFanOut,
                         AnyEvent {});

    if (plugin->activate == nullptr
        || ! plugin->activate (plugin, sampleRate, 1, (uint32_t) maxFrames))
    { errorOut = "activate() failed"; return false; }

    // Plugin latency is valid post-activate; cache it for PDC.
    latencySamples = 0;
    if (const auto* lat = static_cast<const clap_plugin_latency_t*> (
            plugin->get_extension (plugin, CLAP_EXT_LATENCY));
        lat != nullptr && lat->get != nullptr)
        latencySamples = (int) lat->get (plugin);

    // Allocate a valid buffer for every channel of every advertised port. The
    // main port is repointed to the insert adapter at process time; all extra
    // ports remain connected to these silence/sink buffers.
    auto prepareAudioPorts = [this] (const std::vector<AudioPort>& ports,
                                     std::vector<clap_audio_buffer_t>& buffers,
                                     std::vector<float*>& channelData,
                                     std::vector<float>& scratch)
    {
        size_t totalChannels = 0;
        for (const auto& port : ports) totalChannels += port.channelCount;

        buffers.assign (ports.size(), clap_audio_buffer_t {});
        channelData.assign (totalChannels, nullptr);
        scratch.assign (totalChannels * (size_t) maxFrames, 0.0f);
        for (size_t i = 0; i < ports.size(); ++i)
        {
            const auto& port = ports[i];
            for (uint32_t c = 0; c < port.channelCount; ++c)
            {
                const size_t channel = (size_t) port.channelOffset + c;
                channelData[channel] = scratch.data() + channel * (size_t) maxFrames;
            }
            buffers[i].data32 = channelData.data() + port.channelOffset;
            buffers[i].channel_count = port.channelCount;
        }
    };
    prepareAudioPorts (audioInPorts, clapInBuffers, clapInData, clapInScratch);
    prepareAudioPorts (audioOutPorts, clapOutBuffers, clapOutData, clapOutScratch);

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
            // thread so the plugin's thread-check inside stop_processing is satisfied,
            // then hand the stamp back - process() only re-stamps on its next start,
            // which a bypassed slot never reaches.
            const auto previousAudioThread =
                hostObj.exchangeAudioThread (std::this_thread::get_id());
            plugin->stop_processing (plugin);
            hostObj.restoreAudioThread (previousAudioThread);
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
        // The plugin calls this across a C boundary - an allocation failure in
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
    // Don't reject an empty buffer up front - a plugin with zero-byte state is a valid
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
    if (! st->load (plugin, &is)) return false;

    // Restored state can select a mode with different lookahead (linear phase,
    // oversampling), so the value cached at activate() no longer describes the
    // plugin. Re-read it or the host compensates by the wrong amount.
    // clap_plugin_latency.get is [main-thread & active].
    if (active)
    {
        if (const auto* lat = static_cast<const clap_plugin_latency_t*> (
                plugin->get_extension (plugin, CLAP_EXT_LATENCY));
            lat != nullptr && lat->get != nullptr)
            latencySamples = (int) lat->get (plugin);
    }

    return true;
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
    paramRing.push ({ id, value, cookie });   // full => drop (pathological flood only)
}

void ClapInstance::drainParamRing() noexcept
{
    // Drain queued parameter changes into this block's CLAP event list (audio thread
    // = single consumer). All at time 0 - sample-accurate automation is a later step.
    eventCount = 0;
    paramRing.drain ([this] (const ParamChange& pc)
    {
        auto& ev = eventScratch[(size_t) eventCount++].param;
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
    }, (uint32_t) eventScratch.size());
}

void ClapInstance::appendMidiEvents (const dusk::MidiBuffer& midi) noexcept
{
    if (! noteInPort) return;
    const uint32_t cap = (uint32_t) eventScratch.size();
    for (const auto meta : midi)
    {
        if (eventCount >= cap) break;   // scratch full - drop the tail
        const auto* d = meta.data;
        if (meta.numBytes < 1) continue;
        const auto status  = (uint8_t) (d[0] & 0xF0u);
        const auto channel = (int16_t) (d[0] & 0x0Fu);
        const bool noteOn  = status == 0x90 && meta.numBytes >= 3 && d[2] > 0;
        const bool noteOff = meta.numBytes >= 3
                             && (status == 0x80 || (status == 0x90 && d[2] == 0));
        const bool allSoundOff = status == 0xB0 && meta.numBytes >= 3 && d[1] == 120;

        if (! clapNoteInputPorts.empty() && allSoundOff)
        {
            for (const int16_t port : clapNoteInputPorts)
            {
                if (eventCount >= cap) break;
                auto& ev = eventScratch[(size_t) eventCount].note;
                ev.header = { (uint32_t) sizeof (clap_event_note_t),
                              (uint32_t) meta.samplePosition, CLAP_CORE_EVENT_SPACE_ID,
                              CLAP_EVENT_NOTE_CHOKE, 0 };
                ev.note_id    = -1;
                ev.port_index = port;
                ev.channel    = channel;
                ev.key        = -1;
                ev.velocity   = 0.0;
                ++eventCount;
            }
            if (! noteDialectMidi) continue;
            if (eventCount >= cap) break;
        }

        if (noteDialectClap && (noteOn || noteOff))
        {
            auto& slot = eventScratch[(size_t) eventCount];
            auto& ev = slot.note;
            ev.header = { (uint32_t) sizeof (clap_event_note_t),
                          (uint32_t) meta.samplePosition, CLAP_CORE_EVENT_SPACE_ID,
                          (uint16_t) (noteOn ? CLAP_EVENT_NOTE_ON : CLAP_EVENT_NOTE_OFF), 0 };
            ev.note_id    = -1;
            ev.port_index = primaryNoteInputPort;
            ev.channel    = channel;
            ev.key        = (int16_t) d[1];
            ev.velocity   = (double) d[2] / 127.0;
            ++eventCount;
        }
        else if (noteDialectMidi && meta.numBytes <= 3 && status >= 0x80 && status <= 0xE0)
        {
            auto& slot = eventScratch[(size_t) eventCount];
            // Everything else (CC, pitch bend, aftertouch - and the notes
            // themselves when the plugin only takes raw MIDI).
            auto& ev = slot.midi;
            ev.header = { (uint32_t) sizeof (clap_event_midi_t),
                          (uint32_t) meta.samplePosition, CLAP_CORE_EVENT_SPACE_ID,
                          CLAP_EVENT_MIDI, 0 };
            ev.port_index = (uint16_t) primaryNoteInputPort;
            ev.data[0] = d[0];
            ev.data[1] = (uint8_t) (meta.numBytes > 1 ? d[1] : 0);
            ev.data[2] = (uint8_t) (meta.numBytes > 2 ? d[2] : 0);
            ++eventCount;
        }
    }
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

    const auto n = (size_t) numFrames;
    const int mainInputs = layout.mainInChannels();
    if (mainInputs == 1)
    {
        const float* right = inR != nullptr ? inR : inL;
        for (int i = 0; i < numFrames; ++i)
            inScratchL[(size_t) i] = 0.5f * (inL[i] + right[i]);
    }
    else if (mainInputs >= 2)
    {
        std::memcpy (inScratchL.data(), inL, sizeof (float) * n);
        std::memcpy (inScratchR.data(), inR != nullptr ? inR : inL, sizeof (float) * n);
    }
    std::fill (outScratchL.begin(), outScratchL.begin() + numFrames, 0.0f);
    std::fill (outScratchR.begin(), outScratchR.begin() + numFrames, 0.0f);

    float* inPtrs[2]  = { inScratchL.data(),  inScratchR.data()  };
    float* outPtrs[2] = { outScratchL.data(), outScratchR.data() };
    hosting::PortBuffers io;
    io.mainIn = mainInputs > 0 ? inPtrs : nullptr;
    io.mainInChannels = std::min (mainInputs, 2);
    io.mainOut = outPtrs;
    io.mainOutChannels = std::min (layout.mainOutChannels(), 2);
    io.numFrames = numFrames;
    processBlock (io);

    std::memcpy (outL, outScratchL.data(), sizeof (float) * n);
    std::memcpy (outR,
                 layout.mainOutChannels() == 1 ? outScratchL.data() : outScratchR.data(),
                 sizeof (float) * n);
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

    if (startFailed)   // plugin already refused to start - stay silent
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
    if (io.midiIn != nullptr)
        appendMidiEvents (*io.midiIn);

    // Point every port at caller storage or its owned silence/sink buffer.
    // Missing input channels and auxiliary inputs are cleared every block;
    // discarded output scratch does not need a redundant clear.
    auto resetPortBuffers = [numFrames, this] (const std::vector<AudioPort>& ports,
                                               std::vector<clap_audio_buffer_t>& buffers,
                                               std::vector<float*>& channelData,
                                               std::vector<float>& scratch,
                                               int mainPort,
                                               float* const* supplied,
                                               int suppliedChannels,
                                               bool clearOwned)
    {
        for (size_t i = 0; i < ports.size(); ++i)
        {
            const auto& port = ports[i];
            for (uint32_t c = 0; c < port.channelCount; ++c)
            {
                const size_t channel = (size_t) port.channelOffset + c;
                float* const owned = scratch.data() + channel * (size_t) maxFrames;
                const bool useSupplied = (int) i == mainPort
                                      && supplied != nullptr
                                      && (int) c < suppliedChannels
                                      && supplied[c] != nullptr;
                if (useSupplied)
                    channelData[channel] = supplied[c];
                else
                {
                    if (clearOwned)
                        std::fill (owned, owned + numFrames, 0.0f);
                    channelData[channel] = owned;
                }
            }
            buffers[i].data32 = channelData.data() + port.channelOffset;
            buffers[i].channel_count = port.channelCount;
        }
    };
    resetPortBuffers (audioInPorts, clapInBuffers, clapInData, clapInScratch,
                      mainAudioInPort, io.mainIn, io.mainInChannels, true);
    resetPortBuffers (audioOutPorts, clapOutBuffers, clapOutData, clapOutScratch,
                      mainAudioOutPort, io.mainOut, io.mainOutChannels, false);

    clap_process_t p {};
    p.steady_time         = -1;          // free-running
    p.frames_count        = (uint32_t) numFrames;
    p.transport           = nullptr;
    p.audio_inputs        = clapInBuffers.empty() ? nullptr : clapInBuffers.data();
    p.audio_inputs_count  = (uint32_t) clapInBuffers.size();
    p.audio_outputs       = clapOutBuffers.data();
    p.audio_outputs_count = (uint32_t) clapOutBuffers.size();
    p.in_events           = &inEvents;
    p.out_events          = &emptyOut;

    if (plugin->process (plugin, &p) == CLAP_PROCESS_ERROR)
        clearOutputs();
}
} // namespace duskstudio::clap
