#include "Lv2Instance.h"
#include "Lv2Bundle.h"

#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_map>
#include <vector>

namespace duskstudio::lv2
{
struct Lv2Instance::Impl
{
    // ── URID map/unmap host feature (a simple intern table) ──
    std::unordered_map<std::string, uint32_t> uridForUri;
    std::vector<std::string> uriForUrid { std::string() };   // index 0 unused (URIDs start at 1)
    LV2_URID_Map   mapFeature   {};
    LV2_URID_Unmap unmapFeature {};
    LV2_Feature    mapFeatureStruct     {};
    LV2_Feature    unmapFeatureStruct   {};
    LV2_Feature    optionsFeatureStruct {};
    LV2_Feature    boundedFeatureStruct {};
    std::vector<const LV2_Feature*> features;

    // Backing storage for the options feature — must outlive the instance, which
    // keeps pointers to these. Rebuilt per activate() with the live SR / block size.
    int32_t optMinBlock = 1, optMaxBlock = 0, optNominalBlock = 0;
    float   optSampleRate = 0.0f;
    std::vector<LV2_Options_Option> options;

    static LV2_URID mapUri (LV2_URID_Map_Handle handle, const char* uri)
    {
        auto* self = static_cast<Impl*> (handle);
        const std::string key = uri;
        if (auto it = self->uridForUri.find (key); it != self->uridForUri.end())
            return it->second;
        const auto id = (uint32_t) self->uriForUrid.size();
        self->uriForUrid.push_back (key);
        self->uridForUri.emplace (key, id);
        return id;
    }
    static const char* unmapUri (LV2_URID_Unmap_Handle handle, LV2_URID urid)
    {
        auto* self = static_cast<Impl*> (handle);
        return (urid < self->uriForUrid.size()) ? self->uriForUrid[urid].c_str() : nullptr;
    }

    void buildUridFeatures()
    {
        mapFeature.handle   = this;  mapFeature.map     = &Impl::mapUri;
        unmapFeature.handle = this;  unmapFeature.unmap = &Impl::unmapUri;
        mapFeatureStruct   = { LV2_URID__map,   &mapFeature };
        unmapFeatureStruct = { LV2_URID__unmap, &unmapFeature };
        uridFloat  = mapUri (this, LV2_ATOM__Float);
        uridDouble = mapUri (this, LV2_ATOM__Double);
        uridInt    = mapUri (this, LV2_ATOM__Int);
        uridLong   = mapUri (this, LV2_ATOM__Long);
    }
    LV2_URID uridFloat = 0, uridDouble = 0, uridInt = 0, uridLong = 0;

    // Assemble the full feature list for instantiate: urid map/unmap + the block-size
    // and sample-rate options + boundedBlockLength. JUCE/DPF-wrapped plugins REQUIRE
    // options + boundedBlockLength and refuse to instantiate without them.
    void assembleFeatures (double sr, int maxBlock)
    {
        optMinBlock = 1;
        optMaxBlock = maxBlock;
        optNominalBlock = maxBlock;
        optSampleRate = (float) sr;

        const LV2_URID kInt   = mapUri (this, LV2_ATOM__Int);
        const LV2_URID kFloat = mapUri (this, LV2_ATOM__Float);
        options = {
            { LV2_OPTIONS_INSTANCE, 0, mapUri (this, LV2_BUF_SIZE__minBlockLength),     sizeof (int32_t), kInt,   &optMinBlock },
            { LV2_OPTIONS_INSTANCE, 0, mapUri (this, LV2_BUF_SIZE__maxBlockLength),     sizeof (int32_t), kInt,   &optMaxBlock },
            { LV2_OPTIONS_INSTANCE, 0, mapUri (this, LV2_BUF_SIZE__nominalBlockLength), sizeof (int32_t), kInt,   &optNominalBlock },
            { LV2_OPTIONS_INSTANCE, 0, mapUri (this, LV2_PARAMETERS__sampleRate),       sizeof (float),   kFloat, &optSampleRate },
            { LV2_OPTIONS_INSTANCE, 0, 0, 0, 0, nullptr },   // terminator
        };
        optionsFeatureStruct = { LV2_OPTIONS__options, options.data() };
        boundedFeatureStruct = { LV2_BUF_SIZE__boundedBlockLength, nullptr };
        features = { &mapFeatureStruct, &unmapFeatureStruct,
                     &optionsFeatureStruct, &boundedFeatureStruct, nullptr };
    }

    // ── plugin + instance ──
    LilvWorld*        world  = nullptr;   // owned by the bundle
    const LilvPlugin* plugin = nullptr;   // owned by the bundle's world
    LilvInstance*     instance = nullptr;
    bool   active = false;
    double sampleRate = 0.0;
    int    maxFrames = 0;
    std::atomic<int> latencySamples { 0 };

    hosting::PortLayout layout;

    // Port classification (indices into the plugin's port list).
    std::vector<uint32_t> audioInPorts, audioOutPorts, controlPorts, atomInPorts, atomOutPorts;
    std::vector<uint32_t> otherPorts;   // CV / unclassified — LV2 requires every port connected
    int latencyPortIndex = -1;

    // Per-port scratch backing otherPorts: maxFrames floats each, so an audio-rate
    // CV port can safely read silence or sink its output.
    std::vector<std::vector<float>> otherScratch;

    // Baseline audio-port buffers: every audio port is wired to these at activate()
    // so the LV2 every-port-connected invariant holds even if a caller supplies
    // fewer channels than the layout advertises; processBlock re-points the main
    // channels each block. Inputs share the silence, outputs share the sink.
    std::vector<float> audioSilence, audioSink;

    // UI → audio-thread control-port writes. The UI must not store into portValues
    // directly (run() reads it concurrently); writes stage here and processBlock
    // drains them before run(). Same SPSC shape as the CLAP host's param ring.
    struct PortWrite { uint32_t idx; float value; };
    static constexpr uint32_t kWriteRingCap = 256;
    std::array<PortWrite, kWriteRingCap> writeRing {};
    std::atomic<uint32_t> writeRingW { 0 }, writeRingR { 0 };

    // Message-thread shadow of the UI writes. A bypassed slot never runs
    // processBlock, so the ring may never drain — state saves and reactivate read
    // UI-touched ports from here instead of waiting on a drain that may not come.
    // Audio thread never touches these.
    std::vector<float>   uiShadow;
    std::vector<uint8_t> uiDirty;

    // Persistent per-port float storage; control ports are connected to these once
    // and hold their (default) value across blocks. Sized to the port count.
    std::vector<float> portValues;

    // One atom buffer per atom port: inputs first (empty sequences), then outputs
    // (chunk-capacity buffers). atomChunkType re-advertises output capacity per block.
    std::vector<std::vector<uint8_t>> atomBuffers;
    LV2_URID atomChunkType = 0;

    void freeInstance()
    {
        if (instance != nullptr)
        {
            if (active) lilv_instance_deactivate (instance);
            lilv_instance_free (instance);
            instance = nullptr;
        }
        active = false;
    }

    // Filled at create(); lilv's state save/restore resolves every port by symbol,
    // so this lookup runs once per port per save — keep it O(1).
    std::unordered_map<std::string, uint32_t> portIndexBySymbol;

    int portIndexForSymbol (const char* symbol) const
    {
        if (symbol == nullptr) return -1;
        const auto it = portIndexBySymbol.find (symbol);
        return it != portIndexBySymbol.end() ? (int) it->second : -1;
    }

    // lilv state callbacks (message thread; save/restore are fenced by the caller
    // when the instance is live, same contract as activate/deactivate).
    static const void* getPortValue (const char* symbol, void* userData,
                                     uint32_t* size, uint32_t* type)
    {
        auto* self = static_cast<Impl*> (userData);
        const int idx = self->portIndexForSymbol (symbol);
        if (idx < 0 || (size_t) idx >= self->portValues.size())
        { *size = 0; *type = 0; return nullptr; }
        *size = sizeof (float);
        *type = self->uridFloat;
        // UI writes staged while the slot never ran (bypassed) live in the shadow;
        // portValues would still hold the pre-tweak value.
        if ((size_t) idx < self->uiDirty.size() && self->uiDirty[(size_t) idx] != 0)
            return &self->uiShadow[(size_t) idx];
        return &self->portValues[(size_t) idx];
    }

    static void setPortValue (const char* symbol, void* userData,
                              const void* value, uint32_t size, uint32_t type)
    {
        auto* self = static_cast<Impl*> (userData);
        const int idx = self->portIndexForSymbol (symbol);
        if (idx < 0 || (size_t) idx >= self->portValues.size() || value == nullptr)
            return;

        // States written by other hosts may carry any numeric atom type.
        float v = 0.0f;
        if      (type == self->uridFloat  && size >= sizeof (float))   v = *static_cast<const float*> (value);
        else if (type == self->uridDouble && size >= sizeof (double))  v = (float) *static_cast<const double*> (value);
        else if (type == self->uridInt    && size >= sizeof (int32_t)) v = (float) *static_cast<const int32_t*> (value);
        else if (type == self->uridLong   && size >= sizeof (int64_t)) v = (float) *static_cast<const int64_t*> (value);
        else return;
        if (! std::isfinite (v)) return;
        self->portValues[(size_t) idx] = v;
        // A restore supersedes any staged UI value for this port.
        if ((size_t) idx < self->uiDirty.size())
        {
            self->uiShadow[(size_t) idx] = v;
            self->uiDirty [(size_t) idx] = 0;
        }
    }
};

Lv2Instance::Lv2Instance() : impl (std::make_unique<Impl>()) { impl->buildUridFeatures(); }
Lv2Instance::~Lv2Instance() { impl->freeInstance(); }

const hosting::PortLayout& Lv2Instance::portLayout() const noexcept { return impl->layout; }
bool Lv2Instance::isActive() const noexcept { return impl->active; }
int  Lv2Instance::getLatencySamples() const noexcept { return impl->latencySamples.load (std::memory_order_relaxed); }

bool Lv2Instance::create (const Lv2Bundle& bundle, const std::string& uri, std::string& errorOut)
{
    impl->freeInstance();
    impl->plugin = static_cast<const LilvPlugin*> (bundle.pluginByUri (uri));
    if (impl->plugin == nullptr) { errorOut = "plugin URI not found in bundle: " + uri; return false; }

    auto* world = static_cast<LilvWorld*> (bundle.world());
    impl->world = world;
    LilvNode* audioClass   = lilv_new_uri (world, LV2_CORE__AudioPort);
    LilvNode* controlClass = lilv_new_uri (world, LV2_CORE__ControlPort);
    LilvNode* inputClass   = lilv_new_uri (world, LV2_CORE__InputPort);
    LilvNode* atomClass    = lilv_new_uri (world, LV2_ATOM__AtomPort);
    LilvNode* latencyDesig = lilv_new_uri (world, LV2_CORE__latency);

    impl->audioInPorts.clear();  impl->audioOutPorts.clear();
    impl->controlPorts.clear();  impl->atomInPorts.clear(); impl->atomOutPorts.clear();
    impl->otherPorts.clear();
    impl->portIndexBySymbol.clear();
    impl->uiShadow.clear();
    impl->uiDirty.clear();
    impl->latencyPortIndex = -1;

    const uint32_t numPorts = lilv_plugin_get_num_ports (impl->plugin);
    for (uint32_t i = 0; i < numPorts; ++i)
    {
        const LilvPort* port = lilv_plugin_get_port_by_index (impl->plugin, i);
        const bool isInput = lilv_port_is_a (impl->plugin, port, inputClass);
        if (const LilvNode* sym = lilv_port_get_symbol (impl->plugin, port))
            impl->portIndexBySymbol.emplace (lilv_node_as_string (sym), i);

        if (lilv_port_is_a (impl->plugin, port, audioClass))
            (isInput ? impl->audioInPorts : impl->audioOutPorts).push_back (i);
        else if (lilv_port_is_a (impl->plugin, port, controlClass))
            impl->controlPorts.push_back (i);
        else if (lilv_port_is_a (impl->plugin, port, atomClass))
            (isInput ? impl->atomInPorts : impl->atomOutPorts).push_back (i);
        else
            impl->otherPorts.push_back (i);   // CV / unknown → scratch in activate()
    }

    // The port designated lv2:latency (an output control port) reports plugin
    // latency; read it after run() for PDC.
    LilvNode* outputClass = lilv_new_uri (world, LV2_CORE__OutputPort);
    if (const LilvPort* latPort = lilv_plugin_get_port_by_designation (impl->plugin, outputClass, latencyDesig))
        impl->latencyPortIndex = (int) lilv_port_get_index (impl->plugin, latPort);
    lilv_node_free (outputClass);

    // Build the host-agnostic layout the InsertAdapter reads.
    impl->layout = {};
    if (! impl->audioInPorts.empty())
    {
        hosting::BusInfo in;
        in.kind = hosting::BusInfo::Kind::Audio; in.dir = hosting::BusInfo::Direction::Input;
        in.role = hosting::BusInfo::Role::Main;  in.channelCount = (int) impl->audioInPorts.size();
        in.active = true; in.name = "Input";
        impl->layout.inputs.push_back (in);
        impl->layout.mainInIndex = 0;
    }
    if (! impl->atomInPorts.empty())
    {
        hosting::BusInfo ev;
        ev.kind = hosting::BusInfo::Kind::Event; ev.dir = hosting::BusInfo::Direction::Input;
        ev.role = hosting::BusInfo::Role::Main;  ev.carriesMidi = true; ev.active = true; ev.name = "Events";
        impl->layout.eventInIndex = (int) impl->layout.inputs.size();
        impl->layout.inputs.push_back (ev);
    }
    if (! impl->audioOutPorts.empty())
    {
        hosting::BusInfo out;
        out.kind = hosting::BusInfo::Kind::Audio; out.dir = hosting::BusInfo::Direction::Output;
        out.role = hosting::BusInfo::Role::Main;  out.channelCount = (int) impl->audioOutPorts.size();
        out.active = true; out.name = "Output";
        impl->layout.mainOutIndex = 0;
        impl->layout.outputs.push_back (out);
    }
    impl->layout.isInstrument = (impl->audioInPorts.empty()
                                 && ! impl->atomInPorts.empty()
                                 && ! impl->audioOutPorts.empty());

    lilv_node_free (audioClass);   lilv_node_free (controlClass);
    lilv_node_free (inputClass);   lilv_node_free (atomClass);
    lilv_node_free (latencyDesig);
    return true;
}

bool Lv2Instance::activate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    if (impl->plugin == nullptr) { errorOut = "not created"; return false; }
    if (impl->active) return true;

    impl->sampleRate = sampleRate;
    impl->maxFrames  = std::max (1, maxBlockFrames);

    impl->assembleFeatures (sampleRate, impl->maxFrames);   // options need the live SR/block
    impl->instance = lilv_plugin_instantiate (impl->plugin, sampleRate, impl->features.data());
    if (impl->instance == nullptr) { errorOut = "lilv_plugin_instantiate failed"; return false; }

    const uint32_t numPorts = lilv_plugin_get_num_ports (impl->plugin);

    // Control ports: connect each to a persistent float initialised to its default.
    // The UI shadow survives a reactivate of the same plugin (same port count) so
    // staged-but-undrained writes aren't lost across a rate change.
    if (impl->uiShadow.size() != (size_t) numPorts)
    {
        impl->uiShadow.assign ((size_t) numPorts, 0.0f);
        impl->uiDirty .assign ((size_t) numPorts, 0);
    }
    impl->portValues.assign ((size_t) numPorts, 0.0f);
    lilv_plugin_get_port_ranges_float (impl->plugin, nullptr, nullptr, impl->portValues.data());
    for (uint32_t idx : impl->controlPorts)
    {
        if (std::isnan (impl->portValues[idx])) impl->portValues[idx] = 0.0f;
        lilv_instance_connect_port (impl->instance, idx, &impl->portValues[(size_t) idx]);
    }

    // Atom ports: input gets an empty sequence, output a chunk-capacity buffer, so
    // an effect that declares them doesn't run against unconnected memory.
    const LV2_URID seqType   = Impl::mapUri (impl.get(), LV2_ATOM__Sequence);
    const LV2_URID chunkType = Impl::mapUri (impl.get(), LV2_ATOM__Chunk);
    impl->atomChunkType = chunkType;
    impl->atomBuffers.clear();
    auto connectAtom = [&] (uint32_t idx, bool input)
    {
        constexpr size_t kCap = 8192;
        impl->atomBuffers.emplace_back (kCap, (uint8_t) 0);
        auto* buf = impl->atomBuffers.back().data();
        auto* atom = reinterpret_cast<LV2_Atom*> (buf);
        if (input)
        {
            auto* seq = reinterpret_cast<LV2_Atom_Sequence*> (buf);
            seq->atom.type = seqType;
            seq->atom.size = sizeof (LV2_Atom_Sequence_Body);   // empty: header only
            seq->body.unit = 0; seq->body.pad = 0;
        }
        else
        {
            atom->type = chunkType;
            atom->size = (uint32_t) (kCap - sizeof (LV2_Atom));   // advertise capacity
        }
        lilv_instance_connect_port (impl->instance, idx, buf);
    };
    for (uint32_t idx : impl->atomInPorts)  connectAtom (idx, true);
    for (uint32_t idx : impl->atomOutPorts) connectAtom (idx, false);

    // CV / unclassified ports: LV2 requires every port connected before run(), so
    // each gets its own block-sized scratch (silence in, sink out).
    impl->otherScratch.clear();
    impl->otherScratch.reserve (impl->otherPorts.size());
    for (uint32_t idx : impl->otherPorts)
    {
        impl->otherScratch.emplace_back ((size_t) impl->maxFrames, 0.0f);
        lilv_instance_connect_port (impl->instance, idx, impl->otherScratch.back().data());
    }

    // Baseline audio-port wiring (see Impl::audioSilence) — processBlock overrides
    // the main channels every block.
    impl->audioSilence.assign ((size_t) impl->maxFrames, 0.0f);
    impl->audioSink.assign ((size_t) impl->maxFrames, 0.0f);
    for (uint32_t idx : impl->audioInPorts)
        lilv_instance_connect_port (impl->instance, idx, impl->audioSilence.data());
    for (uint32_t idx : impl->audioOutPorts)
        lilv_instance_connect_port (impl->instance, idx, impl->audioSink.data());

    // Seed latency with the port default so getLatencySamples() is sane before the
    // first run() refreshes it.
    impl->latencySamples.store (impl->latencyPortIndex >= 0
                                  ? (int) impl->portValues[(size_t) impl->latencyPortIndex] : 0,
                                std::memory_order_relaxed);

    lilv_instance_activate (impl->instance);
    impl->active = true;
    return true;
}

void Lv2Instance::deactivate() { impl->freeInstance(); }

bool Lv2Instance::reactivate (double sampleRate, int maxBlockFrames, std::string& errorOut)
{
    // LV2 fixes the sample rate at instantiate, so a rate/block change means a
    // fresh instance. Carry the state blob across when the plugin can serialize
    // (control ports + state:interface); fall back to the raw port values when it
    // can't. The blob already reflects staged UI writes via getPortValue's shadow.
    std::vector<uint8_t> blob;
    saveState (blob);
    const std::vector<float> saved = impl->portValues;
    impl->freeInstance();
    if (! activate (sampleRate, maxBlockFrames, errorOut)) return false;
    if (! blob.empty())
    {
        loadState (blob);
    }
    else if (saved.size() == impl->portValues.size())
    {
        for (uint32_t idx : impl->controlPorts)
            impl->portValues[(size_t) idx] = saved[(size_t) idx];
        // Staged-but-undrained UI writes supersede the raw carry.
        for (uint32_t idx : impl->controlPorts)
            if ((size_t) idx < impl->uiDirty.size() && impl->uiDirty[(size_t) idx] != 0)
                impl->portValues[(size_t) idx] = impl->uiShadow[(size_t) idx];
    }
    return true;
}

void Lv2Instance::processBlock (const hosting::PortBuffers& io) noexcept
{
    const int numFrames = io.numFrames;

    auto clearOutputs = [&]
    {
        if (io.mainOut == nullptr || numFrames <= 0) return;
        for (int c = 0; c < io.mainOutChannels; ++c)
            if (io.mainOut[c] != nullptr)
                std::memset (io.mainOut[c], 0, sizeof (float) * (size_t) numFrames);
    };

    if (! impl->active || impl->instance == nullptr
        || numFrames <= 0 || numFrames > impl->maxFrames
        || io.mainOut == nullptr || io.mainOutChannels <= 0)
    {
        clearOutputs();
        return;
    }

    // Connect audio ports to the caller's buffers for this block. Extra plugin
    // channels beyond what the adapter supplies get silence (mainIn) or a scratch
    // sink; the adapter already sized to the negotiated counts.
    const int nin  = io.mainIn != nullptr
                       ? std::min (io.mainInChannels, (int) impl->audioInPorts.size()) : 0;
    const int nout = std::min (io.mainOutChannels, (int) impl->audioOutPorts.size());
    for (int c = 0; c < nin;  ++c)
        lilv_instance_connect_port (impl->instance, impl->audioInPorts[(size_t) c], io.mainIn[c]);
    for (int c = 0; c < nout; ++c)
        lilv_instance_connect_port (impl->instance, impl->audioOutPorts[(size_t) c], io.mainOut[c]);

    // Drain the UI's staged control-port writes (single consumer — this thread).
    {
        uint32_t r = impl->writeRingR.load (std::memory_order_relaxed);
        const uint32_t w = impl->writeRingW.load (std::memory_order_acquire);
        while (r != w)
        {
            const auto& pw = impl->writeRing[(size_t) r];
            if ((size_t) pw.idx < impl->portValues.size())
                impl->portValues[(size_t) pw.idx] = pw.value;
            r = (r + 1u) % Impl::kWriteRingCap;
        }
        impl->writeRingR.store (r, std::memory_order_release);
    }

    // Re-advertise output-atom capacity before every run(): the plugin overwrites
    // atom->size with the bytes it wrote last block, so without this the buffer
    // reads as monotonically shrinking (never-recovering) capacity. Output atom
    // buffers follow the input ones in atomBuffers (inputs connected first).
    for (size_t i = impl->atomInPorts.size(); i < impl->atomBuffers.size(); ++i)
    {
        auto* atom = reinterpret_cast<LV2_Atom*> (impl->atomBuffers[i].data());
        atom->type = impl->atomChunkType;
        atom->size = (uint32_t) (impl->atomBuffers[i].size() - sizeof (LV2_Atom));
    }

    lilv_instance_run (impl->instance, (uint32_t) numFrames);

    if (impl->latencyPortIndex >= 0)
        impl->latencySamples.store ((int) impl->portValues[(size_t) impl->latencyPortIndex],
                                    std::memory_order_relaxed);
}

bool Lv2Instance::saveState (std::vector<uint8_t>& out) const
{
    out.clear();
    if (impl->instance == nullptr || impl->plugin == nullptr || impl->world == nullptr)
        return false;

    // Snapshot control-port values + the plugin's state:interface blob (JUCE-
    // wrapped plugins keep everything there) into a lilv state, serialized as
    // Turtle. No file directories are passed, so file-writing plugins keep
    // their state abstract/in-memory — fine for effects.
    LilvState* state = lilv_state_new_from_instance (
        impl->plugin, impl->instance, &impl->mapFeature,
        nullptr, nullptr, nullptr, nullptr,
        &Impl::getPortValue, impl.get(),
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, impl->features.data());
    if (state == nullptr) return false;

    char* ttl = lilv_state_to_string (impl->world, &impl->mapFeature, &impl->unmapFeature,
                                      state, "urn:duskstudio:lv2state", nullptr);
    lilv_state_free (state);
    if (ttl == nullptr) return false;

    out.assign (ttl, ttl + std::strlen (ttl));
    lilv_free (ttl);
    return ! out.empty();
}

bool Lv2Instance::loadState (const std::vector<uint8_t>& in)
{
    if (impl->instance == nullptr || impl->world == nullptr || in.empty())
        return false;

    const std::string ttl (in.begin(), in.end());
    LilvState* state = lilv_state_new_from_string (impl->world, &impl->mapFeature, ttl.c_str());
    if (state == nullptr) return false;

    // Restores control ports through setPortValue (the plugin reads portValues on
    // its next run()) and hands the state:interface blob to the plugin. Callers
    // fence the audio thread when the instance is live — same as activate().
    lilv_state_restore (state, impl->instance, &Impl::setPortValue, impl.get(),
                        0, impl->features.data());
    lilv_state_free (state);
    return true;
}

void*       Lv2Instance::lilvWorld()        const noexcept { return impl->world; }
const void* Lv2Instance::lilvPlugin()       const noexcept { return impl->plugin; }
void*       Lv2Instance::lilvInstance()     const noexcept { return impl->instance; }
void*       Lv2Instance::uridMapFeature()   const noexcept { return &impl->mapFeatureStruct; }
void*       Lv2Instance::uridUnmapFeature() const noexcept { return &impl->unmapFeatureStruct; }

void Lv2Instance::setControlPortValue (uint32_t portIndex, float value) noexcept
{
    if ((size_t) portIndex >= impl->uiShadow.size()) return;
    // Shadow first: saves/reactivate read the latest UI value from here even when
    // the audio thread never drains the ring (bypassed slot), and a ring overflow
    // can only delay the RT application, never lose the value for persistence.
    impl->uiShadow[(size_t) portIndex] = value;
    impl->uiDirty [(size_t) portIndex] = 1;

    // Stage into the SPSC ring — the audio thread owns portValues (run() reads
    // it concurrently) and applies these at the top of its next processBlock.
    const uint32_t w    = impl->writeRingW.load (std::memory_order_relaxed);
    const uint32_t next = (w + 1u) % Impl::kWriteRingCap;
    if (next == impl->writeRingR.load (std::memory_order_acquire))
        return;   // ring full — drop (only under a pathological flood)
    impl->writeRing[(size_t) w] = { portIndex, value };
    impl->writeRingW.store (next, std::memory_order_release);
}

int Lv2Instance::portIndexForSymbol (const char* symbol) const noexcept
{
    return impl->portIndexForSymbol (symbol);
}
} // namespace duskstudio::lv2
