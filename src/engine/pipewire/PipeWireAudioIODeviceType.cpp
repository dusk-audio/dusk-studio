#include "PipeWireAudioIODeviceType.h"
#include "PipeWireAudioIODevice.h"

#include <pipewire/pipewire.h>
#include <spa/utils/dict.h>

#include <mutex>
#include <cstring>

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

// Collected during a single synchronous registry roundtrip. The *Chans arrays
// are index-aligned with the corresponding *Names / *Ids arrays.
struct ScanResult
{
    juce::StringArray inNames, outNames, inIds, outIds;
    juce::Array<int>  inChans, outChans;
    pw_main_loop*     loop = nullptr;
    int               syncSeq = 0;
    bool              haveSync = false;
};

void onRegistryGlobal (void* data, uint32_t /*id*/, uint32_t /*permissions*/,
                        const char* type, uint32_t /*version*/,
                        const struct spa_dict* props)
{
    auto& r = *static_cast<ScanResult*> (data);
    if (props == nullptr || std::strcmp (type, PW_TYPE_INTERFACE_Node) != 0)
        return;

    const char* mediaClass = spa_dict_lookup (props, PW_KEY_MEDIA_CLASS);
    const char* nodeName    = spa_dict_lookup (props, PW_KEY_NODE_NAME);
    if (mediaClass == nullptr || nodeName == nullptr)
        return;

    const char* desc = spa_dict_lookup (props, PW_KEY_NODE_DESCRIPTION);
    const juce::String label = desc != nullptr ? juce::String::fromUTF8 (desc)
                                               : juce::String::fromUTF8 (nodeName);
    const juce::String id = juce::String::fromUTF8 (nodeName);

    // audio.channels is the node's channel count; absent on some nodes, in which
    // case 0 -> the device falls back to a stereo pair.
    const char* chanStr = spa_dict_lookup (props, PW_KEY_AUDIO_CHANNELS);
    const int chans = chanStr != nullptr ? juce::String (chanStr).getIntValue() : 0;

    const bool isSink   = std::strcmp (mediaClass, "Audio/Sink")   == 0;
    const bool isSource = std::strcmp (mediaClass, "Audio/Source") == 0;
    const bool isDuplex = std::strcmp (mediaClass, "Audio/Duplex") == 0;

    if (isSink || isDuplex)   { r.outNames.add (label); r.outIds.add (id); r.outChans.add (chans); }
    if (isSource || isDuplex) { r.inNames.add  (label); r.inIds.add  (id); r.inChans.add  (chans); }
}

void onCoreDone (void* data, uint32_t id, int seq)
{
    auto& r = *static_cast<ScanResult*> (data);
    // The graph delivers every existing registry global BEFORE answering our
    // sync, so once this matching done arrives the enumeration is complete.
    if (id == PW_ID_CORE && r.haveSync && seq == r.syncSeq)
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

    spa_hook registryHook {};
    spa_hook coreHook {};
    pw_registry_add_listener (registry, &registryHook, &kRegistryEvents, &result);
    pw_core_add_listener (core, &coreHook, &kCoreEvents, &result);

    result.syncSeq  = pw_core_sync (core, PW_ID_CORE, 0);
    result.haveSync = true;

    pw_main_loop_run (result.loop);

    spa_hook_remove (&registryHook);
    spa_hook_remove (&coreHook);
    pw_proxy_destroy ((pw_proxy*) registry);
    pw_core_disconnect (core);
    pw_context_destroy (context);
    pw_main_loop_destroy (result.loop);
    result.loop = nullptr;
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
