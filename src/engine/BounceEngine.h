#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <functional>

namespace duskstudio
{
class AudioEngine;
class Session;

// Offline master-mix bounce. Runs the engine in non-realtime mode by
// detaching it from the AudioDeviceManager and driving its audio callback
// in a tight loop, capturing the master output to a WAV file. Realtime
// playback is paused for the duration; the engine is re-attached when the
// render completes (or is cancelled).
//
// Threading: BounceEngine creates and owns a worker thread (juce::Thread)
// that runs the actual render loop. The message thread can poll progress
// and request cancellation; the engine sits unused until render completes.
//
// Phase 3 spec calls out three modes (Master mix / Stems / Render in
// place). MVP implements Master mix + Stems; render-in-place is still
// a follow-up.
class BounceEngine final : private juce::Thread
{
public:
    // What signal flow to capture.
    //   MasterMix   - the live track / aux / master path. Length comes
    //                 from the longest region on any track + tail.
    //   Stems       - one WAV per track that has content or is armed;
    //                 each rendered through the FULL track→aux→master
    //                 chain with every other track soloed-off so plugin /
    //                 bus-comp / mastering processing matches the master
    //                 mix exactly. Per-track files written next to the
    //                 chosen output as `<base>_<NN>_<sanitized-name>.wav`.
    //                 Original track solo state is snapshotted and
    //                 restored on finish / cancel / error.
    //   MasteringChain - the loaded mastering player → MasteringChain
    //                 path. Length comes from the player's source file
    //                 length + tail. Engine stage is set to Mastering
    //                 for the duration of the render.
    enum class Mode { MasterMix, Stems, MasteringChain };

    // Builds the per-stem output path: `<dir>/<base>_<NN>_<safe>.wav`,
    // where NN is the 1-based track index zero-padded to two digits and
    // `safe` is the track name with characters illegal for the host
    // filesystem replaced. Empty / default names fall back to `track`.
    // Defined inline so the unit test can link against the helper
    // without pulling in AudioEngine + Session + the full bounce loop.
    static juce::File stemOutputFile (const juce::File& base,
                                        int trackIndexZeroBased,
                                        const juce::String& trackName)
    {
        auto dir  = base.getParentDirectory();
        auto stem = base.getFileNameWithoutExtension();
        if (stem.isEmpty()) stem = "bounce";

        auto safeName = trackName.trim();
        // Default track names are the 1-based index as a string. Treat
        // that (or empty) as no-name and fall back to a generic stem
        // suffix so the user doesn't open 24 files called
        // `mix_05_5.wav`.
        if (safeName.isEmpty() || safeName == juce::String (trackIndexZeroBased + 1))
            safeName = "track";

        safeName = juce::File::createLegalFileName (safeName);
        if (safeName.isEmpty()) safeName = "track";

        const auto idx = juce::String (trackIndexZeroBased + 1).paddedLeft ('0', 2);
        return dir.getChildFile (stem + "_" + idx + "_" + safeName + ".wav");
    }

    BounceEngine (AudioEngine& engine, Session& session,
                   juce::AudioDeviceManager& deviceManager) noexcept;
    ~BounceEngine() override;

    // Configure + start a render. Returns false immediately if a render is
    // already in flight. The render runs on a background thread; poll
    // isRendering() / getProgress() / getLastError() on the message thread.
    //
    // outputFile  - destination WAV. Will be overwritten if it exists.
    // sampleRate  - render rate. Pass <= 0 to use the engine's current rate.
    // blockSize   - render block size. 1024 is a good default - bigger
    //               than realtime to amortise overhead.
    // tailSeconds - extra silence appended after the last region's end so
    //               reverb/comp/EQ tails decay naturally. Default 5 s.
    // mode        - which signal path to capture. Defaults to MasterMix.
    bool start (const juce::File& outputFile,
                double sampleRate = 0.0,
                int blockSize = 1024,
                double tailSeconds = 5.0,
                Mode mode = Mode::MasterMix);

    void cancel() noexcept { cancelRequested.store (true, std::memory_order_relaxed); }

    bool         isRendering() const noexcept { return rendering.load (std::memory_order_relaxed); }
    float        getProgress() const noexcept { return progress.load (std::memory_order_relaxed); }
    // Stems-mode only: 1-based index of the track currently being
    // rendered (1..numTracksToRender). 0 before the first stem starts.
    // Always 0 in Master / Mastering modes.
    int          getCurrentStemIndex() const noexcept
        { return currentStemIndex.load (std::memory_order_relaxed); }
    int          getTotalStemsToRender() const noexcept
        { return totalStemsToRender.load (std::memory_order_relaxed); }
    juce::String getLastError() const
    {
        // juce::String is reference-counted; copying it concurrently with the
        // worker's assignment in run() would race on the refcount. The lock
        // serialises the copy with run()'s writes.
        const juce::ScopedLock lock (lastErrorLock);
        return lastError;
    }
    juce::int64  getRenderedSamples() const noexcept { return renderedSamples.load (std::memory_order_relaxed); }

    // Optional callbacks for the UI. Both called on the worker thread -
    // marshal to the message thread (juce::MessageManager::callAsync) if
    // touching UI state from these.
    std::function<void()>                  onStarted;
    std::function<void(float)>             onProgressUpdated;  // 0..1
    std::function<void(bool, juce::String)> onFinished;        // (success, errorOrEmpty)

private:
    void run() override;

    AudioEngine& engine;
    Session&     session;
    juce::AudioDeviceManager& deviceManager;

    juce::File   outputFile;
    double       renderSampleRate = 0.0;
    int          renderBlockSize  = 1024;
    double       tailSeconds      = 5.0;
    juce::int64  totalSamples     = 0;
    Mode         renderMode       = Mode::MasterMix;

    std::atomic<bool>  rendering        { false };
    std::atomic<bool>  cancelRequested  { false };
    std::atomic<float> progress         { 0.0f };
    std::atomic<juce::int64> renderedSamples { 0 };
    std::atomic<int>   currentStemIndex   { 0 };
    std::atomic<int>   totalStemsToRender { 0 };
    juce::String lastError;
    juce::CriticalSection lastErrorLock;

    juce::int64 computeBounceLength (double sampleRate, double tail) const;

    // Stems entry point — owns its own writer + solo-snapshot. Returns
    // true on success, false on early failure (no tracks to render,
    // writer setup failed, or cancellation). Always restores the
    // original solo state before returning.
    bool runStemsMode();
    // Renders one stem to `outFile` driving the engine's audio callback
    // for the full bounce length. `stemFractionStart` and
    // `stemFractionWidth` slice the overall progress bar so the UI sees
    // monotonic 0..1 across all stems.
    bool renderOneStem (const juce::File& outFile,
                          juce::int64 lenSamples,
                          float stemFractionStart,
                          float stemFractionWidth);
};
} // namespace duskstudio
