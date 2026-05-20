#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_dsp/juce_dsp.h>
#include <atomic>
#include <vector>

namespace duskstudio
{
// HardwareInsertParams + HardwareInsertRouting live in Session.h
// (`src/session/Session.h`) alongside the other per-strip POD param
// structs (ChannelStripParams, AuxLaneParams). The slot only needs a
// pointer to them at audio time, so a forward declaration is enough -
// keeps juce_dsp out of Session.h.
struct HardwareInsertParams;
struct HardwareInsertRouting;

// Hardware insert audio-thread DSP. Sends the strip's signal out to
// a pair of physical audio outputs, captures the return from a pair
// of physical audio inputs, optionally re-encodes to Mid/Side, and
// mixes the result against a dry copy of the strip's signal that is
// delayed to phase-align with the hardware's round-trip latency.
//
// The slot owns the dry-path delay line. The wet return arrives
// naturally late (the hardware physically delays the audio), so the
// dry copy is delayed by the configured latencySamples to keep the
// dry/wet mix phase-coherent. The strip also reports getLatencySamples
// to the engine's PDC aggregator so the rest of the session is
// delayed by the same amount and the inserted track stays time-
// aligned with the timeline.
class HardwareInsertSlot
{
public:
    HardwareInsertSlot();
    ~HardwareInsertSlot();

    HardwareInsertSlot (const HardwareInsertSlot&) = delete;
    HardwareInsertSlot& operator= (const HardwareInsertSlot&) = delete;
    HardwareInsertSlot (HardwareInsertSlot&&) = delete;
    HardwareInsertSlot& operator= (HardwareInsertSlot&&) = delete;

    // Message thread. Sets up the per-channel delay lines and the
    // smoothers. Safe to call multiple times - idempotent.
    void prepare (double sampleRate, int blockSize);

    // Message thread, audio-processing STOPPED only. Latches a
    // reference to the live parameter struct. The audio thread reads
    // through paramsRef in processStereoBlock; calling bind() while
    // audio is running is a data race because paramsRef is a plain
    // pointer (not atomic). Caller must keep `params` alive for the
    // duration of audio processing.
    void bind (const HardwareInsertParams& params) noexcept;

    // Audio thread. Reads + writes the strip's stereo buffer in
    // place. deviceInputs / deviceOutputs are the raw device I/O
    // pointer arrays handed to AudioEngine's callback (see Phase 2);
    // routing.{input,output}Ch{L,R} index into them. Out-of-range
    // routing falls through to a dry-only path so a stale config
    // never crashes the audio thread.
    void processStereoBlock (float* L, float* R, int numSamples,
                              const float* const* deviceInputs,
                              int numDeviceInputs,
                              float* const*       deviceOutputs,
                              int numDeviceOutputs) noexcept;

    // Reported to the engine's PDC aggregator. Cached at the top of
    // each process block so the engine sees one consistent value
    // per callback even if the user moves the latency slider mid-
    // playback. Atomic so message-thread PDC callers and the audio
    // thread can both touch it without a torn read.
    int getLatencySamples() const noexcept
    {
        return cachedLatencySamples.load (std::memory_order_relaxed);
    }

    // Called by the strip's mode-flip crossfade gate (Phase 3) when
    // an insert is being swapped in or out. Drops all dry-path delay
    // history so the new path starts clean.
    void resetTailsAndDelayLine() noexcept;

    // Pre-sized so changing latencySamples at runtime never allocates.
    // 16384 samples = ~340 ms at 48 kHz, ample for any realistic
    // outboard round-trip.
    static constexpr int kMaxDelaySamples = 16384;

    // Ping calibration sizing. Chirp = 100 ms @ session SR, capped at
    // 9600 samples. Capture = 8192 samples (= ~170 ms at 48 kHz).
    // Correlation runs ~256 candidate lags per block until the window
    // is exhausted (~32 blocks ≈ 340 ms result latency after capture).
    static constexpr int kChirpMaxSamples   = 9600;
    static constexpr int kCaptureSamples    = 8192;
    static constexpr int kCorrelationsPerBlock = 256;

private:
    const HardwareInsertParams* paramsRef = nullptr;
    double prepSampleRate = 0.0;
    int    prepBlockSize  = 0;

    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>
        dryDelayL { kMaxDelaySamples };
    juce::dsp::DelayLine<float, juce::dsp::DelayLineInterpolationTypes::None>
        dryDelayR { kMaxDelaySamples };

    juce::SmoothedValue<float> outGainLin;
    juce::SmoothedValue<float> inGainLin;
    juce::SmoothedValue<float> dryWetSmooth;

    // Atomic so getLatencySamples() can be polled from any thread (the
    // engine's PDC aggregator polls from the message thread) without
    // tearing the value the audio thread is updating each block.
    std::atomic<int> cachedLatencySamples { 0 };

    // Ping state machine, audio-thread only. Pre-rendered chirp,
    // capture ring, correlation scratch all sized at prepare() so no
    // per-block allocation. The UI flips paramsRef->pingPending to
    // start; processStereoBlock runs the state machine; when result
    // lands, paramsRef->pingResult holds the measured sample lag and
    // pingPending is cleared.
    enum class PingState : int { Idle = 0, Playing = 1, Capturing = 2, Correlating = 3 };

    PingState pingState        = PingState::Idle;
    int       chirpLength      = 0;   // current chirp length in samples
    int       pingPlayPos      = 0;
    int       pingCapturePos   = 0;
    int       pingCorrelateK   = 0;
    float     pingBestPeak     = 0.0f;
    int       pingBestK        = -1;
    float     pingAutoPeak     = 0.0f;   // chirp's own auto-correlation peak for threshold

    // Stall watchdog for the Capturing state. If the user clears the
    // input combo mid-ping or hot-swaps the device, inValid stays false
    // forever and pingCapturePos never advances. Bail with a failure
    // result after the counter exceeds the threshold so the UI doesn't
    // hang on a disabled "Pinging..." button.
    int       pingCaptureStallSamples = 0;
    static constexpr int kPingCaptureStallMax = 2 * kCaptureSamples;

    std::vector<float> chirpBuffer;     // pre-rendered excitation signal
    std::vector<float> captureBuffer;   // received samples (one ear; we use L only)

    void renderChirp (double sampleRate);
    void startPing();
    void finishPing (int measuredLag);
};
} // namespace duskstudio
