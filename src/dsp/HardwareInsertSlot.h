#pragma once

#include "../foundation/IntDelayLine.h"
#include "../foundation/SmoothedValue.h"

#include <atomic>
#include <vector>

namespace duskstudio
{
// HardwareInsertParams / Routing live in Session.h. Forward declared
// here so the header doesn't pull in Session.h.
struct HardwareInsertParams;
struct HardwareInsertRouting;

// Sends to a pair of physical outputs, captures from a pair of inputs,
// optionally re-encodes Mid/Side, mixes against a delay-aligned dry
// copy. Slot owns the dry-path delay line. Strip reports
// getLatencySamples to the PDC aggregator so the rest of the session
// is delayed to keep the track aligned with the timeline.
class HardwareInsertSlot
{
public:
    HardwareInsertSlot();
    ~HardwareInsertSlot();

    HardwareInsertSlot (const HardwareInsertSlot&) = delete;
    HardwareInsertSlot& operator= (const HardwareInsertSlot&) = delete;
    HardwareInsertSlot (HardwareInsertSlot&&) = delete;
    HardwareInsertSlot& operator= (HardwareInsertSlot&&) = delete;

    // Idempotent.
    void prepare (double sampleRate, int blockSize);

    // Message thread, audio STOPPED. paramsRef is a plain pointer (not
    // atomic) - binding while audio runs is a data race. Caller keeps
    // `params` alive for the duration of audio processing.
    void bind (const HardwareInsertParams& params) noexcept;

    // In-place. Out-of-range routing falls through to dry-only so a
    // stale config never crashes the audio thread.
    void processStereoBlock (float* L, float* R, int numSamples,
                              const float* const* deviceInputs,
                              int numDeviceInputs,
                              float* const*       deviceOutputs,
                              int numDeviceOutputs) noexcept;

    // Cached at the top of processStereoBlock so the engine sees one
    // consistent value per callback even if the user moves the slider
    // mid-playback.
    int getLatencySamples() const noexcept
    {
        return cachedLatencySamples.load (std::memory_order_relaxed);
    }

    // Called by the strip's mode-flip crossfade gate when the insert
    // is being swapped. Drops dry-path delay history.
    void resetTailsAndDelayLine() noexcept;

    // True while the UI has a ping in flight (pingPending set, result
    // not yet delivered). Audio thread only - lets the strip bypass its
    // not-passing skip so the slot gets serviced for the measurement.
    bool isPingRequested() const noexcept;

    // ~340 ms at 48 kHz - ample for any realistic outboard round-trip.
    static constexpr int kMaxDelaySamples = 16384;

    // Ping calibration. Chirp = 100 ms (capped 9600 samples). Capture
    // runs concurrently with the chirp and is sized in prepare() to
    // chirpLength + kMaxDelaySamples, so the correlator's lag search
    // covers the full latency-slider range at any sample rate.
    // Correlation runs ~256 candidate lags per block.
    static constexpr int kChirpMaxSamples   = 9600;
    static constexpr int kCorrelationsPerBlock = 256;

private:
    const HardwareInsertParams* paramsRef = nullptr;
    double prepSampleRate = 0.0;
    int    prepBlockSize  = 0;

    dusk::audio::IntDelayLine dryDelayL;
    dusk::audio::IntDelayLine dryDelayR;

    dusk::audio::SmoothedValue<float> outGainLin;
    dusk::audio::SmoothedValue<float> inGainLin;
    dusk::audio::SmoothedValue<float> dryWetSmooth;

    std::atomic<int> cachedLatencySamples { 0 };

    // Audio-thread-only state. UI flips paramsRef->pingPending to start;
    // when result lands, paramsRef->pingResult holds measured lag and
    // pingPending is cleared.
    enum class PingState : int { Idle = 0, Playing = 1, Capturing = 2, Correlating = 3 };

    PingState pingState        = PingState::Idle;
    int       chirpLength      = 0;
    int       pingPlayPos      = 0;
    int       pingCapturePos   = 0;
    int       pingCorrelateK   = 0;
    float     pingBestPeak     = 0.0f;
    int       pingBestK        = -1;
    float     pingAutoPeak     = 0.0f;   // chirp's auto-correlation peak for threshold

    // If user clears the input mid-ping or hot-swaps the device,
    // inValid stays false forever and pingCapturePos never advances.
    // Bail after this many samples so the UI doesn't hang on "Pinging...".
    int       pingCaptureStallSamples = 0;
    static constexpr int kPingCaptureStallMax = 2 * (kChirpMaxSamples + kMaxDelaySamples);

    std::vector<float> chirpBuffer;
    std::vector<float> captureBuffer;   // one ear (L only)

    void renderChirp (double sampleRate);
    void startPing();
    void finishPing (int measuredLag);
};
} // namespace duskstudio
