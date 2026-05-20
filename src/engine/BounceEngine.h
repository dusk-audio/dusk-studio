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
// place). MVP implements Master mix only - capturing post-master-fader
// stereo output. Stems and render-in-place are follow-ups.
class BounceEngine final : private juce::Thread
{
public:
    // What signal flow to capture.
    //   MasterMix   - the live track / aux / master path. Length comes
    //                 from the longest region on any track + tail.
    //   MasteringChain - the loaded mastering player → MasteringChain
    //                 path. Length comes from the player's source file
    //                 length + tail. Engine stage is set to Mastering
    //                 for the duration of the render.
    enum class Mode { MasterMix, MasteringChain };

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
    juce::String lastError;
    juce::CriticalSection lastErrorLock;

    juce::int64 computeBounceLength (double sampleRate, double tail) const;
};
} // namespace duskstudio
