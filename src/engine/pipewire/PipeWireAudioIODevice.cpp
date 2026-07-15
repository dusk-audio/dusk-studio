#include "PipeWireAudioIODevice.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <pipewire/link.h>
#include <spa/node/io.h>
#include <spa/utils/dict.h>

#include <algorithm>
#include <mutex>
#include <cstdio>
#include <cstring>
#include <thread>
#include <chrono>
#include <ctime>

namespace duskstudio
{
namespace
{
// PipeWire's default graph clock.max-quantum. A cycle larger than this is
// pathological (another client forced a huge quantum); we drop it rather than
// grow scratch on the RT thread. Sizing the unlinked-port scratch to this keeps
// the data thread allocation-free across any legal quantum.
constexpr int kMaxQuantum = 8192;

// pw_init refcounts internally; the once-flag just keeps a first-use race
// between the type ctor, a device open and a bare runSelfTest() off the global.
void ensurePipeWireInit()
{
    static std::once_flag once;
    std::call_once (once, [] { pw_init (nullptr, nullptr); });
}

void onProcessTrampoline (void* data, struct spa_io_position* position)
{
    static_cast<PipeWireAudioIODevice*> (data)->onProcess (position);
}

void onStateChangedTrampoline (void* data, enum pw_filter_state /*old*/,
                                enum pw_filter_state /*state*/, const char* /*error*/)
{
    static_cast<PipeWireAudioIODevice*> (data)->onFilterStateChanged();
}

// Zero-init + assignment rather than C99 designated initializers (a C++20
// feature; the project is C++17 -Werror).
pw_filter_events makeFilterEvents()
{
    pw_filter_events e {};
    e.version = PW_VERSION_FILTER_EVENTS;
    e.process = onProcessTrampoline;
    e.state_changed = onStateChangedTrampoline;
    return e;
}

const pw_filter_events kFilterEvents = makeFilterEvents();

// ----- graph port discovery (for manual linking) -----------------------------
// A single node's audio port, collected from the registry so we can reference
// its global id in a link. Direction is the graph's ("out" produces).
struct GraphPort
{
    juce::uint32 nodeId = 0;
    juce::uint32 portId = 0;
    bool         isOutput = false;
    juce::String name;      // "playback_1", "capture_2", device port names...
    juce::String channel;   // audio.channel: "FL"/"FR"/"AUX0".., or empty
};

// A stable per-node channel ordering key. Prefer the semantic audio.channel
// position (survives graph recreation, unlike registry ids); fall back to the
// trailing index in our own DSP port names ("playback_3"), then the port id.
// Only used to order ports WITHIN one node, so the key scales need not align
// across the named/aux/fallback groups.
int channelOrderKey (const GraphPort& p)
{
    if (p.channel.isNotEmpty())
    {
        if (p.channel.startsWith ("AUX"))
            return 100 + p.channel.substring (3).getIntValue();
        static const char* const canonical[] =
            { "MONO", "FL", "FR", "FC", "LFE", "RL", "RR", "FLC", "FRC", "RC", "SL", "SR" };
        for (int i = 0; i < (int) (sizeof (canonical) / sizeof (canonical[0])); ++i)
            if (p.channel == canonical[i]) return i;
    }
    const auto digits = p.name.retainCharacters ("0123456789");
    if (digits.isNotEmpty())
        return digits.getIntValue();
    return (int) p.portId;
}

struct GraphScan
{
    juce::Array<GraphPort> ports;
    juce::StringArray      nodeNames;   // index-aligned with nodeIds
    juce::Array<juce::uint32> nodeIds;
    pw_main_loop* loop = nullptr;
    int  syncSeq = 0;
    bool haveSync = false;

    juce::uint32 resolveNode (const juce::String& name) const
    {
        const int i = nodeNames.indexOf (name);
        return i >= 0 ? nodeIds.getUnchecked (i) : (juce::uint32) SPA_ID_INVALID;
    }
};

void onGraphGlobal (void* data, uint32_t id, uint32_t /*perm*/, const char* type,
                     uint32_t /*version*/, const struct spa_dict* props)
{
    auto& s = *static_cast<GraphScan*> (data);
    if (props == nullptr)
        return;

    if (std::strcmp (type, PW_TYPE_INTERFACE_Node) == 0)
    {
        if (const char* nm = spa_dict_lookup (props, PW_KEY_NODE_NAME))
        {
            s.nodeNames.add (juce::String::fromUTF8 (nm));
            s.nodeIds.add (id);
        }
    }
    else if (std::strcmp (type, PW_TYPE_INTERFACE_Port) == 0)
    {
        const char* nodeIdStr = spa_dict_lookup (props, PW_KEY_NODE_ID);
        const char* dir       = spa_dict_lookup (props, PW_KEY_PORT_DIRECTION);
        if (nodeIdStr == nullptr || dir == nullptr)
            return;
        const bool isOut = std::strcmp (dir, "out") == 0;
        const bool isIn  = std::strcmp (dir, "in")  == 0;
        if (! isOut && ! isIn)
            return;
        const char* ch = spa_dict_lookup (props, PW_KEY_AUDIO_CHANNEL);
        const char* nm = spa_dict_lookup (props, PW_KEY_PORT_NAME);
        const juce::String name = nm != nullptr ? juce::String::fromUTF8 (nm) : juce::String();
        // Link only audio ports: device audio ports carry audio.channel; our own
        // DSP ports don't but are named playback_/capture_. Everything else
        // (MIDI / control / notify ports) must not be linked.
        if (ch == nullptr && ! name.startsWith ("playback_") && ! name.startsWith ("capture_"))
            return;
        GraphPort p;
        p.nodeId   = (juce::uint32) juce::String (nodeIdStr).getLargeIntValue();
        p.portId   = id;
        p.isOutput = isOut;
        p.name     = name;
        if (ch != nullptr)
            p.channel = juce::String::fromUTF8 (ch);
        s.ports.add (p);
    }
}

void onGraphDone (void* data, uint32_t id, int seq)
{
    auto& s = *static_cast<GraphScan*> (data);
    if (id == PW_ID_CORE && s.haveSync && seq == s.syncSeq)
        pw_main_loop_quit (s.loop);
}

void onGraphError (void* data, uint32_t /*id*/, int /*seq*/, int /*res*/, const char* /*msg*/)
{
    // No matching done will arrive after a fatal core error; quit so discovery
    // can't block the open() path forever.
    auto& s = *static_cast<GraphScan*> (data);
    if (s.loop != nullptr)
        pw_main_loop_quit (s.loop);
}

void onGraphTimeout (void* data, uint64_t /*expirations*/)
{
    auto& s = *static_cast<GraphScan*> (data);
    if (s.loop != nullptr)
        pw_main_loop_quit (s.loop);
}

pw_registry_events makeGraphRegistryEvents()
{
    pw_registry_events e {};
    e.version = PW_VERSION_REGISTRY_EVENTS;
    e.global  = onGraphGlobal;
    return e;
}

pw_core_events makeGraphCoreEvents()
{
    pw_core_events e {};
    e.version = PW_VERSION_CORE_EVENTS;
    e.done    = onGraphDone;
    e.error   = onGraphError;
    return e;
}

const pw_registry_events kGraphRegistryEvents = makeGraphRegistryEvents();
const pw_core_events     kGraphCoreEvents     = makeGraphCoreEvents();

// Registry ids of a node's ports in one direction, ordered by channelOrderKey
// (semantic audio.channel, else port-name index) so link N maps our channel N
// to the device's channel N regardless of registry id assignment.
juce::Array<juce::uint32> orderedPortIds (const GraphScan& s, juce::uint32 nodeId, bool isOutput)
{
    juce::Array<GraphPort> matching;
    for (const auto& p : s.ports)
        if (p.nodeId == nodeId && p.isOutput == isOutput)
            matching.add (p);
    std::sort (matching.begin(), matching.end(),
               [] (const GraphPort& a, const GraphPort& b)
               { return channelOrderKey (a) < channelOrderKey (b); });
    juce::Array<juce::uint32> ids;
    for (const auto& p : matching) ids.add (p.portId);
    return ids;
}
} // namespace

// ----- pure helpers (shared with the self-test) ------------------------------
int PipeWireAudioIODevice::countActiveChannels (const juce::BigInteger& mask)
{
    int n = 0;
    for (int i = 0; i <= mask.getHighestBit(); ++i)
        if (mask[i]) ++n;
    return n;
}

juce::String PipeWireAudioIODevice::formatNodeLatency (int quantum, int sampleRate)
{
    return juce::String (quantum) + "/" + juce::String (sampleRate);
}

// ----- construction ----------------------------------------------------------
PipeWireAudioIODevice::PipeWireAudioIODevice (const juce::String& deviceName,
                                              const juce::String& inId,
                                              const juce::String& outId,
                                              int numInChannels,
                                              int numOutChannels)
    : juce::AudioIODevice (deviceName, "PipeWire"),
      inputId (inId), outputId (outId), displayName (deviceName),
      deviceInChannels (std::max (0, numInChannels)),
      deviceOutChannels (std::max (0, numOutChannels))
{
}

PipeWireAudioIODevice::~PipeWireAudioIODevice()
{
    close();
}

// ----- capability queries ----------------------------------------------------
juce::StringArray PipeWireAudioIODevice::getOutputChannelNames()
{
    // Report the node's real channel count (from enumeration's audio.channels)
    // so a multichannel interface isn't capped at stereo. Fall back to a stereo
    // pair when the node didn't advertise a count.
    juce::StringArray names;
    const int n = deviceOutChannels > 0 ? deviceOutChannels : 2;
    for (int i = 1; i <= n; ++i) names.add ("Out " + juce::String (i));
    return names;
}

juce::StringArray PipeWireAudioIODevice::getInputChannelNames()
{
    juce::StringArray names;
    const int n = deviceInChannels > 0 ? deviceInChannels : 2;
    for (int i = 1; i <= n; ++i) names.add ("In " + juce::String (i));
    return names;
}

juce::Array<double> PipeWireAudioIODevice::getAvailableSampleRates()
{
    // PipeWire resamples transparently around the graph rate, so any common rate
    // is usable regardless of the underlying device.
    return { 44100.0, 48000.0, 88200.0, 96000.0, 176400.0, 192000.0 };
}

juce::Array<int> PipeWireAudioIODevice::getAvailableBufferSizes()
{
    return { 32, 64, 128, 256, 512, 1024, 2048 };
}

int PipeWireAudioIODevice::getDefaultBufferSize() { return 512; }

// ----- open / close ----------------------------------------------------------
juce::String PipeWireAudioIODevice::open (const juce::BigInteger& inputChannels,
                                          const juce::BigInteger& outputChannels,
                                          double sampleRate, int bufferSizeSamples)
{
    if (isDeviceOpen.load (std::memory_order_acquire))
        close();

    currentInputChannels  = inputChannels;
    currentOutputChannels = outputChannels;

    numOutputChannels = countActiveChannels (outputChannels);
    numInputChannels  = countActiveChannels (inputChannels);

    // The device selector activates a fixed wide channel mask; clamp to the
    // interface's real port count (from enumeration) so we don't create dead
    // ports the graph can't link, and the active set the callback/UI see matches
    // what actually exists. deviceOutChannels == 0 means the count is unknown
    // (leave the request as-is).
    if (deviceOutChannels > 0 && numOutputChannels > deviceOutChannels)
    {
        currentOutputChannels.setRange (deviceOutChannels,
            currentOutputChannels.getHighestBit() + 1 - deviceOutChannels, false);
        numOutputChannels = countActiveChannels (currentOutputChannels);
    }
    if (deviceInChannels > 0 && numInputChannels > deviceInChannels)
    {
        currentInputChannels.setRange (deviceInChannels,
            currentInputChannels.getHighestBit() + 1 - deviceInChannels, false);
        numInputChannels = countActiveChannels (currentInputChannels);
    }

    const bool wantOutput = numOutputChannels > 0 && outputId.isNotEmpty();
    const bool wantInput  = numInputChannels  > 0 && inputId.isNotEmpty();
    if (! wantOutput && ! wantInput)
    {
        lastError = "no input or output channels selected";
        return lastError;
    }
    if (! wantOutput) numOutputChannels = 0;
    if (! wantInput)  numInputChannels  = 0;

    const int rate    = (int) sampleRate;
    const int quantum = std::max (32, bufferSizeSamples);
    openedSampleRate  = sampleRate;
    currentBlockSize  = quantum;
    maxQuantum        = kMaxQuantum;

    // Unlinked-port scratch. silenceIn stays zeroed (read by the callback for a
    // port the graph hasn't linked); dumpOut absorbs writes to an unlinked output.
    silenceIn.allocate ((size_t) maxQuantum, true);
    dumpOut  .allocate ((size_t) maxQuantum, true);

    callbackInPointers.resize (numInputChannels);
    callbackOutPointers.resize (numOutputChannels);

    ensurePipeWireInit();

    threadLoop = pw_thread_loop_new ("Dusk Studio PipeWire", nullptr);
    if (threadLoop == nullptr)
    {
        lastError = "pw_thread_loop_new failed";
        close();
        return lastError;
    }

    const char* category = (wantOutput && wantInput) ? "Duplex"
                          : wantOutput                ? "Playback"
                          :                             "Capture";
    const auto latency = formatNodeLatency (quantum, rate);

    auto* props = pw_properties_new (
        PW_KEY_MEDIA_TYPE,       "Audio",
        PW_KEY_MEDIA_CATEGORY,   category,
        PW_KEY_MEDIA_ROLE,       "Production",
        PW_KEY_APP_NAME,         "Dusk Studio",
        PW_KEY_NODE_NAME,        "Dusk Studio",
        PW_KEY_NODE_LATENCY,     latency.toRawUTF8(),
        // We link to the target ourselves (linkToHardware), so keep the session
        // manager's auto-link policy out of it - autoconnect would also try to
        // link the node to the DEFAULT device, routing audio to the wrong place.
        PW_KEY_NODE_AUTOCONNECT, "false",
        // A duplex filter is not a driver and, unlinked, is never scheduled -
        // it sits at PAUSED and on_process never fires. want-driver asks the
        // graph to group it with the hardware driver node so the cycle runs.
        PW_KEY_NODE_WANT_DRIVER,  "true",
        nullptr);
    // Drive the graph to the session's rate + block size (PipeWire otherwise
    // runs at its own quantum/rate and resamples, so our callback would get a
    // different block size than the engine prepared for). force-* holds them
    // while our node is active - this is the DAW acting as the graph's clock.
    pw_properties_setf (props, PW_KEY_NODE_FORCE_RATE,    "%d", rate);
    pw_properties_setf (props, PW_KEY_NODE_FORCE_QUANTUM, "%d", quantum);
    // Node-level target links the primary direction to the chosen device. Ports
    // also carry a per-direction target below; recent WirePlumber honours the
    // port target, older policy the node target, so we set both.
    if (wantOutput)      pw_properties_set (props, PW_KEY_TARGET_OBJECT, outputId.toRawUTF8());
    else if (wantInput)  pw_properties_set (props, PW_KEY_TARGET_OBJECT, inputId.toRawUTF8());
    // Strict target: don't let the session manager fall back to a different node
    // when the requested one is unavailable. The connect then fails to reach
    // PAUSED (see open()'s wait below) instead of silently linking elsewhere.
    pw_properties_set (props, PW_KEY_NODE_DONT_RECONNECT, "true");

    filter = pw_filter_new_simple (pw_thread_loop_get_loop (threadLoop),
                                    "Dusk Studio", props, &kFilterEvents, this);
    if (filter == nullptr)
    {
        // props ownership is taken by pw_filter_new_simple even on failure.
        lastError = "pw_filter_new_simple failed";
        close();
        return lastError;
    }

    inPorts.clearQuick();
    outPorts.clearQuick();

    for (int i = 0; i < numOutputChannels; ++i)
    {
        const auto portName = "playback_" + juce::String (i + 1);
        auto* pp = pw_properties_new (PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                       PW_KEY_PORT_NAME,  portName.toRawUTF8(),
                                       nullptr);
        pw_properties_set (pp, PW_KEY_TARGET_OBJECT, outputId.toRawUTF8());
        void* port = pw_filter_add_port (filter, PW_DIRECTION_OUTPUT,
                                          PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                          0, pp, nullptr, 0);
        if (port == nullptr)
        {
            // pp ownership is taken by pw_filter_add_port even on failure.
            lastError = "pw_filter_add_port (output) failed";
            close();
            return lastError;
        }
        outPorts.add (port);
    }

    for (int i = 0; i < numInputChannels; ++i)
    {
        const auto portName = "capture_" + juce::String (i + 1);
        auto* pp = pw_properties_new (PW_KEY_FORMAT_DSP, "32 bit float mono audio",
                                       PW_KEY_PORT_NAME,  portName.toRawUTF8(),
                                       nullptr);
        pw_properties_set (pp, PW_KEY_TARGET_OBJECT, inputId.toRawUTF8());
        void* port = pw_filter_add_port (filter, PW_DIRECTION_INPUT,
                                          PW_FILTER_PORT_FLAG_MAP_BUFFERS,
                                          0, pp, nullptr, 0);
        if (port == nullptr)
        {
            // pp ownership is taken by pw_filter_add_port even on failure.
            lastError = "pw_filter_add_port (input) failed";
            close();
            return lastError;
        }
        inPorts.add (port);
    }

    // The graph's real port latency isn't known until links are made; report one
    // quantum as the estimate. (Graph-accurate latency via the port latency
    // param is a refinement.)
    outputLatency = wantOutput ? quantum : 0;
    inputLatency  = wantInput  ? quantum : 0;

    // Initialise the RT-thread state before the data thread can run - once
    // started, onProcess may read these on the very first cycle.
    lastXrunRecovering = false;
    xrunCount.store (0, std::memory_order_relaxed);
    negotiatedQuantum.store (0, std::memory_order_relaxed);

    if (const int rc = pw_thread_loop_start (threadLoop); rc < 0)
    {
        lastError = "pw_thread_loop_start failed (" + juce::String (rc) + ")";
        close();
        return lastError;
    }
    threadLoopRunning = true;

    // Connect and wait for the async handshake to resolve, under the loop lock so
    // no state_changed is missed. pw_filter_connect is scheduled on the loop and
    // completes asynchronously - returning success before it lands would report a
    // dead device as open when the target vanished or the server disconnected.
    {
        pw_thread_loop_lock (threadLoop);
        if (pw_filter_connect (filter, PW_FILTER_FLAG_RT_PROCESS, nullptr, 0) < 0)
        {
            pw_thread_loop_unlock (threadLoop);
            lastError = "pw_filter_connect failed";
            close();
            return lastError;
        }

        // A functioning graph resolves in milliseconds; the timeout backstops a
        // wedged server so open() never blocks indefinitely. Each iteration is
        // woken by state_changed (onFilterStateChanged signals the loop) or by
        // the per-wait timeout.
        const char* stateError = nullptr;
        pw_filter_state state = pw_filter_get_state (filter, &stateError);
        for (int tries = 0;
             state != PW_FILTER_STATE_PAUSED
             && state != PW_FILTER_STATE_STREAMING
             && state != PW_FILTER_STATE_ERROR
             && tries < 10;
             ++tries)
        {
            if (pw_thread_loop_timed_wait (threadLoop, 2) != 0)
                break;
            state = pw_filter_get_state (filter, &stateError);
        }
        pw_thread_loop_unlock (threadLoop);

        if (state != PW_FILTER_STATE_PAUSED && state != PW_FILTER_STATE_STREAMING)
        {
            lastError = juce::String ("PipeWire filter did not connect: ")
                        + (stateError != nullptr ? stateError : "timeout");
            close();
            return lastError;
        }
    }

    // The session manager won't link a duplex filter, so link our ports to the
    // chosen device ourselves. Once linked, the graph drives the node (STREAMING)
    // and onProcess starts firing.
    const int nLinks = linkToHardware();
    if (nLinks <= 0)
    {
        lastError = "PipeWire: could not link all device ports";
        close();
        return lastError;
    }

    // Wait for the links to drive the node to STREAMING - that state IS the
    // confirmation the links bound and the graph is running. If it never reaches
    // STREAMING (no links bound, or the driver didn't start), the device would
    // play silence with a frozen transport, so fail the open instead.
    bool streaming = false;
    {
        pw_thread_loop_lock (threadLoop);
        const char* se = nullptr;
        pw_filter_state st = pw_filter_get_state (filter, &se);
        for (int t = 0; st != PW_FILTER_STATE_STREAMING && st != PW_FILTER_STATE_ERROR && t < 10; ++t)
        {
            if (pw_thread_loop_timed_wait (threadLoop, 1) != 0)
                break;
            st = pw_filter_get_state (filter, &se);
        }
        streaming = (st == PW_FILTER_STATE_STREAMING);
        pw_thread_loop_unlock (threadLoop);
    }
    if (! streaming)
    {
        lastError = "PipeWire node did not start streaming ("
                    + juce::String (nLinks) + " link(s) created)";
        close();
        return lastError;
    }

    // Adopt the graph's real quantum: PipeWire runs the graph at its own block
    // size (our request is only advisory), so the engine must prepare for what
    // onProcess actually delivers - otherwise its oversized-block guard clears
    // every block to silence. A quantum above maxQuantum is itself dropped by
    // onProcess, so treat "no usable quantum" as a failed open rather than
    // silently keeping the requested size.
    for (int t = 0; t < 100 && negotiatedQuantum.load (std::memory_order_relaxed) == 0; ++t)
        std::this_thread::sleep_for (std::chrono::milliseconds (2));
    const int nq = negotiatedQuantum.load (std::memory_order_relaxed);
    if (nq <= 0 || nq > maxQuantum)
    {
        lastError = "PipeWire delivered no usable quantum (" + juce::String (nq) + ")";
        close();
        return lastError;
    }
    currentBlockSize = nq;
    if (wantOutput) outputLatency = nq;
    if (wantInput)  inputLatency  = nq;

    isDeviceOpen.store (true, std::memory_order_release);
    lastError.clear();

    std::fprintf (stderr,
                  "[Dusk Studio/PipeWire] opened \"%s\" rate=%d quantum=%d out=%dch in=%dch "
                  "links=%d target-out=\"%s\" target-in=\"%s\"\n",
                  displayName.toRawUTF8(), rate, currentBlockSize,
                  numOutputChannels, numInputChannels, linkProxies.size(),
                  wantOutput ? outputId.toRawUTF8() : "-",
                  wantInput  ? inputId.toRawUTF8()  : "-");

    return {};
}

int PipeWireAudioIODevice::linkToHardware()
{
    // Filter accessors touch objects on the thread loop; briefly lock for them
    // (but NOT across the registry roundtrip below, which would stall the RT
    // thread for the whole sync).
    pw_thread_loop_lock (threadLoop);
    const juce::uint32 ourNode = pw_filter_get_node_id (filter);
    pw_thread_loop_unlock (threadLoop);
    if (ourNode == (juce::uint32) SPA_ID_INVALID)
        return 0;

    // Throwaway registry roundtrip to discover the global port ids of our node
    // and the target device nodes. Object ids are graph-global, so ids found on
    // this scratch connection are valid to reference from the filter's core.
    GraphScan s;
    s.loop = pw_main_loop_new (nullptr);
    if (s.loop == nullptr) return 0;
    auto* ctx = pw_context_new (pw_main_loop_get_loop (s.loop), nullptr, 0);
    if (ctx == nullptr) { pw_main_loop_destroy (s.loop); return 0; }
    auto* core = pw_context_connect (ctx, nullptr, 0);
    if (core == nullptr) { pw_context_destroy (ctx); pw_main_loop_destroy (s.loop); return 0; }
    auto* registry = pw_core_get_registry (core, PW_VERSION_REGISTRY, 0);
    if (registry == nullptr)
    {
        pw_core_disconnect (core); pw_context_destroy (ctx); pw_main_loop_destroy (s.loop);
        return 0;
    }

    spa_hook rHook {}, cHook {};
    pw_registry_add_listener (registry, &rHook, &kGraphRegistryEvents, &s);
    pw_core_add_listener (core, &cHook, &kGraphCoreEvents, &s);
    s.syncSeq  = pw_core_sync (core, PW_ID_CORE, 0);
    s.haveSync = true;

    auto* pwLoop = pw_main_loop_get_loop (s.loop);
    auto* timer  = pw_loop_add_timer (pwLoop, onGraphTimeout, &s);
    struct timespec timeout {};
    timeout.tv_sec = 2;
    pw_loop_update_timer (pwLoop, timer, &timeout, nullptr, false);

    pw_main_loop_run (s.loop);

    pw_loop_destroy_source (pwLoop, timer);
    spa_hook_remove (&rHook);
    spa_hook_remove (&cHook);
    pw_proxy_destroy ((pw_proxy*) registry);
    pw_core_disconnect (core);
    pw_context_destroy (ctx);
    pw_main_loop_destroy (s.loop);
    s.loop = nullptr;

    const bool wantOut = numOutputChannels > 0 && outputId.isNotEmpty();
    const bool wantIn  = numInputChannels  > 0 && inputId.isNotEmpty();
    const juce::uint32 sinkNode   = wantOut ? s.resolveNode (outputId) : (juce::uint32) SPA_ID_INVALID;
    const juce::uint32 sourceNode = wantIn  ? s.resolveNode (inputId)  : (juce::uint32) SPA_ID_INVALID;

    const auto ourOut = orderedPortIds (s, ourNode, true);
    const auto ourIn  = orderedPortIds (s, ourNode, false);
    const auto sinkIn = sinkNode   != (juce::uint32) SPA_ID_INVALID ? orderedPortIds (s, sinkNode,   false)
                                                                    : juce::Array<juce::uint32>();
    const auto srcOut = sourceNode != (juce::uint32) SPA_ID_INVALID ? orderedPortIds (s, sourceNode, true)
                                                                    : juce::Array<juce::uint32>();

    // Every port we created must have a matching device port, or routing would
    // be partial. numOut/InChannels were clamped to the device's real port count
    // in open(), so this holds exactly unless the device changed between the
    // enumeration and now - in which case fail rather than half-wire it.
    if (sinkIn.size() < ourOut.size() || srcOut.size() < ourIn.size())
        return -1;

    const int wantLinks = ourOut.size() + ourIn.size();

    int made = 0;
    pw_thread_loop_lock (threadLoop);
    auto* filterCore = pw_filter_get_core (filter);
    if (filterCore != nullptr)
    {
        auto makeLink = [&] (juce::uint32 outNode, juce::uint32 outPort,
                             juce::uint32 inNode,  juce::uint32 inPort)
        {
            auto* lp = pw_properties_new (nullptr, nullptr);
            pw_properties_setf (lp, PW_KEY_LINK_OUTPUT_NODE, "%u", outNode);
            pw_properties_setf (lp, PW_KEY_LINK_OUTPUT_PORT, "%u", outPort);
            pw_properties_setf (lp, PW_KEY_LINK_INPUT_NODE,  "%u", inNode);
            pw_properties_setf (lp, PW_KEY_LINK_INPUT_PORT,  "%u", inPort);
            auto* proxy = (pw_proxy*) pw_core_create_object (filterCore, "link-factory",
                PW_TYPE_INTERFACE_Link, PW_VERSION_LINK, &lp->dict, 0);
            pw_properties_free (lp);
            if (proxy != nullptr) { linkProxies.add (proxy); ++made; }
        };
        for (int i = 0; i < ourOut.size(); ++i)
            makeLink (ourNode, ourOut.getUnchecked (i), sinkNode,   sinkIn.getUnchecked (i));
        for (int j = 0; j < ourIn.size(); ++j)
            makeLink (sourceNode, srcOut.getUnchecked (j), ourNode, ourIn.getUnchecked (j));
    }
    pw_thread_loop_unlock (threadLoop);

    // A partial link set (a proxy failed to create) would route only some
    // channels; signal failure so open() rejects it rather than half-wiring.
    return (wantLinks > 0 && made == wantLinks) ? made : -1;
}

void PipeWireAudioIODevice::close()
{
    stop();

    // Destroy the links we created (on the filter's core) before stopping the
    // loop - proxy destruction touches the loop, so it needs the lock and a
    // running loop.
    if (threadLoop != nullptr && threadLoopRunning && ! linkProxies.isEmpty())
    {
        pw_thread_loop_lock (threadLoop);
        for (auto* p : linkProxies)
            if (p != nullptr) pw_proxy_destroy ((pw_proxy*) p);
        pw_thread_loop_unlock (threadLoop);
    }
    linkProxies.clearQuick();

    // pw_thread_loop_stop must be called WITHOUT the lock held; it joins the RT
    // thread, after which teardown races nothing. Only stop a loop we actually
    // started - an open() that failed before pw_thread_loop_start leaves the
    // loop created-but-not-running.
    if (threadLoop != nullptr && threadLoopRunning)
    {
        pw_thread_loop_stop (threadLoop);
        threadLoopRunning = false;
    }

    if (filter != nullptr)     { pw_filter_destroy (filter);      filter = nullptr; }
    if (threadLoop != nullptr) { pw_thread_loop_destroy (threadLoop); threadLoop = nullptr; }

    inPorts.clearQuick();
    outPorts.clearQuick();
    silenceIn.free();
    dumpOut.free();

    isDeviceOpen.store (false, std::memory_order_release);
}

// ----- start / stop ----------------------------------------------------------
void PipeWireAudioIODevice::start (juce::AudioIODeviceCallback* newCallback)
{
    if (! isDeviceOpen.load (std::memory_order_acquire) || newCallback == nullptr)
        return;
    if (isStarted.load (std::memory_order_acquire))
        return;

    newCallback->audioDeviceAboutToStart (this);

    {
        const juce::ScopedLock sl (callbackLock);
        callback = newCallback;
    }

    isStarted.store (true, std::memory_order_release);
}

void PipeWireAudioIODevice::stop()
{
    if (! isStarted.load (std::memory_order_acquire))
        return;

    isStarted.store (false, std::memory_order_release);

    juce::AudioIODeviceCallback* cb = nullptr;
    {
        const juce::ScopedLock sl (callbackLock);
        std::swap (cb, callback);
    }
    if (cb != nullptr)
        cb->audioDeviceStopped();
}

// ----- RT process ------------------------------------------------------------
void PipeWireAudioIODevice::onProcess (struct spa_io_position* position) noexcept
{
    const juce::uint32 n = position != nullptr ? (juce::uint32) position->clock.duration : 0;
    if (n == 0)
        return;

    negotiatedQuantum.store ((int) n, std::memory_order_relaxed);

    // Count one xrun per recovery episode: increment on the rising edge of the
    // XRUN_RECOVER flag, not on every cycle it stays set (a recovery can span
    // several cycles). clock.xrun is only an estimated duration, so the flag is
    // the reliable event signal.
    if (position != nullptr)
    {
        const bool recovering = (position->clock.flags & SPA_IO_CLOCK_FLAG_XRUN_RECOVER) != 0;
        if (recovering && ! lastXrunRecovering)
            xrunCount.fetch_add (1, std::memory_order_relaxed);
        lastXrunRecovering = recovering;
    }

    // A cycle larger than our scratch ceiling can't be serviced safely without
    // allocating; clear any real output buffers and skip the callback.
    if (n > (juce::uint32) maxQuantum)
    {
        for (int i = 0; i < numOutputChannels; ++i)
            if (auto* b = (float*) pw_filter_get_dsp_buffer (outPorts.getUnchecked (i), n))
                std::memset (b, 0, (size_t) n * sizeof (float));
        return;
    }

    for (int i = 0; i < numOutputChannels; ++i)
    {
        auto* b = (float*) pw_filter_get_dsp_buffer (outPorts.getUnchecked (i), n);
        callbackOutPointers.setUnchecked (i, b != nullptr ? b : dumpOut.getData());
    }
    for (int i = 0; i < numInputChannels; ++i)
    {
        auto* b = (const float*) pw_filter_get_dsp_buffer (inPorts.getUnchecked (i), n);
        callbackInPointers.setUnchecked (i, b != nullptr ? b : silenceIn.getData());
    }

    // Clear the output buffers up front so channels the callback leaves untouched
    // (or every channel when stopped) are silence, not stale graph memory.
    for (int i = 0; i < numOutputChannels; ++i)
        std::memset (callbackOutPointers.getUnchecked (i), 0, (size_t) n * sizeof (float));

    const juce::ScopedLock sl (callbackLock);
    if (callback != nullptr)
        callback->audioDeviceIOCallbackWithContext (
            callbackInPointers.getRawDataPointer(),  numInputChannels,
            callbackOutPointers.getRawDataPointer(), numOutputChannels,
            (int) n, {});
}

void PipeWireAudioIODevice::onFilterStateChanged() noexcept
{
    // Wake open()'s bounded wait; it re-reads pw_filter_get_state for the truth.
    if (threadLoop != nullptr)
        pw_thread_loop_signal (threadLoop, false);
}

// ----- self-test -------------------------------------------------------------
juce::String PipeWireAudioIODevice::runSelfTest()
{
    juce::StringArray out;
    auto check = [&out] (bool ok, const juce::String& what)
    { out.add ((ok ? "[PASS] " : "[FAIL] ") + what); };

    // Active-channel counting from a mask (drives port creation).
    {
        juce::BigInteger m;
        m.setBit (0); m.setBit (1); m.setBit (5);
        check (countActiveChannels (m) == 3, "PipeWire: countActiveChannels stereo+1 = 3");
        juce::BigInteger empty;
        check (countActiveChannels (empty) == 0, "PipeWire: countActiveChannels empty = 0");
    }

    // node.latency string formatting (quantum/rate).
    check (formatNodeLatency (512, 48000) == "512/48000", "PipeWire: node.latency format 512/48000");
    check (formatNodeLatency (64, 44100)  == "64/44100",  "PipeWire: node.latency format 64/44100");

    // Channel ordering key that maps our ports to the device's ports for
    // manual linking. Hardware ports order by audio.channel; our DSP ports by
    // the trailing index in their name.
    {
        auto hw = [] (const char* ch) { GraphPort p; p.channel = ch; return channelOrderKey (p); };
        check (hw ("FL") < hw ("FR"),     "PipeWire: channel order FL before FR");
        check (hw ("AUX0") < hw ("AUX1"), "PipeWire: channel order AUX0 before AUX1");
        check (hw ("AUX2") < hw ("AUX10"),"PipeWire: channel order AUX2 before AUX10");
        auto ours = [] (const char* nm) { GraphPort p; p.name = nm; return channelOrderKey (p); };
        check (ours ("playback_2") < ours ("playback_10"),
               "PipeWire: our port order playback_2 before playback_10");
    }

    return out.joinIntoString ("\n");
}
} // namespace duskstudio
