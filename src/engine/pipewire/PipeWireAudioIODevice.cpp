#include "PipeWireAudioIODevice.h"

#include <pipewire/pipewire.h>
#include <pipewire/filter.h>
#include <spa/node/io.h>

#include <algorithm>
#include <mutex>
#include <cstdio>
#include <cstring>

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
        PW_KEY_MEDIA_TYPE,     "Audio",
        PW_KEY_MEDIA_CATEGORY, category,
        PW_KEY_MEDIA_ROLE,     "Production",
        PW_KEY_APP_NAME,       "Dusk Studio",
        PW_KEY_NODE_NAME,      "Dusk Studio",
        PW_KEY_NODE_LATENCY,   latency.toRawUTF8(),
        nullptr);
    // Node-level target links the primary direction to the chosen device. Ports
    // also carry a per-direction target below; recent WirePlumber honours the
    // port target, older policy the node target, so we set both.
    if (wantOutput)      pw_properties_set (props, PW_KEY_TARGET_OBJECT, outputId.toRawUTF8());
    else if (wantInput)  pw_properties_set (props, PW_KEY_TARGET_OBJECT, inputId.toRawUTF8());

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
    lastXrun = 0;
    xrunCount.store (0, std::memory_order_relaxed);

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

    isDeviceOpen.store (true, std::memory_order_release);
    lastError.clear();

    std::fprintf (stderr,
                  "[Dusk Studio/PipeWire] opened \"%s\" rate=%d quantum=%d out=%dch in=%dch "
                  "target-out=\"%s\" target-in=\"%s\"\n",
                  displayName.toRawUTF8(), rate, quantum,
                  numOutputChannels, numInputChannels,
                  wantOutput ? outputId.toRawUTF8() : "-",
                  wantInput  ? inputId.toRawUTF8()  : "-");

    return {};
}

void PipeWireAudioIODevice::close()
{
    stop();

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

    // Surface the graph's accumulated xrun as an event count for the UI.
    if (position != nullptr)
    {
        const juce::uint64 x = position->clock.xrun;
        if (x > lastXrun) { xrunCount.fetch_add (1, std::memory_order_relaxed); lastXrun = x; }
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

    return out.joinIntoString ("\n");
}
} // namespace duskstudio
