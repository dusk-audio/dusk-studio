#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_core/juce_core.h>
#include <alsa/asoundlib.h>
#include <atomic>

namespace duskstudio
{
// Dusk Studio-owned ALSA audio I/O. One instance per open device pair (a playback
// PCM, a capture PCM, or both linked together). Implements juce::AudioIODevice
// so the rest of Dusk Studio (AudioDeviceManager, AudioDeviceSelectorComponent,
// AudioEngine's callback) can use it interchangeably with the JACK backend.
//
// Design choices, for new readers:
//   - Raw `hw:CARD,DEV` PCMs only. No `plug:`/`default:`/`front:` aliases -
//     those route through alsa-lib's plug plugin which on PipeWire systems
//     gets intercepted, defeating the point of "direct hardware".
//   - RW interleaved access only. MMAP correctness varies per-driver across
//     the long tail of USB-class-compliant interfaces; the zero-copy gain
//     is theoretical and the failure mode (silent or distorted output) is
//     awful. Conservative-correct beats fast-broken.
//   - Format priority by reliability: S32_LE first, S16_LE second, S24_LE
//     third, S24_3LE fourth, FLOAT_LE last. S32_LE has zero alignment
//     ambiguity; FLOAT_LE is the format most likely to be falsely advertised.
//   - Conservative sw_params: start_threshold = period_size, stop_threshold
//     = buffer_size, no silence-fill override. Underrun stops the device,
//     recovery restarts it cleanly. The xrun counter is surfaced to the UI
//     so the user knows.
//   - Open at the device's reported max channel count; map JUCE's active-
//     channel mask to per-channel float buffers; zero inactive slots in the
//     interleaved frame.
//   - SCHED_RR I/O thread, priority sized below RLIMIT_RTPRIO so
//     pthread_create won't EPERM. mlockall() is done once at app start.
//
// Patterns referenced from Ardour's libs/backends/alsa/zita-alsa-pcmi.cc
// (study only, no copied code).
class AlsaAudioIODevice final : public juce::AudioIODevice,
                                  private juce::Thread
{
public:
    AlsaAudioIODevice (const juce::String& deviceName,
                       const juce::String& inputId,
                       const juce::String& outputId);
    ~AlsaAudioIODevice() override;

    // Identifiers used by the AudioIODeviceType to look up this instance.
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
    bool  isOpen() override                                   { return isDeviceOpen.load (std::memory_order_acquire); }

    void  start (juce::AudioIODeviceCallback* newCallback) override;
    void  stop() override;
    bool  isPlaying() override                                { return isStarted.load (std::memory_order_acquire); }

    juce::String getLastError() override                       { return lastError; }

    int    getCurrentBufferSizeSamples() override              { return periodSize; }
    double getCurrentSampleRate()        override              { return openedSampleRate; }
    int    getCurrentBitDepth()          override              { return openedBitDepth; }

    juce::BigInteger getActiveOutputChannels() const override  { return currentOutputChannels; }
    juce::BigInteger getActiveInputChannels()  const override  { return currentInputChannels; }

    int getOutputLatencyInSamples() override                   { return outputLatency; }
    int getInputLatencyInSamples()  override                   { return inputLatency; }

    int getXRunCount() const noexcept override                 { return xrunCount.load (std::memory_order_relaxed); }

    // Periods-per-buffer override. Reads as the value that will be used on
    // the NEXT open(). Default 2 (Ardour-style; the lowest value that gives
    // the kernel any slack and is the lowest-latency setting that's stable
    // on most modern interfaces). Range clamped to [2, 16].
    static void setRequestedPeriods (int p) noexcept;
    static int  getRequestedPeriods() noexcept;

    // Synthetic backend self-test. Exercises the pure-logic surfaces that
    // don't need real hardware: float<->int sample conversion round-trip
    // for every format we negotiate, channel-mask routing into the
    // interleaved frame, hw:CARD,DEV id parsing for cross-card detection,
    // and periods-knob clamping. Returns a multi-line "[PASS] ..." /
    // "[FAIL] ..." report. AudioPipelineSelfTest::runAll() invokes this
    // alongside the engine pipeline tests, so DUSKSTUDIO_RUN_SELFTEST=1 picks
    // it up. Real-device opens are covered by the existing backend cycle
    // section of AudioPipelineSelfTest, not here.
    static juce::String runSelfTest();

private:
    void run() override;  // SCHED_RR I/O thread

    // hw_params + sw_params negotiation. Caller passes the requested values;
    // these may be modified by the kernel's "near" snapping. Returns true
    // on success, sets lastError on failure.
    bool configurePcm (snd_pcm_t* handle, bool isCapture,
                        unsigned int& sampleRate,
                        unsigned int& numChannels,
                        snd_pcm_uframes_t& period,
                        unsigned int& periods,
                        int& bytesPerSample, bool& sampleIsFloat);

    bool openOneHandle (const juce::String& id, bool isCapture, snd_pcm_t*& handle);

    // Recover from an EPIPE / ESTRPIPE / EBADFD; bumps xrunCount. Returns
    // 0 on successful recovery, the original errno on failure. On success it
    // also re-arms the stream (see rearmStream) — snd_pcm_recover alone leaves
    // a linked duplex device PREPARED-but-not-RUNNING, which stalls audio.
    int recoverFromXrun (snd_pcm_t* handle, int err);

    // Restart a recovered stream: prepare both handles, prefill the playback
    // ring past start_threshold from silencePrefill, then snd_pcm_start. Mirrors
    // the start sequence in open(). Audio-thread safe — no allocation.
    void rearmStream() noexcept;

    // Convert one period's worth of float samples to the negotiated sample
    // type, interleaved across all device channels. Inactive channels are
    // zero-filled. The reverse for capture.
    void interleavePlaybackBlock (const juce::AudioBuffer<float>& src,
                                   void* destInterleaved, int numFrames) const;
    void deinterleaveCaptureBlock (const void* srcInterleaved,
                                    juce::AudioBuffer<float>& dest, int numFrames) const;

    // Capability cache populated lazily on first sample-rate / buffer-size
    // query (or open()). Avoids re-probing the device for every UI redraw.
    // Message-thread only: callers are JUCE's channel-name / rate / buffer
    // queries and open(), all driven from AudioDeviceManager. The cached
    // members below are not synchronised; do not call from the I/O thread.
    void probeIfNeeded();

    // State ---------------------------------------------------------------
    const juce::String displayName;

    // Capability cache (probed once, cleared on close).
    bool                hasProbed         = false;
    unsigned int        deviceMaxOutChannels = 0;
    unsigned int        deviceMaxInChannels  = 0;
    juce::Array<double> supportedSampleRates;
    juce::Array<int>    supportedBufferSizes;

    // Negotiated values from the most recent successful open().
    snd_pcm_t* outHandle = nullptr;
    snd_pcm_t* inHandle  = nullptr;
    int        periodSize       = 0;       // frames per period (block size)
    int        periodsCount      = 0;
    double     openedSampleRate  = 0.0;
    int        openedBitDepth    = 0;       // 16, 24, 32
    bool       openedIsFloat     = false;
    int        bytesPerOutSample = 0;
    int        bytesPerInSample  = 0;
    unsigned int outNumChannels  = 0;       // device hardware channel count
    unsigned int inNumChannels   = 0;
    int        outputLatency     = 0;
    int        inputLatency      = 0;

    juce::BigInteger currentOutputChannels, currentInputChannels;
    juce::Array<int> activeOutDeviceChannelIndex; // active-callback-i -> device-ch-j
    juce::Array<int> activeInDeviceChannelIndex;

    // Per-period scratch (allocated on open, never reallocated on the audio
    // thread).
    juce::HeapBlock<char>     interleavedOutBytes;
    juce::HeapBlock<char>     interleavedInBytes;
    // periodSize * periodsCount frames of zeroed output bytes, pre-allocated at
    // open so rearmStream can refill the playback ring without allocating on
    // the audio thread.
    juce::HeapBlock<char>     silencePrefill;
    juce::AudioBuffer<float>  callbackOutFloats;  // sized [numChannelsActive, periodSize]
    juce::AudioBuffer<float>  callbackInFloats;
    juce::Array<float*>       callbackOutPointers;
    juce::Array<const float*> callbackInPointers;

    juce::CriticalSection callbackLock;
    juce::AudioIODeviceCallback* callback = nullptr;

    std::atomic<bool> isDeviceOpen { false };
    std::atomic<bool> isStarted    { false };
    std::atomic<int>  xrunCount    { 0 };

    juce::String lastError;

    JUCE_DECLARE_NON_COPYABLE_WITH_LEAK_DETECTOR (AlsaAudioIODevice)
};
} // namespace duskstudio
