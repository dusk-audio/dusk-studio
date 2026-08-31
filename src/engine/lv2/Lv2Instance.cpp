#include "Lv2Instance.h"
#include "Lv2Bundle.h"
#include "Lv2StatePaths.h"
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
#include <cstdio>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <vector>

namespace duskstudio::lv2
{
struct Lv2Instance::Impl
{
    // Bumped whenever a LilvInstance is destroyed, so an embedded UI holding
    // instance-access handles can detect the swap.
    std::atomic<std::uint64_t> instanceEpoch { 0 };

    // URID map/unmap host feature (a simple intern table)
    std::unordered_map<std::string, uint32_t> uridForUri;
    std::vector<std::string> uriForUrid { std::string() };   // index 0 unused (URIDs start at 1)
    LV2_URID_Map   mapFeature   {};
    LV2_URID_Unmap unmapFeature {};
    LV2_Feature    mapFeatureStruct     {};
    LV2_Feature    unmapFeatureStruct   {};
    LV2_Feature    optionsFeatureStruct {};
    LV2_Feature    boundedFeatureStruct {};
    std::vector<const LV2_Feature*> features;

    // Backing storage for the options feature - must outlive the instance, which
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
        uridPatchGet      = mapUri (this, LV2_PATCH__Get);
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
    LV2_URID uridObject = 0, uridPatchGet = 0, uridPatchSet = 0, uridPatchProperty = 0,
             uridPatchValue = 0, uridEventTransfer = 0, uridMidiEvent = 0,
             uridPatchPut = 0, uridPatchBody = 0, uridSequence = 0, uridFloatType = 0,
             uridUridType = 0;

    // state:mapPath / state:makePath / state:freePath for file-backed state
    // Save side: lilv builds its own map/make-path from the directories we
    // hand lilv_state_new_from_instance. Restore side: the blob carries
    // ABSTRACT (cur/-relative) paths, so we supply the mapping back to
    // absolute ourselves. Returned strings are malloc'd - the plugin frees
    // them through freePathCb (or plain free()), per the state spec.
    std::filesystem::path stateDir;
    // A failed restore leaves the instance at defaults. Refuse later snapshots
    // from that instance so an ignored restore error cannot rotate away the
    // carried file-backed generations with a default-state save.
    bool stateRestoreFailed = false;

    static char* absolutePathCb (LV2_State_Map_Path_Handle handle, const char* abstractPath)
    {
        if (abstractPath == nullptr) return nullptr;
        auto* self = static_cast<Impl*> (handle);
        const auto absolute = statepaths::toAbsolute (self->stateDir, abstractPath);
        // A refused blob value must not fall through as the relative string it
        // came in as: the plugin would resolve it against the process working
        // directory and read a file outside the session entirely.
        return absolute.empty() ? nullptr : ::strdup (absolute.c_str());
    }
    static char* abstractPathCb (LV2_State_Map_Path_Handle handle, const char* absolutePath)
    {
        if (absolutePath == nullptr) return nullptr;
        auto* self = static_cast<Impl*> (handle);
        return ::strdup (statepaths::toAbstract (self->stateDir, absolutePath).c_str());
    }
    static char* makePathCb (LV2_State_Make_Path_Handle handle, const char* requestedPath)
    {
        if (requestedPath == nullptr) return nullptr;
        auto* self = static_cast<Impl*> (handle);
        std::error_code ec;
        const auto path = statepaths::makeRestorePath (
            self->stateDir, requestedPath, ec);
        return ec ? nullptr : ::strdup (path.u8string().c_str());
    }
    static void freePathCb (LV2_State_Free_Path_Handle, char* path) { ::free (path); }

    // Assemble the full feature list for instantiate: urid map/unmap + the block-size
    // and sample-rate options + boundedBlockLength. Framework-wrapped plugins REQUIRE
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

    // plugin + instance
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
    std::vector<uint32_t> otherPorts;   // CV / unclassified - LV2 requires every port connected
    int latencyPortIndex = -1;

    struct ControlRestoreInfo
    {
        float minValue = 0.0f, maxValue = 0.0f;
        bool hasUsableRange = false;
        bool toggled = false, integer = false, enumeration = false;
        std::vector<float> scalePoints;
    };
    // Snapshotted for every input control port, including designated/hidden
    // ports that are deliberately absent from the user parameter surface.
    std::vector<ControlRestoreInfo> controlRestoreInfo;

    // Per-port scratch backing otherPorts: maxFrames floats each, so an audio-rate
    // CV port can safely read silence or sink its output.
    std::vector<std::vector<float>> otherScratch;

    // Baseline audio-port buffers: every audio port is wired to these at activate()
    // so the LV2 every-port-connected invariant holds even if a caller supplies
    // fewer channels than the layout advertises; processBlock re-points the main
    // channels each block. Inputs share the silence, outputs share the sink.
    std::vector<float> audioSilence, audioSink;

    // UI -> audio-thread control-port writes. The UI must not store into portValues
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
    // instance-access shortcut loses nothing - see forwardUiAtomEvent).
    struct AtomBlob { uint32_t size = 0; uint8_t data[128]; };
    hosting::SpscRing<AtomBlob, 64> atomRing;

    uint32_t forgePatchSet (LV2_URID property, float value,
                            uint8_t* data, size_t capacity) noexcept
    {
        LV2_Atom_Forge forge;
        lv2_atom_forge_init (&forge, &mapFeature);
        lv2_atom_forge_set_buffer (&forge, data, capacity);
        LV2_Atom_Forge_Frame frame;
        if (lv2_atom_forge_object (&forge, &frame, 0, uridPatchSet) == 0) return 0;
        lv2_atom_forge_key (&forge, uridPatchProperty);
        lv2_atom_forge_urid (&forge, property);
        lv2_atom_forge_key (&forge, uridPatchValue);
        lv2_atom_forge_float (&forge, value);
        lv2_atom_forge_pop (&forge, &frame);
        const auto* atom = reinterpret_cast<const LV2_Atom*> (data);
        const auto size = (uint32_t) sizeof (LV2_Atom) + atom->size;
        return size <= capacity ? size : 0;
    }

    uint32_t forgePatchGet (uint8_t* data, size_t capacity) noexcept
    {
        LV2_Atom_Forge forge;
        lv2_atom_forge_init (&forge, &mapFeature);
        lv2_atom_forge_set_buffer (&forge, data, capacity);
        LV2_Atom_Forge_Frame frame;
        if (lv2_atom_forge_object (&forge, &frame, 0, uridPatchGet) == 0) return 0;
        lv2_atom_forge_pop (&forge, &frame);
        const auto* atom = reinterpret_cast<const LV2_Atom*> (data);
        const auto size = (uint32_t) sizeof (LV2_Atom) + atom->size;
        return size <= capacity ? size : 0;
    }

    // Which atomInPorts entry takes injected events: the lv2:control-designated
    // port, else the first atom input.
    int controlAtomInPos = 0;
    // Mirrors it for the plugin's outgoing patch responses (-1 = no atom output).
    int controlAtomOutPos = -1;

    // Plugin -> host property feedback (audio-thread parse of the control atom
    // output, drained into patchShadow on the message thread). Size this once
    // from the declared float patch surface: one generic patch:Get may return
    // every property in a single patch:Put, so a fixed queue silently truncates
    // otherwise-valid large plug-ins.
    struct PatchFeedback { LV2_URID prop; float value; };
    class PatchFeedbackRing
    {
    public:
        void configure (size_t itemCapacity)
        {
            slots.assign (std::max<size_t> (2, itemCapacity + 1), {});
            writeIdx.store (0, std::memory_order_relaxed);
            readIdx.store (0, std::memory_order_relaxed);
        }

        bool push (const PatchFeedback& value) noexcept
        {
            const size_t capacity = slots.size();
            if (capacity < 2) return false;
            const size_t write = writeIdx.load (std::memory_order_relaxed);
            const size_t next = (write + 1) % capacity;
            if (next == readIdx.load (std::memory_order_acquire)) return false;
            slots[write] = value;
            writeIdx.store (next, std::memory_order_release);
            return true;
        }

        template <typename Fn>
        void drain (Fn&& fn) noexcept
        {
            const size_t capacity = slots.size();
            if (capacity < 2) return;
            size_t read = readIdx.load (std::memory_order_relaxed);
            const size_t write = writeIdx.load (std::memory_order_acquire);
            while (read != write)
            {
                fn (slots[read]);
                read = (read + 1) % capacity;
            }
            readIdx.store (read, std::memory_order_release);
        }

    private:
        std::vector<PatchFeedback> slots;
        std::atomic<size_t> writeIdx { 0 };
        std::atomic<size_t> readIdx { 0 };
    };

    PatchFeedbackRing patchOutRing;
    std::unordered_set<LV2_URID> patchPropertyUrids;
    std::atomic<uint32_t> patchFeedbackOverflows { 0 };
    std::atomic<uint32_t> patchGetQueueFailures { 0 };
    std::atomic<bool> patchRefreshPending { false };

    void queuePatchFeedback (LV2_URID property, float value) noexcept
    {
        // A generic patch:Get may include readable or internal properties that
        // are not part of the host's declared parameter surface. They cannot
        // update a parameter and must not consume capacity reserved for one.
        if (patchPropertyUrids.find (property) == patchPropertyUrids.end()) return;
        if (! patchOutRing.push ({ property, value }))
        {
            patchFeedbackOverflows.fetch_add (1, std::memory_order_relaxed);
            patchRefreshPending.store (true, std::memory_order_release);
        }
    }

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
                queuePatchFeedback (
                    reinterpret_cast<const LV2_Atom_URID*> (propAtom)->body,
                    reinterpret_cast<const LV2_Atom_Float*> (valAtom)->body);
        }
        else if (obj->body.otype == uridPatchPut)
        {
            const LV2_Atom* bodyAtom = nullptr;
            lv2_atom_object_get (obj, uridPatchBody, &bodyAtom, 0);
            if (bodyAtom == nullptr || bodyAtom->type != uridObject) return;
            const auto* body = reinterpret_cast<const LV2_Atom_Object*> (bodyAtom);
            LV2_ATOM_OBJECT_FOREACH (body, p)
                if (p->value.type == uridFloatType)
                    queuePatchFeedback (
                        p->key,
                        reinterpret_cast<const LV2_Atom_Float*> (&p->value)->body);
        }
    }

    // Last host/UI-written value per patch property (message thread) - the
    // read-back source until patch:Put parsing exists.
    std::unordered_map<LV2_URID, float> patchShadow;

    // Message-thread shadow of the UI writes. A bypassed slot never runs
    // processBlock, so the ring may never drain - state saves and reactivate read
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
            // The instance an editor captured instance-access / data-access
            // handles from is gone; every teardown path has to mark that embed
            // stale, not just reactivate's.
            instanceEpoch.fetch_add (1, std::memory_order_release);
        }
        active = false;
    }

    // Filled at create(); lilv's state save/restore resolves every port by symbol,
    // so this lookup runs once per port per save - keep it O(1).
    std::unordered_map<std::string, uint32_t> portIndexBySymbol;

    int portIndexForSymbol (const char* symbol) const
    {
        if (symbol == nullptr) return -1;
        const auto it = portIndexBySymbol.find (symbol);
        return it != portIndexBySymbol.end() ? (int) it->second : -1;
    }

    bool normalizeRestoredControlValue (size_t index, double& value) const noexcept
    {
        if (index >= controlRestoreInfo.size() || ! std::isfinite (value)) return false;
        const auto& info = controlRestoreInfo[index];
        if (! info.hasUsableRange)
            return ! info.toggled && ! info.integer && ! info.enumeration;

        value = std::clamp (value, (double) info.minValue, (double) info.maxValue);
        if (info.toggled)
        {
            if (info.minValue > 0.0f || info.maxValue < 1.0f) return false;
            value = value > 0.0f ? 1.0f : 0.0f;
        }
        else if (info.enumeration)
        {
            if (info.scalePoints.empty()) return false;
            value = *std::min_element (
                info.scalePoints.begin(), info.scalePoints.end(),
                [value] (float a, float b)
                { return std::abs (a - value) < std::abs (b - value); });
        }
        else if (info.integer)
        {
            const double integerMin = std::ceil ((double) info.minValue);
            const double integerMax = std::floor ((double) info.maxValue);
            if (integerMin > integerMax) return false;
            value = std::clamp (std::round (value), integerMin, integerMax);
        }
        return true;
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
        double v = 0.0;
        if      (type == self->uridFloat  && size >= sizeof (float))   v = (double) *static_cast<const float*> (value);
        else if (type == self->uridDouble && size >= sizeof (double))  v = *static_cast<const double*> (value);
        else if (type == self->uridInt    && size >= sizeof (int32_t)) v = (double) *static_cast<const int32_t*> (value);
        else if (type == self->uridLong   && size >= sizeof (int64_t)) v = (double) *static_cast<const int64_t*> (value);
        else return;
        if (! self->normalizeRestoredControlValue ((size_t) idx, v)) return;
        const auto restoredValue = (float) v;
        if (! std::isfinite (restoredValue)) return;
        self->portValues[(size_t) idx] = restoredValue;
        // A restore supersedes any staged UI value for this port.
        if ((size_t) idx < self->uiDirty.size())
        {
            self->uiShadow[(size_t) idx] = restoredValue;
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
    impl->stateRestoreFailed = false;
    impl->plugin = static_cast<const LilvPlugin*> (bundle.pluginByUri (uri));
    if (impl->plugin == nullptr) { errorOut = "plugin URI not found in bundle: " + uri; return false; }

    auto* world = static_cast<LilvWorld*> (bundle.world());
    impl->world = world;
    LilvNode* audioClass   = lilv_new_uri (world, LV2_CORE__AudioPort);
    LilvNode* controlClass = lilv_new_uri (world, LV2_CORE__ControlPort);
    LilvNode* inputClass   = lilv_new_uri (world, LV2_CORE__InputPort);
    LilvNode* atomClass    = lilv_new_uri (world, LV2_ATOM__AtomPort);
    LilvNode* latencyDesig = lilv_new_uri (world, LV2_CORE__latency);
    const uint32_t numPorts = lilv_plugin_get_num_ports (impl->plugin);

    impl->audioInPorts.clear();  impl->audioOutPorts.clear();
    impl->controlPorts.clear();  impl->atomInPorts.clear(); impl->atomOutPorts.clear();
    impl->otherPorts.clear();
    impl->portIndexBySymbol.clear();
    impl->controlRestoreInfo.assign ((size_t) numPorts, {});
    impl->uiShadow.clear();
    impl->uiDirty.clear();
    impl->latencyPortIndex = -1;

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
            impl->otherPorts.push_back (i);   // CV / unknown -> scratch in activate()
    }

    // Input control ports double as the parameter surface (MIDI bindings /
    // MIDI Learn). Snapshot name + range + steppedness now so later reads
    // never touch lilv.
    impl->params.clear();
    impl->patchPropertyUrids.clear();
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

            auto& restore = impl->controlRestoreInfo[(size_t) i];
            float defaultValue = 0.0f;
            LilvNode* def = nullptr; LilvNode* mn = nullptr; LilvNode* mx = nullptr;
            lilv_port_get_range (impl->plugin, port, &def, &mn, &mx);
            if (mn  != nullptr) { restore.minValue = lilv_node_as_float (mn); lilv_node_free (mn); }
            if (mx  != nullptr) { restore.maxValue = lilv_node_as_float (mx); lilv_node_free (mx); }
            if (def != nullptr) { defaultValue = lilv_node_as_float (def);    lilv_node_free (def); }
            restore.hasUsableRange = std::isfinite (restore.minValue)
                                  && std::isfinite (restore.maxValue)
                                  && restore.minValue < restore.maxValue;
            restore.toggled = lilv_port_has_property (impl->plugin, port, toggledProp);
            restore.integer = lilv_port_has_property (impl->plugin, port, integerProp);
            restore.enumeration = lilv_port_has_property (impl->plugin, port, enumProp);
            if (restore.enumeration)
            {
                if (LilvScalePoints* points = lilv_port_get_scale_points (impl->plugin, port))
                {
                    LILV_FOREACH (scale_points, point, points)
                    {
                        const auto* scalePoint = lilv_scale_points_get (points, point);
                        const float v = lilv_node_as_float (
                            lilv_scale_point_get_value (scalePoint));
                        if (std::isfinite (v) && restore.hasUsableRange
                            && v >= restore.minValue && v <= restore.maxValue)
                            restore.scalePoints.push_back (v);
                    }
                    lilv_scale_points_free (points);
                    std::sort (restore.scalePoints.begin(), restore.scalePoints.end());
                    restore.scalePoints.erase (
                        std::unique (restore.scalePoints.begin(), restore.scalePoints.end()),
                        restore.scalePoints.end());
                }
            }

            // Designated ports (lv2:enabled, lv2:freeWheeling, time/transport
            // feeds) are host-managed, not user parameters; notOnGUI ports are
            // hidden by the plugin's own request. JUCE-wrapped LV2s expose ONLY
            // such ports - their real parameters ride atom patch messages, which
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
            p.minValue = restore.minValue;
            p.maxValue = restore.maxValue;
            p.defaultValue = defaultValue;
            if (! (p.minValue < p.maxValue)) { p.minValue = 0.0f; p.maxValue = 1.0f; }
            p.stepped = restore.toggled || restore.integer || restore.enumeration;
            impl->params.push_back (std::move (p));
        }
        lilv_node_free (notOnGuiProp);

        // patch:writable float properties join the surface - JUCE-built LV2s
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
            // lilv does not define an iteration order for a node collection, so the
            // properties come out in a different order per instance. Parameter INDEX
            // is what MIDI bindings persist and what the UI addresses, so collect
            // first and append sorted by property URI - stable across instances,
            // launches and machines.
            std::vector<std::pair<std::string, ParamInfo>> patchParams;
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
                patchParams.emplace_back (lilv_node_as_uri (prop), std::move (p));
            }
            lilv_nodes_free (props);

            std::sort (patchParams.begin(), patchParams.end(),
                       [] (const auto& a, const auto& b) { return a.first < b.first; });
            for (auto& entry : patchParams)
                impl->params.push_back (std::move (entry.second));
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

    size_t patchPropertyCount = 0;
    for (const auto& param : impl->params)
    {
        if (! param.isPatchProperty) continue;
        impl->patchPropertyUrids.insert (param.id & ~Impl::kPatchIdFlag);
        ++patchPropertyCount;
    }
    impl->patchOutRing.configure (patchPropertyCount);
    impl->patchFeedbackOverflows.store (0, std::memory_order_relaxed);
    impl->patchGetQueueFailures.store (0, std::memory_order_relaxed);
    impl->patchRefreshPending.store (false, std::memory_order_relaxed);

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

    // Baseline audio-port wiring (see Impl::audioSilence) - processBlock overrides
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
    const bool carriedRestoreFailure = impl->stateRestoreFailed;
    saveStateBlobOnly (blob);
    const std::vector<float> saved = impl->portValues;
    // freeInstance bumps the epoch before the rebuild, so a failed activate
    // still leaves the embed marked stale.
    impl->freeInstance();
    if (! activate (sampleRate, maxBlockFrames, errorOut)) return false;
    if (! blob.empty())
    {
        // This blob was just captured from the live instance, rather than read
        // from the persisted session. Its file paths already describe the
        // active cur/ generation, so do not require byte equality with the
        // older session blob stored there.
        if (! loadStateInternal (blob, false))
        {
            errorOut = "LV2 state restore failed after reactivation";
            return false;
        }
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
    if (carriedRestoreFailure)
    {
        errorOut = "LV2 state remained unavailable after reactivation";
        return false;
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

    // Drain the UI's staged control-port writes (single consumer - this thread).
    impl->writeRing.drain ([this] (const Impl::PortWrite& pw)
    {
        if ((size_t) pw.idx < impl->portValues.size())
            impl->portValues[(size_t) pw.idx] = pw.value;
    });

    // Rebuild the control atom input's sequence: staged patch/UI atoms first
    // (frame 0), then the block's MIDI at its sample offsets - the sequence
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
            if (used + padded > buf.size()) return;   // sequence full - drop
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
    // the read-back shadow honest - parse the control atom output and stage
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

void Lv2Instance::setStateDirectory (const std::filesystem::path& dir)
{
    impl->stateDir = statepaths::normalizeStateDirectory (dir);
}

bool Lv2Instance::saveStateBlobOnly (std::vector<uint8_t>& out) const
{
    out.clear();
    if (impl->instance == nullptr || impl->plugin == nullptr || impl->world == nullptr
        || impl->stateRestoreFailed)
        return false;

    LilvState* state = lilv_state_new_from_instance (
        impl->plugin, impl->instance, &impl->mapFeature,
        nullptr, nullptr, nullptr, nullptr,
        &Impl::getPortValue, impl.get(),
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, impl->features.data());
    if (state == nullptr) return false;

    char* ttl = lilv_state_to_string (
        impl->world, &impl->mapFeature, &impl->unmapFeature,
        state, "urn:duskstudio:lv2state", nullptr);
    lilv_state_free (state);
    if (ttl == nullptr) return false;
    out.assign (ttl, ttl + std::strlen (ttl));
    lilv_free (ttl);
    return ! out.empty();
}

bool Lv2Instance::saveState (std::vector<uint8_t>& out) const
{
    if (impl->stateDir.empty()) return saveStateBlobOnly (out);

    out.clear();
    if (impl->instance == nullptr || impl->plugin == nullptr || impl->world == nullptr
        || impl->stateRestoreFailed)
        return false;

    // Snapshot control-port values + the plugin's state:interface blob (JUCE-
    // wrapped plugins keep everything there) into a lilv state, serialized as
    // Turtle. With a state directory set, lilv also snapshots FILE-BACKED
    // state (sample banks, IRs) into a fresh <dir>/next/ generation and emits
    // abstract paths in the Turtle. A successful serialization publishes next/
    // as cur/ below; without a directory, file-writing plugins keep only their
    // in-memory state (the pre-file-state behaviour, fine for effects).
    std::error_code ec;
    const auto transaction = statepaths::prepareNextGeneration (impl->stateDir, ec);
    // Never report a successful blob-only save for a slot configured with
    // file-backed state: that would silently stop carrying its external files
    // after a staging or recovery failure.
    if (transaction.next.empty()) return false;
    const auto& next = transaction.next;

    // A restored plugin refers to files in cur/. Lilv must consider that the
    // scratch generation so it preserves their bytes in the stable copy store,
    // then links the new generation to those copies.
    const auto copyDir = impl->stateDir / "copy";
    const auto linkDir = impl->stateDir / "link";
    std::filesystem::create_directories (copyDir, ec);
    if (! ec) std::filesystem::create_directories (linkDir, ec);
    if (ec)
    {
        statepaths::discardNextGeneration (impl->stateDir);
        return false;
    }
    const auto scratchPath = (impl->stateDir / "cur").u8string();
    const auto copyPath = copyDir.u8string();
    const auto linkPath = linkDir.u8string();
    const auto savePath = next.u8string();

    LilvState* state = lilv_state_new_from_instance (
        impl->plugin, impl->instance, &impl->mapFeature,
        scratchPath.c_str(), copyPath.c_str(), linkPath.c_str(), savePath.c_str(),
        &Impl::getPortValue, impl.get(),
        LV2_STATE_IS_POD | LV2_STATE_IS_PORTABLE, impl->features.data());
    if (state == nullptr)
    {
        statepaths::discardNextGeneration (impl->stateDir);
        return false;
    }

    std::vector<uint8_t> serialized;
    constexpr const char* stateFile = "state.ttl";
    const int saveResult = lilv_state_save (
        impl->world, &impl->mapFeature, &impl->unmapFeature, state,
        "urn:duskstudio:lv2state", savePath.c_str(), stateFile);
    lilv_state_free (state);
    if (saveResult != 0)
    {
        statepaths::discardNextGeneration (impl->stateDir);
        return false;
    }

    std::ifstream input (next / stateFile, std::ios::binary);
    serialized.assign (std::istreambuf_iterator<char> (input),
                       std::istreambuf_iterator<char>());
    if ((! input.good() && ! input.eof()) || serialized.empty())
    {
        statepaths::discardNextGeneration (impl->stateDir);
        return false;
    }

    // Only now that both the plugin snapshot and Turtle serialization succeeded
    // may the fresh files replace cur/. Until this point a restored plugin's
    // absolute cur/ paths remain valid throughout the save.
    if (! statepaths::commitNextGeneration (transaction, ec))
    {
        // commitNextGeneration may have moved cur aside before the publish or
        // rollback failed. Preserve the complete staged generation as recovery
        // data; the next prepare removes it only when cur exists.
        return false;
    }

    out = std::move (serialized);
    return true;
}

bool Lv2Instance::loadState (const std::vector<uint8_t>& in)
{
    return loadStateInternal (in, true);
}

bool Lv2Instance::loadStateInternal (const std::vector<uint8_t>& in,
                                     bool recoverFileGeneration)
{
    if (impl->instance == nullptr || impl->world == nullptr || in.empty())
        return false;

    LilvState* state = nullptr;
    if (! impl->stateDir.empty() && recoverFileGeneration)
    {
        const std::string_view persistedState {
            reinterpret_cast<const char*> (in.data()), in.size() };
        std::error_code recoveryError;
        if (! statepaths::recoverGeneration (impl->stateDir, persistedState,
                                             recoveryError))
        {
            impl->stateRestoreFailed = true;
            return false;
        }

        const auto stateFile = impl->stateDir / "cur" / "state.ttl";
        std::ifstream input (stateFile, std::ios::binary);
        const std::vector<uint8_t> saved {
            std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>() };
        if (saved == in)
        {
            LilvNode* subject = lilv_new_uri (impl->world, "urn:duskstudio:lv2state");
            state = lilv_state_new_from_file (
                impl->world, &impl->mapFeature, subject, stateFile.c_str());
            lilv_node_free (subject);
        }
    }

    // Blob-only snapshots and states written by older versions contain no
    // relative file URIs, so the filesystem-independent parser remains the
    // compatible fallback.
    if (state == nullptr)
    {
        const std::string ttl (in.begin(), in.end());
        state = lilv_state_new_from_string (impl->world, &impl->mapFeature, ttl.c_str());
    }
    if (state == nullptr)
    {
        impl->stateRestoreFailed = true;
        return false;
    }

    // Restores control ports through setPortValue (the plugin reads portValues on
    // its next run()) and hands the state:interface blob to the plugin. Callers
    // fence the audio thread when the instance is live - same as activate().
    // mapPath/freePath resolve the blob's abstract file paths against the
    // slot's state directory. makePath lets restore create derived files in
    // cur/ without escaping the slot's state directory.
    LV2_State_Map_Path  mapPath  { impl.get(), &Impl::abstractPathCb, &Impl::absolutePathCb };
    LV2_State_Make_Path makePath { impl.get(), &Impl::makePathCb };
    LV2_State_Free_Path freePath { nullptr, &Impl::freePathCb };
    LV2_Feature mapPathFeat  { LV2_STATE__mapPath,  &mapPath };
    LV2_Feature makePathFeat { LV2_STATE__makePath, &makePath };
    LV2_Feature freePathFeat { LV2_STATE__freePath, &freePath };
    std::vector<const LV2_Feature*> feats;
    for (const auto* f : impl->features)
        if (f != nullptr) feats.push_back (f);
    feats.push_back (&mapPathFeat);
    feats.push_back (&makePathFeat);
    feats.push_back (&freePathFeat);
    feats.push_back (nullptr);

    lilv_state_restore (state, impl->instance, &Impl::setPortValue, impl.get(),
                        0, feats.data());
    lilv_state_free (state);
    impl->stateRestoreFailed = false;
    return true;
}

void*       Lv2Instance::lilvWorld()        const noexcept { return impl->world; }
const void* Lv2Instance::lilvPlugin()       const noexcept { return impl->plugin; }
void*       Lv2Instance::lilvInstance()     const noexcept { return impl->instance; }
std::uint64_t Lv2Instance::instanceEpoch()  const noexcept { return impl->instanceEpoch.load (std::memory_order_acquire); }
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

    // Stage into the SPSC ring - the audio thread owns portValues (run() reads
    // it concurrently) and applies these at the top of its next processBlock.
    impl->writeRing.push ({ portIndex, value });   // full => drop (pathological flood only)
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

    const LV2_URID propUrid = paramId & ~Impl::kPatchIdFlag;
    Impl::AtomBlob blob;
    blob.size = impl->forgePatchSet (propUrid, v, blob.data, sizeof (blob.data));

    impl->patchShadow[propUrid] = v;
    if (blob.size != 0)
        impl->atomRing.push (blob);   // full => drop (pathological flood only)
}

void Lv2Instance::drainPatchFeedback()
{
    impl->patchOutRing.drain ([this] (const Impl::PatchFeedback& f)
    {
        impl->patchShadow[f.prop] = f.value;
    });

    const auto overflows = impl->patchFeedbackOverflows.exchange (
        0, std::memory_order_acq_rel);
    const auto requestFailures = impl->patchGetQueueFailures.exchange (
        0, std::memory_order_acq_rel);
    if (overflows != 0)
        std::fprintf (stderr,
                      "[lv2] patch feedback overflow dropped %u value(s); requesting a full refresh\n",
                      overflows);
    if (requestFailures != 0)
        std::fprintf (stderr,
                      "[lv2] patch feedback refresh queue was full %u time(s); retrying\n",
                      requestFailures);

    if (impl->patchRefreshPending.exchange (false, std::memory_order_acq_rel))
        requestPatchParameterValuesForUi();
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

int Lv2Instance::uiParameterEventCount() const noexcept
{
    return (int) impl->params.size();
}

bool Lv2Instance::currentUiParameterEvent (int index, UiParameterEvent& out) const
{
    out = {};
    const auto* param = paramInfo (index);
    if (param == nullptr) return false;

    double value = 0.0;
    if (! getParamValue (param->id, value)) return false;
    out.value = (float) value;

    if (! param->isPatchProperty)
    {
        out.portIndex = param->id;
        out.sizeBytes = sizeof (float);
        std::memcpy (out.data.data(), &out.value, sizeof (out.value));
        return true;
    }

    if (impl->controlAtomOutPos < 0) return false;
    out.portIndex = impl->atomOutPorts[(size_t) impl->controlAtomOutPos];
    out.protocol = impl->uridEventTransfer;
    out.sizeBytes = impl->forgePatchSet (
        param->id & ~Impl::kPatchIdFlag, out.value, out.data.data(), out.data.size());
    return out.sizeBytes != 0;
}

void Lv2Instance::requestPatchParameterValuesForUi() noexcept
{
    if (impl->atomInPorts.empty()) return;
    if (std::none_of (impl->params.begin(), impl->params.end(),
                      [] (const ParamInfo& param) { return param.isPatchProperty; }))
        return;

    Impl::AtomBlob blob;
    blob.size = impl->forgePatchGet (blob.data, sizeof (blob.data));
    if (blob.size != 0 && ! impl->atomRing.push (blob))
    {
        impl->patchGetQueueFailures.fetch_add (1, std::memory_order_relaxed);
        impl->patchRefreshPending.store (true, std::memory_order_release);
    }
}

void Lv2Instance::forwardUiAtomEvent (const void* atomData, uint32_t sizeBytes) noexcept
{
    if (atomData == nullptr || sizeBytes < sizeof (LV2_Atom)) return;
    const auto* atom = static_cast<const LV2_Atom*> (atomData);
    if (sizeof (LV2_Atom) + atom->size != sizeBytes) return;

    // patch:Set -> stamp MIDI Learn's last-touched + the read-back shadow.
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
    // relying on the instance-access shortcut. Oversized atoms are dropped -
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
