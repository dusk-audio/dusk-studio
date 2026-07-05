#pragma once

#include <juce_audio_basics/juce_audio_basics.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include <juce_core/juce_core.h>
#include <atomic>
#include <memory>

namespace duskstudio
{
// Stereo file player used by the Mastering stage. Loads a WAV/AIFF on the
// message thread; on the audio thread, fills a stereo block from the file
// at the current playhead and advances. EOF returns silence (the player
// stops itself so the UI can re-enable Play). Mono files are duplicated
// to L+R; >2-channel files use the first two channels only.
//
// This is a deliberate sibling of PlaybackEngine rather than a reuse -
// PlaybackEngine is per-track mono with multi-region playback, and
// retrofitting it for stereo single-source playback would muddy the
// per-track contract.
class MasteringPlayer
{
public:
    MasteringPlayer();
    ~MasteringPlayer();

    // Message thread. deviceSampleRate drives the resampler: a source whose
    // rate differs from the device is Lagrange-interpolated in process() so
    // it plays at the right speed and pitch (the playhead stays in SOURCE
    // samples — the UI's waveform and seek math use the source rate).
    void prepare (int maxBlockSize, double deviceSampleRate);
    bool loadFile (const juce::File& file);
    void unloadFile();

    bool        isLoaded() const noexcept    { return ownedReader != nullptr; }
    juce::File  getLoadedFile() const         { return loadedFile; }
    juce::int64 getLengthSamples() const noexcept;
    double      getSourceSampleRate() const noexcept;

    // Transport.
    void play() noexcept    { playing.store (true, std::memory_order_relaxed); }
    void stop() noexcept    { playing.store (false, std::memory_order_relaxed); }
    bool isPlaying() const noexcept { return playing.load (std::memory_order_relaxed); }

    juce::int64 getPlayhead() const noexcept     { return playhead.load (std::memory_order_relaxed); }
    void setPlayhead (juce::int64 p) noexcept    { playhead.store (p, std::memory_order_relaxed); }

    // Audio thread. Writes `numSamples` of stereo audio to L/R. Both must
    // be valid pointers. Output is silence when not playing or when the
    // playhead is past the file length.
    void process (float* L, float* R, int numSamples) noexcept;

private:
    juce::AudioFormatManager formatManager;

    // Declared before the readers so it outlives them (destruction is
    // reverse declaration order). Prefetches for the BufferingAudioReader
    // wrapper so process() never does synchronous disk I/O — a cold file
    // or a seek after setPlayhead would otherwise stall the callback.
    juce::TimeSliceThread bufferingThread { "Dusk Studio mastering prefetch" };

    // Owning pointer - mutated only on the message thread (loadFile /
    // unloadFile). The audio thread reads via the `currentReader` atomic
    // below (PluginSlot pattern). `previousReader` keeps the prior owned
    // reader alive across one publish so the audio thread can safely finish
    // its current block with the old pointer; the next publish drops it.
    std::unique_ptr<juce::AudioFormatReader> ownedReader;
    std::unique_ptr<juce::AudioFormatReader> previousReader;

    // Audio-thread-safe view of `ownedReader.get()`. The audio thread
    // acquire-loads this once per block and uses the resulting pointer for
    // the rest of the block. Message-thread writes are release-stores.
    std::atomic<juce::AudioFormatReader*> currentReader { nullptr };

    juce::File loadedFile;

    juce::AudioBuffer<float> readScratch;  // 2 ch × maxBlockSize, pre-allocated

    // Resampling state. speedRatio = sourceRate / deviceRate; 1 (within
    // epsilon) takes the exact-copy path. inScratch is sized on the message
    // thread (prepare/loadFile) for the worst per-block source need; the
    // audio thread never allocates. resampleReadPos tracks read continuity so
    // an external seek (setPlayhead) resets the interpolator history instead
    // of smearing across the jump.
    std::atomic<double> speedRatio { 1.0 };
    juce::AudioBuffer<float>  inScratch;
    juce::LagrangeInterpolator interpL, interpR;
    juce::int64 resampleReadPos = -1;   // audio thread only
    int    preparedBlockSize   = 0;
    double preparedDeviceRate  = 0.0;

    void updateResampleState();         // message thread

    std::atomic<bool>        playing  { false };
    std::atomic<juce::int64> playhead { 0 };
};
} // namespace duskstudio
