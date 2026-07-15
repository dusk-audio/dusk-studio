#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <atomic>

// libpipewire types kept out of this header - forward-declared so callers that
// only need the juce::AudioIODevice interface don't pull <pipewire/pipewire.h>.
struct pw_thread_loop;
struct pw_filter;
struct spa_io_position;

namespace duskstudio
{
// Native PipeWire audio I/O. One instance per open device (a Sink, a Source,
// or a Source+Sink pair driven as one duplex client). Implements
// juce::AudioIODevice so AudioEngine's callback, AudioDeviceManager and the
// selector UI use it interchangeably with the ALSA backend.
//
// Design choices, for new readers:
//   - ONE pw_filter node with N input ports + M output ports, all processed in
//     a single graph cycle. This is the JACK model JUCE's single duplex
//     callback expects - capture and playback are inherently sample-aligned
//     because they share the cycle. (pw_stream would need two nodes with no
//     cross-node sync guarantee.)
//   - Ports carry SPA_AUDIO_FORMAT_F32P (planar float) - PipeWire's native
//     working format, so no per-sample int<->float conversion and no format
//     negotiation guessing. The graph resamples around us if the device rate
//     differs.
//   - pw_thread_loop owns the RT thread; on_process fires on it and calls
//     straight into the JUCE callback. No separate SCHED_RR thread (PipeWire
//     already runs its data thread at RT priority).
//   - Target node chosen via PW_KEY_TARGET_OBJECT = the node name captured at
//     enumeration; the session manager links our ports to it.
//   - Quantum (block size) requested via PW_KEY_NODE_LATENCY; the graph may
//     round it. getCurrentBufferSizeSamples reports the value actually used.
class PipeWireAudioIODevice final : public juce::AudioIODevice
{
public:
    PipeWireAudioIODevice (const juce::String& deviceName,
                           const juce::String& inputId,
                           const juce::String& outputId,
                           int numInputChannels,
                           int numOutputChannels);
    ~PipeWireAudioIODevice() override;

    // Identifiers (PW_KEY_NODE_NAME) the type uses to look this instance up.
    const juce::String inputId, outputId;

    // juce::AudioIODevice ------------------------------------------------------
    juce::StringArray  getOutputChannelNames() override;
    juce::StringArray  getInputChannelNames()  override;
    juce::Array<double> getAvailableSampleRates() override;
    juce::Array<int>    getAvailableBufferSizes() override;
    int                 getDefaultBufferSize()    override;

    juce::String open (const juce::BigInteger& inputChannels,
                        const juce::BigInteger& outputChannels,
                        double sampleRate, int bufferSizeSamples) override;
    void  close()  override;
    bool  isOpen() override                    { return isDeviceOpen.load (std::memory_order_acquire); }

    void  start (juce::AudioIODeviceCallback* newCallback) override;
    void  stop() override;
    bool  isPlaying() override                 { return isStarted.load (std::memory_order_acquire); }

    juce::String getLastError() override        { return lastError; }

    int    getCurrentBufferSizeSamples() override { return currentBlockSize; }
    double getCurrentSampleRate()        override { return openedSampleRate; }
    int    getCurrentBitDepth()          override { return 32; }  // F32 float throughout

    juce::BigInteger getActiveOutputChannels() const override { return currentOutputChannels; }
    juce::BigInteger getActiveInputChannels()  const override { return currentInputChannels; }

    int getOutputLatencyInSamples() override    { return outputLatency; }
    int getInputLatencyInSamples()  override    { return inputLatency; }

    int getXRunCount() const noexcept override  { return xrunCount.load (std::memory_order_relaxed); }

    // Synthetic backend self-test - exercises the pure-logic surfaces that need
    // no live graph (active-channel counting from a mask, node.latency string
    // formatting). Returns a multi-line "[PASS]/[FAIL]" report;
    // AudioPipelineSelfTest::runAll() picks it up under DUSKSTUDIO_RUN_SELFTEST=1.
    static juce::String runSelfTest();

    // Pure helpers shared by open() and the self-test (no live graph needed).
    static int         countActiveChannels (const juce::BigInteger& mask);
    static juce::String formatNodeLatency (int quantum, int sampleRate);

    // pw_filter process entry point. Public only so the C trampoline in the .cpp
    // can reach it; not part of the juce::AudioIODevice contract. Runs on
    // PipeWire's RT data thread; allocation-free, lock-free bar the uncontended
    // callback lock.
    void onProcess (struct spa_io_position* position) noexcept;

    // pw_filter state-change entry point (C trampoline in the .cpp). Runs on the
    // thread loop; wakes open()'s bounded wait so it can read the resolved state.
    void onFilterStateChanged() noexcept;

private:
    const juce::String displayName;

    // Device channel counts from enumeration (the node's audio.channels). Bound
    // the reported channel-name lists so a multichannel interface isn't capped
    // at stereo. numInput/OutputChannels below are the ACTIVE counts at open().
    const int deviceInChannels;
    const int deviceOutChannels;

    // libpipewire objects (owned; torn down in close()). pw_filter_new_simple
    // manages its own context/core internally, so we hold only the thread loop
    // (owns the RT data thread) and the filter node.
    pw_thread_loop* threadLoop = nullptr;
    pw_filter*      filter     = nullptr;
    bool            threadLoopRunning = false;  // gate stop() - only stop a started loop

    // Port userdata blocks (one per active channel, dense). Opaque void* -
    // pw_filter owns the port lifetime; we pass these to pw_filter_get_dsp_buffer.
    juce::Array<void*> inPorts;
    juce::Array<void*> outPorts;

    double openedSampleRate = 0.0;
    int    currentBlockSize = 0;
    int    maxQuantum       = 0;      // scratch ceiling; cycles above this are dropped
    int    outputLatency    = 0;
    int    inputLatency     = 0;
    int    numInputChannels  = 0;
    int    numOutputChannels = 0;
    bool         lastXrunRecovering = false;  // XRUN_RECOVER edge state (RT thread only)

    juce::BigInteger currentOutputChannels, currentInputChannels;

    juce::CriticalSection callbackLock;
    juce::AudioIODeviceCallback* callback = nullptr;

    // Pointer arrays handed to the JUCE callback (sized on open, only indexed on
    // the RT thread). For an unlinked port the slot points at the shared scratch.
    juce::Array<const float*> callbackInPointers;
    juce::Array<float*>       callbackOutPointers;

    // Read-only silence for unlinked input ports; write-only dump for unlinked
    // output ports. Sized to maxQuantum at open, never reallocated on the RT thread.
    juce::HeapBlock<float> silenceIn;
    juce::HeapBlock<float> dumpOut;

    std::atomic<bool> isDeviceOpen { false };
    std::atomic<bool> isStarted    { false };
    std::atomic<int>  xrunCount    { 0 };

    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (PipeWireAudioIODevice)
};
} // namespace duskstudio
