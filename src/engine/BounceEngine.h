#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <atomic>
#include <functional>

namespace duskstudio
{
class AudioEngine;
class Session;

// Offline bounce. Detaches the engine from AudioDeviceManager and drives
// audioDeviceIOCallbackWithContext in a tight loop on its own worker
// thread. Real-time playback pauses for the duration. Render-in-place
// is still a follow-up.
class BounceEngine final : private juce::Thread
{
public:
    // MasterMix   : full track / aux / master path. Length = longest
    //               region end + tail.
    // Stems       : one WAV per track with content or armed. Each
    //               renders through the full chain with every other
    //               track soloed-off so plugin / bus-comp / mastering
    //               processing matches the master mix exactly. Files
    //               written as `<base>_<NN>_<sanitized-name>.wav`
    //               next to base. Original solo state restored on
    //               finish / cancel / error.
    // MasteringChain : MasteringPlayer -> MasteringChain. Length =
    //               player's source file + tail. Engine stage forced
    //               to Mastering for the render.
    enum class Mode { MasterMix, Stems, MasteringChain };

    // Output container. Wav = stereo 24-bit (always available). Mp3 = CBR via
    // libmp3lame; only usable when the build found LAME (else start() fails with
    // a clear error). The caller picks the matching file extension.
    enum class Format { Wav, Mp3 };

    // lastError value set when a render is cancelled. Shared so the dialog can
    // tell a user cancel from a real failure without a string literal that
    // silently drifts out of sync with run().
    static constexpr const char* kCancelledError = "Cancelled";

    // <dir>/<base>_<NN>_<safe>.wav. Empty / default ("5") track names
    // fall back to "track" so the user doesn't open 24 files called
    // mix_05_5.wav. Inline so the unit test links without dragging in
    // AudioEngine + Session + the bounce loop.
    static juce::File stemOutputFile (const juce::File& base,
                                        int trackIndexZeroBased,
                                        const juce::String& trackName)
    {
        auto dir  = base.getParentDirectory();
        auto stem = base.getFileNameWithoutExtension();
        if (stem.isEmpty()) stem = "bounce";

        auto safeName = trackName.trim();
        if (safeName.isEmpty() || safeName == juce::String (trackIndexZeroBased + 1))
            safeName = "track";

        safeName = juce::File::createLegalFileName (safeName);
        if (safeName.isEmpty()) safeName = "track";

        // Stems are always WAV regardless of the base file's extension: start()
        // forces Format::Wav for Stems mode (MP3 encoder delay + frame padding
        // would shift each stem and break the sample-accurate alignment a
        // re-import needs).
        const auto idx = juce::String (trackIndexZeroBased + 1).paddedLeft ('0', 2);
        return dir.getChildFile (stem + "_" + idx + "_" + safeName + ".wav");
    }

    BounceEngine (AudioEngine& engine, Session& session,
                   juce::AudioDeviceManager& deviceManager) noexcept;
    ~BounceEngine() override;

    // False if a render is already in flight. sampleRate <= 0 uses
    // engine's current rate. blockSize 1024 amortises overhead vs
    // realtime. tail = silence appended so reverb/comp tails decay.
    bool start (const juce::File& outputFile,
                double sampleRate = 0.0,
                int blockSize = 1024,
                double tailSeconds = 5.0,
                Mode mode = Mode::MasterMix,
                Format format = Format::Wav,
                int mp3BitrateKbps = 320);

    void cancel() noexcept { cancelRequested.store (true, std::memory_order_relaxed); }

    bool         isRendering() const noexcept { return rendering.load (std::memory_order_relaxed); }
    float        getProgress() const noexcept { return progress.load (std::memory_order_relaxed); }
    // Stems mode: 1-based current stem (1..total). 0 before first stem
    // or in Master / Mastering modes.
    int          getCurrentStemIndex() const noexcept
        { return currentStemIndex.load (std::memory_order_relaxed); }
    int          getTotalStemsToRender() const noexcept
        { return totalStemsToRender.load (std::memory_order_relaxed); }
    juce::String getLastError() const
    {
        // juce::String is refcounted; concurrent copy races the refcount.
        const juce::ScopedLock lock (lastErrorLock);
        return lastError;
    }
    juce::int64  getRenderedSamples() const noexcept { return renderedSamples.load (std::memory_order_relaxed); }

    // Called on the worker thread. Use MessageManager::callAsync for UI.
    std::function<void()>                  onStarted;
    std::function<void(float)>             onProgressUpdated;
    std::function<void(bool, juce::String)> onFinished;

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
    Format       renderFormat     = Format::Wav;
    int          renderBitrateKbps = 320;

    std::atomic<bool>  rendering        { false };
    std::atomic<bool>  cancelRequested  { false };
    std::atomic<float> progress         { 0.0f };
    std::atomic<juce::int64> renderedSamples { 0 };
    std::atomic<int>   currentStemIndex   { 0 };
    std::atomic<int>   totalStemsToRender { 0 };
    juce::String lastError;
    juce::CriticalSection lastErrorLock;

    juce::int64 computeBounceLength (double sampleRate, double tail) const;

    // Create the WAV or MP3 writer for the chosen format over an already-opened
    // (truncated) output stream. Returns null + sets errOut on failure. Takes
    // ownership of outStream on success; on failure outStream is freed.
    std::unique_ptr<juce::AudioFormatWriter>
        makeWriter (std::unique_ptr<juce::FileOutputStream> outStream, juce::String& errOut) const;

    // Always restores original solo state before returning.
    bool runStemsMode();
    // stemFractionStart + stemFractionWidth slice the progress bar so
    // the UI sees monotonic 0..1 across all stems.
    bool renderOneStem (const juce::File& outFile,
                          juce::int64 lenSamples,
                          float stemFractionStart,
                          float stemFractionWidth);
};
} // namespace duskstudio
