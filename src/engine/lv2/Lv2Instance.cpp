#include "Lv2Instance.h"
#include "Lv2Bundle.h"
#include "../hosting/SpscRing.h"

#include <lilv/lilv.h>
#include <lv2/core/lv2.h>
#include <lv2/atom/atom.h>
#include <lv2/buf-size/buf-size.h>
#include <lv2/options/options.h>
#include <lv2/parameters/parameters.h>
#include <lv2/port-props/port-props.h>
#include <lv2/atom/forge.h>
#include <lv2/atom/util.h>
#include <lv2/midi/midi.h>
#include <lv2/patch/patch.h>
#include <lv2/state/state.h>
#include <lv2/urid/urid.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdlib>
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
        uridObject        = mapUri (this, LV2_ATOM__Object);
        uridPatchSet      = mapUri (this, LV2_PATCH__Set);
        uridPatchProperty = mapUri (this, LV2_PATCH__property);
        uridPatchValue    = mapUri (this, LV2_PATCH__value);
        uridEventTransfer = mapUri (this, LV2_ATOM__eventTransfer);
        uridMidiEvent     = mapUri (this, LV2_MIDI__MidiEvent);
        uridPatchPut      = mapUri (this, LV2_PATCH__Put);
        uridPatchBody     = mapUri (this, LV2_PATCH__body);
        uridSequence      = mapUri (this, LV2_ATOM__Sequence);
        uridFloatType     = mapUri (this, LV2_ATOM__Float);
        uridUridType      = mapUri (this, LV2_ATOM__URID);
    }
    LV2_URID uridFloat = 0, uridDouble = 0, uridInt = 0, uridLong = 0;
    LV2_URID uridObject = 0, uridPatchSet = 0, uridPatchProperty = 0,
             uridPatchValue = 0, uridEventTransfer = 0, uridMidiEvent = 0,
             uridPatchPut = 0, uridPatchBody = 0, uridSequence = 0, uridFloatType = 0,
             uridUridType = 0;

    // ── state:mapPath / state:freePath for file-backed state ──
    // Save side: lilv builds its own map/make-path from the directories we
    // hand lilv_state_new_from_instance. Restore side: the blob carries
    // ABSTRACT (cur/-relative) paths, so we supply the mapping back to
    // absolute ourselves. Returned strings are malloc'd — the plugin frees
    // them through freePathCb (or plain free()), per the state spec.
    juce::File stateDir;

    static char* absolutePathCb (LV2_State_Map_Path_Handle handle, const char* abstractPath)
    {
        if (abstractPath == nullptr) return nullptr;
        auto* self = static_cast<Impl*> (handle);
        const auto abs = juce::String::fromUTF8 (abstractPath);
        if (self->stateDir == juce::File() || juce::File::isAbsolutePath (abs))
            return ::strdup (abstractPath);
        return ::strdup (self->stateDir.getChildFile ("cur").getChildFile (abs)
                             .getFullPathName().toRawUTF8());
    }
    static char* abstractPathCb (LV2_State_Map_Path_Handle handle, const char* absolutePath)
    {
        if (absolutePath == nullptr) return nullptr;
        auto* self = static_cast<Impl*> (handle);
        const juce::File f { juce::String::fromUTF8 (absolutePath) };
        const auto cur = self->stateDir.getChildFile ("cur");
        if (self->stateDir != juce::File() && f.isAChildOf (cur))
            return ::strdup (f.getRelativePathFrom (cur)
                                 .replaceCharacter ('\\', '/').toRawUTF8());
        return ::strdup (absolutePath);
    }
    static void freePathCb (LV2_State_Free_Path_Handle, char* path) { ::free (path); }

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
    // drains them before run().
    struct PortWrite { uint32_t idx; float value; };
    hosting::SpscRing<PortWrite, 256> writeRing;

    // Input control ports + patch:writable float properties as a parameter
    // surface (MIDI bindings / MIDI Learn). Patch-property ids carry the high
    // bit so they can't collide with port indices.
    static constexpr uint32_t kPatchIdFlag = 0x80000000u;
    std::vector<ParamInfo> params;
    std::atomic<int64_t>   lastTouchedParam { -1 };   // index into params

    int paramIndexForId (uint32_t id) const noexcept
    {
        for (size_t i = 0; i < params.size(); ++i)
            if (params[i].id == id) return (int) i;
        return -1;
    }

    // patch:Set atoms staged UI/host-side, injected into the control atom input
    // at the top of the next process block. 128 bytes covers a patch:Set with a
    // float payload several times over; larger UI atoms are dropped (only the
    // instance-access shortcut loses nothing — see forwardUiAtomEvent).
    struct AtomBlob { uint32_t size = 0; uint8_t data[128]; };
    hosting::SpscRing<AtomBlob, 64> atomRing;

    // Which atomInPorts entry takes injected events: the lv2:control-designated
    // port, else the first atom input.
    int controlAtomInPos = 0;
    // Mirrors it for the plugin's outgoing patch responses (-1 = no atom output).
    int controlAtomOutPos = -1;

    // Plugin → host property feedback (audio-thread parse of the control atom
    // output, drained into patchShadow on the message thread).
    struct PatchFeedback { LV2_URID prop; float value; };
    hosting::SpscRing<PatchFeedback, 128> patchOutRing;

    // Audio thread. Queue one patch:Set / patch:Put object's float properties.
    void queuePatchObject (const LV2_Atom_Object* obj) noexcept
    {
        if (obj->body.otype == uridPatchSet)
        {
            const LV2_Atom* propAtom = nullptr;
            const LV2_Atom* valAtom  = nullptr;
            lv2_atom_object_get (obj, uridPatchProperty, &propAtom,
                                      uridPatchValue,    &valAtom, 0);
            if (propAtom != nullptr && propAtom->type == uridUridType
                && valAtom != nullptr && valAtom->type == uridFloatType)
                patchOutRing.push ({ reinterpret_cast<const LV2_Atom_URID*> (propAtom)->body,
                                     reinterpret_cast<const LV2_Atom_Float*> (valAtom)->body });
        }
        else if (obj->body.otype == uridPatchPut)
        {
            const LV2_Atom* bodyAtom = nullptr;
            lv2_atom_object_get (obj, uridPatchBody, &bodyAtom, 0);
            if (bodyAtom == nullptr || bodyAtom->type != uridObject) return;
            const auto* body = reinterpret_cast<const LV2_Atom_Object*> (bodyAtom);
            LV2_ATOM_OBJECT_FOREACH (body, p)
                if (p->value.type == uridFloatType)
                    patchOutRing.push ({ p->key,
                        reinterpret_cast<const LV2_Atom_Float*> (&p->value)->body });
        }
    }

    // Last host/UI-written value per patch property (message thread) — the
    // read-back source until patch:Put parsing exists.
    std::unordered_map<LV2_URID, float> patchShadow;

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

    // Input control ports double as the parameter surface (MIDI bindings /
    // MIDI Learn). Snapshot name + range + steppedness now so later reads
    // never touch lilv.
    impl->params.clear();
    impl->lastTouchedParam.store (-1, std::memory_order_relaxed);
    {
        LilvNode* toggledProp  = lilv_new_uri (world, LV2_CORE__toggled);
        LilvNode* integerProp  = lilv_new_uri (world, LV2_CORE__integer);
        LilvNode* enumProp     = lilv_new_uri (world, LV2_CORE__enumeration);
        LilvNode* designation  = lilv_new_uri (world, LV2_CORE_PREFIX "designation");
        LilvNode* notOnGuiProp = lilv_new_uri (world, LV2_PORT_PROPS_PREFIX "notOnGUI");
        for (uint32_t i : impl->controlPorts)
        {
            const LilvPort* port = lilv_plugin_get_port_by_index (impl->plugin, i);
            if (! lilv_port_is_a (impl->plugin, port, inputClass))
                continue;   // output control ports (meters, lv2:latency) aren't parameters
            // Designated ports (lv2:enabled, lv2:freeWheeling, time/transport
            // feeds) are host-managed, not user parameters; notOnGUI ports are
            // hidden by the plugin's own request. JUCE-wrapped LV2s expose ONLY
            // such ports — their real parameters ride atom patch messages, which
            // this surface doesn't cover (yet).
            if (LilvNodes* desig = lilv_port_get_value (impl->plugin, port, designation))
            {
                const bool designated = lilv_nodes_size (desig) > 0;
                lilv_nodes_free (desig);
                if (designated) continue;
            }
            if (lilv_port_has_property (impl->plugin, port, notOnGuiProp))
                continue;
            ParamInfo p;
            p.id = i;
            if (LilvNode* nm = lilv_port_get_name (impl->plugin, port))
            {
                p.name = lilv_node_as_string (nm);
                lilv_node_free (nm);
            }
            LilvNode* def = nullptr; LilvNode* mn = nullptr; LilvNode* mx = nullptr;
            lilv_port_get_range (impl->plugin, port, &def, &mn, &mx);
            if (mn  != nullptr) { p.minValue     = lilv_node_as_float (mn);  lilv_node_free (mn); }
            if (mx  != nullptr) { p.maxValue     = lilv_node_as_float (mx);  lilv_node_free (mx); }
            if (def != nullptr) { p.defaultValue = lilv_node_as_float (def); lilv_node_free (def); }
            if (! (p.minValue < p.maxValue)) { p.minValue = 0.0f; p.maxValue = 1.0f; }
            p.stepped = lilv_port_has_property (impl->plugin, port, toggledProp)
                     || lilv_port_has_property (impl->plugin, port, integerProp)
                     || lilv_port_has_property (impl->plugin, port, enumProp);
            impl->params.push_back (std::move (p));
        }
        lilv_node_free (notOnGuiProp);

        // patch:writable float properties join the surface — JUCE-built LV2s
        // expose ALL their parameters this way (their control ports are only
        // the designated host-managed ones). Non-float ranges (paths, strings)
        // aren't parameters and are skipped.
        LilvNode* patchWritable = lilv_new_uri (world, LV2_PATCH__writable);
        LilvNode* rdfsLabel     = lilv_new_uri (world, "http://www.w3.org/2000/01/rdf-schema#label");
        LilvNode* rdfsRange     = lilv_new_uri (world, "http://www.w3.org/2000/01/rdf-schema#range");
        LilvNode* atomFloat     = lilv_new_uri (world, LV2_ATOM__Float);
        LilvNode* lv2Min        = lilv_new_uri (world, LV2_CORE__minimum);
        LilvNode* lv2Max        = lilv_new_uri (world, LV2_CORE__maximum);
        LilvNode* lv2Default    = lilv_new_uri (world, LV2_CORE__default);
        LilvNode* portPropPred  = lilv_new_uri (world, LV2_CORE_PREFIX "portProperty");
        if (LilvNodes* props = lilv_world_find_nodes (world,
                                   lilv_plugin_get_uri (impl->plugin), patchWritable, nullptr))
        {
            LILV_FOREACH (nodes, it, props)
            {
                const LilvNode* prop = lilv_nodes_get (props, it);
                if (! lilv_node_is_uri (prop)) continue;
                LilvNode* range = lilv_world_get (world, prop, rdfsRange, nullptr);
                const bool isFloat = range != nullptr && lilv_node_equals (range, atomFloat);
                lilv_node_free (range);
                if (! isFloat) continue;

                ParamInfo p;
                p.id = Impl::kPatchIdFlag
                     | Impl::mapUri (impl.get(), lilv_node_as_uri (prop));
                p.isPatchProperty = true;
                if (LilvNode* nm = lilv_world_get (world, prop, rdfsLabel, nullptr))
                { p.name = lilv_node_as_string (nm); lilv_node_free (nm); }
                if (p.name.empty())
                    p.name = lilv_node_as_uri (prop);
                auto numberOf = [&] (const LilvNode* pred, float fallback)
                {
                    float v = fallback;
                    if (LilvNode* n = lilv_world_get (world, prop, pred, nullptr))
                    { v = lilv_node_as_float (n); lilv_node_free (n); }
                    return v;
                };
                p.minValue     = numberOf (lv2Min, 0.0f);
                p.maxValue     = numberOf (lv2Max, 1.0f);
                p.defaultValue = numberOf (lv2Default, p.minValue);
                if (! (p.minValue < p.maxValue)) { p.minValue = 0.0f; p.maxValue = 1.0f; }
                if (LilvNodes* pps = lilv_world_find_nodes (world, prop, portPropPred, nullptr))
                {
                    LILV_FOREACH (nodes, pit, pps)
                    {
                        const LilvNode* pp = lilv_nodes_get (pps, pit);
                        if (lilv_node_equals (pp, toggledProp)
                            || lilv_node_equals (pp, integerProp)
                            || lilv_node_equals (pp, enumProp))
                            p.stepped = true;
                    }
                    lilv_nodes_free (pps);
                }
                impl->patchShadow[p.id & ~Impl::kPatchIdFlag] = p.defaultValue;
                impl->params.push_back (std::move (p));
            }
            lilv_nodes_free (props);
        }
        lilv_node_free (patchWritable); lilv_node_free (rdfsLabel);
        lilv_node_free (rdfsRange);     lilv_node_free (atomFloat);
        lilv_node_free (lv2Min);        lilv_node_free (lv2Max);
        lilv_node_free (lv2Default);    lilv_node_free (portPropPred);
        lilv_node_free (toggledProp);
        lilv_node_free (integerProp);
        lilv_node_free (enumProp);
        lilv_node_free (designation);
    }

    // Which atom input takes injected patch events: the lv2:control-designated
    // one, else the first.
    impl->controlAtomInPos = 0;
    {
        LilvNode* ctrlDesig  = lilv_new_uri (world, LV2_CORE_PREFIX "control");
        LilvNode* inputClass2 = lilv_new_uri (world, LV2_CORE__InputPort);
        if (const LilvPort* cp = lilv_plugin_get_port_by_designation (impl->plugin, inputClass2, ctrlDesig))
        {
            const uint32_t idx = lilv_port_get_index (impl->plugin, cp);
            for (size_t i = 0; i < impl->atomInPorts.size(); ++i)
                if (impl->atomInPorts[i] == idx) { impl->controlAtomInPos = (int) i; break; }
        }
        impl->controlAtomOutPos = impl->atomOutPorts.empty() ? -1 : 0;
        LilvNode* outputClass2 = lilv_new_uri (world, LV2_CORE__OutputPort);
        if (const LilvPort* cp = lilv_plugin_get_port_by_designation (impl->plugin, outputClass2, ctrlDesig))
        {
            const uint32_t idx = lilv_port_get_index (impl->plugin, cp);
            for (size_t i = 0; i < impl->atomOutPorts.size(); ++i)
                if (impl->atomOutPorts[i] == idx) { impl->controlAtomOutPos = (int) i; break; }
        }
        lilv_node_free (outputClass2);
        lilv_node_free (ctrlDesig);
        lilv_node_free (inputClass2);
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
    impl->writeRing.drain ([this] (const Impl::PortWrite& pw)
    {
        if ((size_t) pw.idx < impl->portValues.size())
            impl->portValues[(size_t) pw.idx] = pw.value;
    });

    // Rebuild the control atom input's sequence: staged patch/UI atoms first
    // (frame 0), then the block's MIDI at its sample offsets — the sequence
    // stays time-sorted. Input sequences are host-owned, so the reset is cheap
    // and the other atom inputs keep their empty headers from activate().
    if (! impl->atomInPorts.empty())
    {
        auto& buf = impl->atomBuffers[(size_t) impl->controlAtomInPos];
        auto* seq = reinterpret_cast<LV2_Atom_Sequence*> (buf.data());
        seq->atom.size = sizeof (LV2_Atom_Sequence_Body);
        auto appendEvent = [&] (int64_t frames, uint32_t type,
                                const uint8_t* data, uint32_t size)
        {
            const uint32_t evSize = (uint32_t) sizeof (LV2_Atom_Event) + size;
            const uint32_t padded = lv2_atom_pad_size (evSize);
            const uint32_t used   = (uint32_t) sizeof (LV2_Atom) + seq->atom.size;
            if (used + padded > buf.size()) return;   // sequence full — drop
            auto* ev = reinterpret_cast<LV2_Atom_Event*> (buf.data() + used);
            ev->time.frames = frames;
            ev->body.size   = size;
            ev->body.type   = type;
            std::memcpy (ev + 1, data, size);
            seq->atom.size += padded;
        };
        impl->atomRing.drain ([&] (const Impl::AtomBlob& blob)
        {
            // blob is a full atom (header + body); re-emit as header + payload.
            const auto* atom = reinterpret_cast<const LV2_Atom*> (blob.data);
            appendEvent (0, atom->type,
                         blob.data + sizeof (LV2_Atom), atom->size);
        });
        if (io.midiIn != nullptr)
            for (const auto meta : *io.midiIn)
                if (meta.numBytes > 0 && meta.numBytes <= 3)
                    appendEvent ((int64_t) meta.samplePosition, impl->uridMidiEvent,
                                 meta.data, (uint32_t) meta.numBytes);
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

    // The plugin's outgoing patch responses (its own UI / preset loads) keep
    // the read-back shadow honest — parse the control atom output and stage
    // the float properties for the message-thread drain.
    if (impl->controlAtomOutPos >= 0)
    {
        const auto& buf = impl->atomBuffers[impl->atomInPorts.size()
                                            + (size_t) impl->controlAtomOutPos];
        const auto* seq = reinterpret_cast<const LV2_Atom_Sequence*> (buf.data());
        if (seq->atom.type == impl->uridSequence)
        {
            LV2_ATOM_SEQUENCE_FOREACH (seq, ev)
            {
                if (ev->body.type == impl->uridObject)
                    impl->queuePatchObject (
                        reinterpret_cast<const LV2_Atom_Object*> (&ev->body));
            }
        }
    }

    if (impl->latencyPortIndex >= 0)
        impl->latencySamples.store ((int) impl->portValues[(size_t) impl->latencyPortIndex],
                                    std::memory_order_relaxed);
}

void Lv2Instance::setStateDirectory (const juce::File& dir)
{
    impl->stateDir = dir;
}

bool Lv2Instance::saveState (std::vector<uint8_t>& out) const
{
    out.clear();
    if (impl->instance == nullptr || impl->plugin == nullptr || impl->world == nullptr)
        return false;

    // Snapshot control-port values + the plugin's state:interface blob (JUCE-
    // wrapped plugins keep everything there) into a lilv state, serialized as
    // Turtle. With a state directory set, lilv also snapshots FILE-BACKED
    // state (sample banks, IRs) into <dir>/cur/ and emits abstract paths in
    // the Turtle; without one, file-writing plugins keep only their in-memory
    // state (the pre-file-state behaviour, fine for effects).
    juce::String curPath;
    if (impl->stateDir != juce::File())
    {
        // Rotate generations instead of wiping: a disk-streaming sampler may
        // still be reading the files the PREVIOUS save snapshotted — those
        // survive one more save cycle in prev/.
        const auto cur  = impl->stateDir.getChildFile ("cur");
        const auto prev = impl->stateDir.getChildFile ("prev");
        prev.deleteRecursively();
        if (cur.isDirectory()) cur.moveFileTo (prev);
        cur.createDirectory();
        curPath = cur.getFullPathName();
    }
    const auto curUtf8 = curPath.toStdString();
    const char* dirC   = curUtf8.empty() ? nullptr : curUtf8.c_str();

    LilvState* state = lilv_state_new_from_instance (
        impl->plugin, impl->instance, &impl->mapFeature,
        dirC, dirC, dirC, dirC,
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
    // mapPath/freePath resolve the blob's abstract file paths against the
    // slot's state directory (no-ops when none is set / the blob has none).
    LV2_State_Map_Path  mapPath  { impl.get(), &Impl::abstractPathCb, &Impl::absolutePathCb };
    LV2_State_Free_Path freePath { nullptr, &Impl::freePathCb };
    LV2_Feature mapPathFeat  { LV2_STATE__mapPath,  &mapPath };
    LV2_Feature freePathFeat { LV2_STATE__freePath, &freePath };
    std::vector<const LV2_Feature*> feats;
    for (const auto* f : impl->features)
        if (f != nullptr) feats.push_back (f);
    feats.push_back (&mapPathFeat);
    feats.push_back (&freePathFeat);
    feats.push_back (nullptr);

    lilv_state_restore (state, impl->instance, &Impl::setPortValue, impl.get(),
                        0, feats.data());
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
    impl->writeRing.push ({ portIndex, value });   // full ⇒ drop (pathological flood only)
}

void Lv2Instance::setControlPortValueFromUi (uint32_t portIndex, float value) noexcept
{
    const int idx = impl->paramIndexForId (portIndex);
    if (idx >= 0)
        impl->lastTouchedParam.store ((int64_t) idx, std::memory_order_relaxed);
    setControlPortValue (portIndex, value);
}

int Lv2Instance::paramCount() const noexcept { return (int) impl->params.size(); }

const Lv2Instance::ParamInfo* Lv2Instance::paramInfo (int index) const noexcept
{
    return (index >= 0 && index < (int) impl->params.size())
             ? &impl->params[(size_t) index] : nullptr;
}

bool Lv2Instance::getParamValue (uint32_t paramId, double& out) const
{
    const int idx = impl->paramIndexForId (paramId);
    if (idx < 0) return false;
    if (impl->params[(size_t) idx].isPatchProperty)
    {
        const auto it = impl->patchShadow.find (paramId & ~Impl::kPatchIdFlag);
        if (it == impl->patchShadow.end()) return false;
        out = (double) it->second;
        return true;
    }
    const uint32_t portIndex = paramId;
    // A staged UI/host write the audio thread hasn't drained yet (bypassed
    // slot) lives in the shadow; portValues still holds the older value.
    if ((size_t) portIndex < impl->uiDirty.size()
        && impl->uiDirty[(size_t) portIndex] != 0)
        out = (double) impl->uiShadow[(size_t) portIndex];
    else if ((size_t) portIndex < impl->portValues.size())
        out = (double) impl->portValues[(size_t) portIndex];
    else
        return false;
    return true;
}

void Lv2Instance::setParamValue (uint32_t paramId, double value) noexcept
{
    const int idx = impl->paramIndexForId (paramId);
    if (idx < 0) return;
    const auto& p = impl->params[(size_t) idx];
    const float v = (float) std::clamp (value, (double) p.minValue, (double) p.maxValue);
    if (! p.isPatchProperty)
    {
        setControlPortValue (paramId, v);
        return;
    }

    // patch:Set { property = <prop>, value = Float } forged into a blob the
    // audio thread injects into the control atom port next block.
    const LV2_URID propUrid = paramId & ~Impl::kPatchIdFlag;
    Impl::AtomBlob blob;
    LV2_Atom_Forge forge;
    lv2_atom_forge_init (&forge, &impl->mapFeature);
    lv2_atom_forge_set_buffer (&forge, blob.data, sizeof (blob.data));
    LV2_Atom_Forge_Frame frame;
    lv2_atom_forge_object (&forge, &frame, 0, impl->uridPatchSet);
    lv2_atom_forge_key (&forge, impl->uridPatchProperty);
    lv2_atom_forge_urid (&forge, propUrid);
    lv2_atom_forge_key (&forge, impl->uridPatchValue);
    lv2_atom_forge_float (&forge, v);
    lv2_atom_forge_pop (&forge, &frame);
    const auto* atom = reinterpret_cast<const LV2_Atom*> (blob.data);
    blob.size = (uint32_t) sizeof (LV2_Atom) + atom->size;

    impl->patchShadow[propUrid] = v;
    impl->atomRing.push (blob);   // full ⇒ drop (pathological flood only)
}

void Lv2Instance::drainPatchFeedback()
{
    impl->patchOutRing.drain ([this] (const Impl::PatchFeedback& f)
    {
        impl->patchShadow[f.prop] = f.value;
    });
}

int Lv2Instance::lastTouchedParamIndex() const noexcept
{
    const auto idx = impl->lastTouchedParam.load (std::memory_order_relaxed);
    return (idx >= 0 && idx < (int64_t) impl->params.size()) ? (int) idx : -1;
}

uint32_t Lv2Instance::uiEventTransferUrid() const noexcept
{
    return impl->uridEventTransfer;
}

void Lv2Instance::forwardUiAtomEvent (const void* atomData, uint32_t sizeBytes) noexcept
{
    if (atomData == nullptr || sizeBytes < sizeof (LV2_Atom)) return;
    const auto* atom = static_cast<const LV2_Atom*> (atomData);
    if (sizeof (LV2_Atom) + atom->size != sizeBytes) return;

    // patch:Set → stamp MIDI Learn's last-touched + the read-back shadow.
    if (atom->type == impl->uridObject)
    {
        const auto* obj = reinterpret_cast<const LV2_Atom_Object*> (atom);
        if (obj->body.otype == impl->uridPatchSet)
        {
            const LV2_Atom* propAtom = nullptr;
            const LV2_Atom* valAtom  = nullptr;
            lv2_atom_object_get (obj, impl->uridPatchProperty, &propAtom,
                                      impl->uridPatchValue,    &valAtom, 0);
            if (propAtom != nullptr && propAtom->type == impl->uridUridType)
            {
                const LV2_URID prop = reinterpret_cast<const LV2_Atom_URID*> (propAtom)->body;
                const int idx = impl->paramIndexForId (Impl::kPatchIdFlag | prop);
                if (idx >= 0)
                    impl->lastTouchedParam.store ((int64_t) idx, std::memory_order_relaxed);
                if (valAtom != nullptr && valAtom->type == impl->uridFloat)
                    impl->patchShadow[prop] =
                        reinterpret_cast<const LV2_Atom_Float*> (valAtom)->body;
            }
        }
    }

    // Forward onto the control atom port so the DSP hears the event without
    // relying on the instance-access shortcut. Oversized atoms are dropped —
    // for JUCE-built UIs instance access already carried the change.
    if (sizeBytes <= sizeof (Impl::AtomBlob::data))
    {
        Impl::AtomBlob blob;
        std::memcpy (blob.data, atomData, sizeBytes);
        blob.size = sizeBytes;
        impl->atomRing.push (blob);
    }
}

int Lv2Instance::portIndexForSymbol (const char* symbol) const noexcept
{
    return impl->portIndexForSymbol (symbol);
}
} // namespace duskstudio::lv2
