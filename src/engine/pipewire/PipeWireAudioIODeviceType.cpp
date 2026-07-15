#include "PipeWireAudioIODeviceType.h"
#include "PipeWireAudioIODevice.h"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <mutex>
#include <cstring>
#include <ctime>

namespace duskstudio
{
namespace
{
// pw_init refcounts internally, but guard it so concurrent first-use from the
// type ctor and a bare runSelfTest() can't race the global init.
void ensurePipeWireInit()
{
    static std::once_flag once;
    std::call_once (once, [] { pw_init (nullptr, nullptr); });
}

// Collected during a single synchronous registry roundtrip. Nodes and their
// audio ports arrive as separate globals; channel counts come from counting the
// ports (audio.channels is not in the node's registry props - it lives in the
// node info, which the registry roundtrip doesn't fetch). Finalised into the
// index-aligned *Names / *Ids / *Chans arrays by finalizeScan().
struct NodeRec
{
    juce::uint32 gid = 0;
    juce::String name, label;
    bool isSink = false, isSource = false;
};

struct ScanResult
{
    juce::Array<NodeRec>      nodes;
    juce::Array<juce::uint32> portNodeId;   // parallel with portIsOutput
    juce::Array<bool>         portIsOutput;

    juce::StringArray inNames, outNames, inIds, outIds;
    juce::Array<int>  inChans, outChans;

    pw_main_loop* loop = nullptr;
    int  syncSeq = 0;
    bool haveSync = false;
};

void onRegistryGlobal (void* data, uint32_t id, uint32_t /*permissions*/,
                        const char* type, uint32_t /*version*/,
                        const struct spa_dict* props)
{
    auto& r = *static_cast<ScanResult*> (data);
    if (props == nullptr)
        return;

    if (std::strcmp (type, PW_TYPE_INTERFACE_Node) == 0)
    {
        const char* mediaClass = spa_dict_lookup (props, PW_KEY_MEDIA_CLASS);
        const char* nodeName   = spa_dict_lookup (props, PW_KEY_NODE_NAME);
        if (mediaClass == nullptr || nodeName == nullptr)
            return;
        const bool isSink   = std::strcmp (mediaClass, "Audio/Sink")   == 0;
        const bool isSource = std::strcmp (mediaClass, "Audio/Source") == 0;
        const bool isDuplex = std::strcmp (mediaClass, "Audio/Duplex") == 0;
        if (! (isSink || isSource || isDuplex))
            return;
        const char* desc = spa_dict_lookup (props, PW_KEY_NODE_DESCRIPTION);
        NodeRec n;
        n.gid      = id;
        n.name     = juce::String::fromUTF8 (nodeName);
        n.label    = desc != nullptr ? juce::String::fromUTF8 (desc) : n.name;
        n.isSink   = isSink   || isDuplex;
        n.isSource = isSource || isDuplex;
        r.nodes.add (n);
    }
    else if (std::strcmp (type, PW_TYPE_INTERFACE_Port) == 0)
    {
        const char* nodeIdStr = spa_dict_lookup (props, PW_KEY_NODE_ID);
        const char* dir       = spa_dict_lookup (props, PW_KEY_PORT_DIRECTION);
        // Audio channels carry audio.channel; skip control / non-audio ports so
        // the count reflects real channels.
        if (nodeIdStr == nullptr || dir == nullptr
            || spa_dict_lookup (props, PW_KEY_AUDIO_CHANNEL) == nullptr)
            return;
        r.portNodeId.add ((juce::uint32) juce::String (nodeIdStr).getLargeIntValue());
        r.portIsOutput.add (std::strcmp (dir, "out") == 0);
    }
}

// A sink's channel count is its INPUT ports (audio we send to it); a source's
// is its OUTPUT ports. Build the index-aligned device lists from the port tally.
void finalizeScan (ScanResult& r)
{
    for (const auto& n : r.nodes)
    {
        int inPorts = 0, outPorts = 0;
        for (int k = 0; k < r.portNodeId.size(); ++k)
            if (r.portNodeId.getUnchecked (k) == n.gid)
                (r.portIsOutput.getUnchecked (k) ? outPorts : inPorts) += 1;

        if (n.isSink)   { r.outNames.add (n.label); r.outIds.add (n.name); r.outChans.add (inPorts); }
        if (n.isSource) { r.inNames.add  (n.label); r.inIds.add  (n.name); r.inChans.add  (outPorts); }
    }
}

void onCoreDone (void* data, uint32_t id, int seq)
{
    auto& r = *static_cast<ScanResult*> (data);
    // The graph delivers every existing registry global BEFORE answering our
    // sync, so once this matching done arrives the enumeration is complete.
    if (id == PW_ID_CORE && r.haveSync && seq == r.syncSeq)
        pw_main_loop_quit (r.loop);
}

void onCoreError (void* data, uint32_t /*id*/, int /*seq*/, int /*res*/, const char* /*message*/)
{
    // A fatal core error means no matching done will arrive; quit the loop so the
    // synchronous roundtrip can't hang waiting for a sync it will never get.
    auto& r = *static_cast<ScanResult*> (data);
    if (r.loop != nullptr)
        pw_main_loop_quit (r.loop);
}

void onEnumTimeout (void* data, uint64_t /*expirations*/)
{
    // Backstop the roundtrip: a server that neither answers our sync nor errors
    // would otherwise block the message thread forever.
    auto& r = *static_cast<ScanResult*> (data);
    if (r.loop != nullptr)
        pw_main_loop_quit (r.loop);
}

// Zero-init + field assignment rather than C99 designated initializers, which
// are a C++20 feature (the project is C++17 -Werror).
pw_registry_events makeRegistryEvents()
{
    pw_registry_events e {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global  = onRegistryGlobal;
    return e;
}

pw_core_events makeCoreEvents()
{
    pw_core_events e {};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done    = onCoreDone;
    e.error   = onCoreError;
    return e;
}

const pw_registry_events kRegistryEvents = makeRegistryEvents();
const pw_core_events     kCoreEvents     = makeCoreEvents();

// Enumerate the graph's audio nodes synchronously: spin up a throwaway
// context/core/registry, run one roundtrip, tear it all down. Message-thread
// only (matches AlsaAudioIODeviceType::scanForDevices).
void enumerateNodes (ScanResult& result)
{
    ensurePipeWireInit();

    result.loop = pw_main_loop_new (nullptr);
    if (result.loop == nullptr)
        return;

    auto* context = pw_context_new (pw_main_loop_get_loop (result.loop), nullptr, 0);
    if (context == nullptr) { pw_main_loop_destroy (result.loop); result.loop = nullptr; return; }

    auto* core = pw_context_connect (context, nullptr, 0);
    if (core == nullptr) { pw_context_destroy (context); pw_main_loop_destroy (result.loop); result.loop = nullptr; return; }

    auto* registry = pw_core_get_registry (core, PW_VERSION_REGISTRY, 0);
    if (registry == nullptr)
    {
        pw_core_disconnect (core);
        pw_context_destroy (context);
        pw_main_loop_destroy (result.loop);
        result.loop = nullptr;
        return;
    }

    spa_hook registryHook {};
    spa_hook coreHook {};
    pw_registry_add_listener (registry, &registryHook, &kRegistryEvents, &result);
    pw_core_add_listener (core, &coreHook, &kCoreEvents, &result);

    result.syncSeq  = pw_core_sync (core, PW_ID_CORE, 0);
    result.haveSync = true;

    auto* pwLoop = pw_main_loop_get_loop (result.loop);
    auto* timer  = pw_loop_add_timer (pwLoop, onEnumTimeout, &result);
    struct timespec timeout {};
    timeout.tv_sec = 2;
    pw_loop_update_timer (pwLoop, timer, &timeout, nullptr, false);

    pw_main_loop_run (result.loop);

    pw_loop_destroy_source (pwLoop, timer);
    spa_hook_remove (&registryHook);
    spa_hook_remove (&coreHook);
    pw_proxy_destroy ((pw_proxy*) registry);
    pw_core_disconnect (core);
    pw_context_destroy (context);
    pw_main_loop_destroy (result.loop);
    result.loop = nullptr;

    finalizeScan (result);
}
} // namespace

PipeWireAudioIODeviceType::PipeWireAudioIODeviceType()
    : juce::AudioIODeviceType ("PipeWire")
{
    ensurePipeWireInit();
}

void PipeWireAudioIODeviceType::scanForDevices()
{
    ScanResult r;
    enumerateNodes (r);

    inputNames  = r.inNames;
    outputNames = r.outNames;
    inputIds    = r.inIds;
    outputIds   = r.outIds;
    inputChans  = r.inChans;
    outputChans = r.outChans;

    inputNames.appendNumbersToDuplicates  (false, true);
    outputNames.appendNumbersToDuplicates (false, true);

    hasScanned = true;
}

juce::StringArray PipeWireAudioIODeviceType::getDeviceNames (bool wantInputNames) const
{
    jassert (hasScanned);
    return wantInputNames ? inputNames : outputNames;
}

int PipeWireAudioIODeviceType::getDefaultDeviceIndex (bool forInput) const
{
    jassert (hasScanned);
    // Skip monitor sources and HDMI sinks - the same "don't default to the
    // thing the user didn't mean" heuristic the ALSA backend uses. A saved
    // selection, once resolved, always takes precedence over this.
    const auto& names = forInput ? inputNames : outputNames;
    for (int i = 0; i < names.size(); ++i)
    {
        const auto& n = names[i];
        if (n.containsIgnoreCase ("Monitor")) continue;
        if (n.containsIgnoreCase ("HDMI"))    continue;
        return i;
    }
    return names.isEmpty() ? -1 : 0;
}

int PipeWireAudioIODeviceType::getIndexOfDevice (juce::AudioIODevice* device, bool asInput) const
{
    jassert (hasScanned);
    if (auto* pw = dynamic_cast<PipeWireAudioIODevice*> (device))
        return (asInput ? inputIds : outputIds).indexOf (asInput ? pw->inputId : pw->outputId);
    return -1;
}

juce::AudioIODevice* PipeWireAudioIODeviceType::createDevice (const juce::String& outputDeviceName,
                                                               const juce::String& inputDeviceName)
{
    jassert (hasScanned);
    const int outIdx = outputNames.indexOf (outputDeviceName);
    const int inIdx  = inputNames .indexOf (inputDeviceName);

    if (outIdx < 0 && inIdx < 0)
        return nullptr;

    const juce::String outId = outIdx >= 0 ? outputIds[outIdx] : juce::String();
    const juce::String inId  = inIdx  >= 0 ? inputIds [inIdx]  : juce::String();
    const juce::String name  = outIdx >= 0 ? outputDeviceName : inputDeviceName;
    const int outChans = outIdx >= 0 ? outputChans[outIdx] : 0;
    const int inChans  = inIdx  >= 0 ? inputChans [inIdx]  : 0;

    return new PipeWireAudioIODevice (name, inId, outId, inChans, outChans);
}

void PipeWireAudioIODeviceType::rescan()
{
    scanForDevices();
    callDeviceChangeListeners();
}
} // namespace duskstudio
