#include "PipeWireAudioIODeviceType.h"
#include "PipeWireAudioIODevice.h"

#include "../../foundation/Text.h"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <mutex>
#include <string>
#include <vector>

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

int indexOfString (const std::vector<std::string>& v, const std::string& s)
{
    for (int i = 0; i < (int) v.size(); ++i)
        if (v[(size_t) i] == s) return i;
    return -1;
}

// Replicates JUCE's StringArray::appendNumbersToDuplicates(false, true):
// exact (case-sensitive) duplicate device labels get " (1)", " (2)", ...
// suffixes, the first instance numbered too, so the dropdown never shows two
// identical names.
void appendNumbersToDuplicates (std::vector<std::string>& names)
{
    auto findFrom = [&names] (const std::string& target, int startIndex) -> int
    {
        for (int j = startIndex; j < (int) names.size(); ++j)
            if (names[(size_t) j] == target) return j;
        return -1;
    };
    for (int i = 0; i + 1 < (int) names.size(); ++i)
    {
        int nextIndex = findFrom (names[(size_t) i], i + 1);
        if (nextIndex < 0) continue;
        const std::string original = names[(size_t) i];
        int number = 0;
        names[(size_t) i] = original + " (" + std::to_string (++number) + ")";
        while (nextIndex >= 0)
        {
            names[(size_t) nextIndex] = names[(size_t) nextIndex] + " (" + std::to_string (++number) + ")";
            nextIndex = findFrom (original, nextIndex + 1);
        }
    }
}

// Collected during a single synchronous registry roundtrip. Nodes and their
// audio ports arrive as separate globals; channel counts come from counting the
// ports (audio.channels is not in the node's registry props - it lives in the
// node info, which the registry roundtrip doesn't fetch). Finalised into the
// index-aligned *Names / *Ids / *Chans arrays by finalizeScan().
struct NodeRec
{
    std::uint32_t gid = 0;
    std::string name, label;
    bool isSink = false, isSource = false;
};

struct ScanResult
{
    std::vector<NodeRec>      nodes;
    std::vector<std::uint32_t> portNodeId;   // parallel with portIsOutput
    std::vector<bool>         portIsOutput;

    std::vector<std::string> inNames, outNames, inIds, outIds;
    std::vector<int>  inChans, outChans;

    pw_main_loop* loop = nullptr;
    int  syncSeq = 0;
    bool haveSync = false;
    bool completed = false;   // the matching core.done arrived (scan is complete)
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
        n.name     = std::string (nodeName);
        n.label    = desc != nullptr ? std::string (desc) : n.name;
        n.isSink   = isSink   || isDuplex;
        n.isSource = isSource || isDuplex;
        r.nodes.push_back (n);
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
        r.portNodeId.push_back ((std::uint32_t) std::strtoul (nodeIdStr, nullptr, 10));
        r.portIsOutput.push_back (std::strcmp (dir, "out") == 0);
    }
}

// A sink's channel count is its INPUT ports (audio we send to it); a source's
// is its OUTPUT ports. Build the index-aligned device lists from the port tally.
void finalizeScan (ScanResult& r)
{
    for (const auto& n : r.nodes)
    {
        int inPorts = 0, outPorts = 0;
        for (int k = 0; k < (int) r.portNodeId.size(); ++k)
            if (r.portNodeId[(size_t) k] == n.gid)
                (r.portIsOutput[(size_t) k] ? outPorts : inPorts) += 1;

        if (n.isSink)   { r.outNames.push_back (n.label); r.outIds.push_back (n.name); r.outChans.push_back (inPorts); }
        if (n.isSource) { r.inNames.push_back  (n.label); r.inIds.push_back  (n.name); r.inChans.push_back  (outPorts); }
    }
}

void onCoreDone (void* data, uint32_t id, int seq)
{
    auto& r = *static_cast<ScanResult*> (data);
    // The graph delivers every existing registry global BEFORE answering our
    // sync, so once this matching done arrives the enumeration is complete.
    if (id == PW_ID_CORE && r.haveSync && seq == r.syncSeq)
    {
        r.completed = true;
        pw_main_loop_quit (r.loop);
    }
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

    // Only publish devices from a complete scan; a core error or the timeout
    // quits the loop early with partial data, which would list wrong counts.
    if (result.completed)
        finalizeScan (result);
}
} // namespace

PipeWireAudioIODeviceType::PipeWireAudioIODeviceType()
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

    appendNumbersToDuplicates (inputNames);
    appendNumbersToDuplicates (outputNames);

    hasScanned = true;
}

std::vector<std::string> PipeWireAudioIODeviceType::getDeviceNames (bool wantInputNames) const
{
    assert (hasScanned);
    return wantInputNames ? inputNames : outputNames;
}

int PipeWireAudioIODeviceType::getDefaultDeviceIndex (bool forInput) const
{
    assert (hasScanned);
    // Skip monitor sources and HDMI sinks - the same "don't default to the
    // thing the user didn't mean" heuristic the ALSA backend uses. A saved
    // selection, once resolved, always takes precedence over this.
    const auto& names = forInput ? inputNames : outputNames;
    for (int i = 0; i < (int) names.size(); ++i)
    {
        const auto& n = names[(size_t) i];
        if (dusk::text::containsIgnoreCase (n, "Monitor")) continue;
        if (dusk::text::containsIgnoreCase (n, "HDMI"))    continue;
        return i;
    }
    return names.empty() ? -1 : 0;
}

int PipeWireAudioIODeviceType::getIndexOfDevice (device::IODevice* device, bool asInput) const
{
    assert (hasScanned);
    if (auto* pw = dynamic_cast<PipeWireAudioIODevice*> (device))
        return indexOfString (asInput ? inputIds : outputIds, asInput ? pw->inputId : pw->outputId);
    return -1;
}

std::unique_ptr<device::IODevice> PipeWireAudioIODeviceType::createDevice (const std::string& outputDeviceName,
                                                                           const std::string& inputDeviceName)
{
    assert (hasScanned);
    const int outIdx = indexOfString (outputNames, outputDeviceName);
    const int inIdx  = indexOfString (inputNames, inputDeviceName);

    if (outIdx < 0 && inIdx < 0)
        return nullptr;

    const std::string outId = outIdx >= 0 ? outputIds[(size_t) outIdx] : std::string();
    const std::string inId  = inIdx  >= 0 ? inputIds [(size_t) inIdx]  : std::string();
    const std::string name  = outIdx >= 0 ? outputDeviceName : inputDeviceName;
    const int outChans = outIdx >= 0 ? outputChans[(size_t) outIdx] : 0;
    const int inChans  = inIdx  >= 0 ? inputChans [(size_t) inIdx]  : 0;

    return std::make_unique<PipeWireAudioIODevice> (name, inId, outId, inChans, outChans);
}
} // namespace duskstudio
