#pragma once

#include <juce_core/juce_core.h>
#include <atomic>
#include <cstdint>
#include <memory>
#include "audiofile/BufferedFileReader.h"
#include "MasteringTransport.h"
#include "../foundation/WindowedSincInterpolator.h"
#include "../foundation/PlanarBuffer.h"

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
    ~MasteringPlayer();

    // Message thread. deviceSampleRate drives the resampler: a source whose
    // rate differs from the device is band-limited in process() so
    // it plays at the right speed and pitch (the playhead stays in SOURCE
    // samples - the UI's waveform and seek math use the source rate).
    void prepare (int maxBlockSize, double deviceSampleRate);
    bool loadFile (const juce::File& file);
    void unloadFile();

    // Render owner only, with the device callback detached. Takes filling from
    // the background worker and verifies every source span before process().
    bool primeOfflineBlock (int numSamples);
    void finishOfflineRender();

    bool        isLoaded() const noexcept    { return ownedReader != nullptr; }
    juce::File  getLoadedFile() const         { return loadedFile; }
    std::int64_t getLengthSamples() const noexcept;
    double      getSourceSampleRate() const noexcept;
    // Changes on every load and unload, a reload of the same file included,
    // so a view can follow the source by polling. Message thread.
    std::uint64_t getSourceGeneration() const noexcept { return sourceGeneration; }

    // Transport.
    void play() noexcept
    {
        ControlChangeScope change (*this);
        transport.play();
    }
    void stop() noexcept
    {
        ControlChangeScope change (*this);
        transport.stop();
    }
    bool isPlaying() const noexcept { return transport.isPlaying(); }

    std::int64_t getPlayhead() const noexcept { return transport.getPlayhead(); }
    void setPlayhead (std::int64_t p) noexcept
    {
        ControlChangeScope change (*this);
        const auto seek = transport.beginSeek (p);
        transport.finishSeek (seek);
    }

    struct BlockInfo
    {
        std::int64_t sourceStart = 0, sourceEnd = 0;
        std::uint64_t revision = 0;
        bool playing = false;
        // False includes a prefetched prefix followed by an unavailable tail.
        bool completeSourceRead = true;
        // False when a control or source change overlapped this block.
        bool stable = false;
    };

    // Audio thread. Writes `numSamples` of stereo audio to L/R. Both must
    // be valid pointers. Output is silence when not playing or when the
    // playhead is past the file length.
    BlockInfo process (float* L, float* R, int numSamples) noexcept;

private:
    // This validates analysis metadata, not ownership of reader mutations.
    // The active count covers nested load/unload and overlapping controls.
    std::atomic<unsigned> controlsInFlight { 0 };
    std::atomic<std::uint64_t> controlRevision { 0 };
    struct ControlChangeScope
    {
        MasteringPlayer& owner;
        explicit ControlChangeScope (MasteringPlayer& player) noexcept : owner (player)
        {
            owner.controlsInFlight.fetch_add (1, std::memory_order_seq_cst);
            owner.controlRevision.fetch_add (1, std::memory_order_seq_cst);
        }
        ~ControlChangeScope() noexcept
        {
            owner.controlRevision.fetch_add (1, std::memory_order_seq_cst);
            owner.controlsInFlight.fetch_sub (1, std::memory_order_seq_cst);
        }
    };

    // Owning pointers - mutated only on the message thread. The audio thread
    // reads via the `currentReader` atomic below. An unpublished reader is
    // destroyed, closing its file, as soon as parkAndWaitForAudio proves no
    // block can still hold it. `retiredReader` is non-null only when that
    // drain timed out; the next successful drain or the destructor frees it.
    // loadFile publishes only after a successful drain has cleared it, so it
    // never holds more than the one reader that was unpublished last.
    std::unique_ptr<dusk::audio::BufferedFileReader> ownedReader;
    std::unique_ptr<dusk::audio::BufferedFileReader> retiredReader;

    // Audio-thread-safe view of `ownedReader.get()`. The audio thread loads
    // this once per block and uses the resulting pointer for the rest of the
    // block. Publishing stores are release; the park that unpublishes is
    // seq_cst (see audioInFlight).
    std::atomic<dusk::audio::BufferedFileReader*> currentReader { nullptr };

    juce::File loadedFile;
    std::uint64_t sourceGeneration = 0;   // message thread only

    dusk::audio::PlanarBuffer readScratch;   // 2 ch x maxBlockSize, pre-allocated

    // Resampling state. speedRatio = sourceRate / deviceRate; 1 (within
    // epsilon) takes the exact-copy path. inScratch is sized on the message
    // thread (prepare/loadFile) for the worst per-block source need; the
    // audio thread never allocates. The centered kernel reads ahead without
    // advancing the source-domain playhead. Explicit seeks reset fractional
    // phase, including a seek to the current integer playhead.
    //
    // Mutation protocol: readScratch / inScratch / the resampler are
    // touched by process() between its audioInFlight bump and scope exit.
    // The message thread parks currentReader on null and drains
    // audioInFlight to zero (parkAndWaitForAudio) BEFORE resizing or
    // resetting any of them - a ratio publish alone wouldn't stop an
    // in-flight block from racing the resize.
    std::atomic<double> speedRatio { 1.0 };
    dusk::audio::PlanarBuffer inScratch;
    dusk::audio::WindowedSincInterpolator resampler;
    std::int64_t resampleReadPos = -1;   // audio thread only
    double resampleFraction = 0.0;
    std::int64_t resampleFramesRemaining = 0;
    std::uint32_t resampleSeekRevision = 0;
    bool resampleReady = false;
    int    preparedBlockSize   = 0;    // message thread only
    double preparedDeviceRate  = 0.0;  // message thread only

    // Dekker handshake with parkAndWaitForAudio: process() increments this
    // and then loads currentReader; the park stores null to currentReader and
    // then loads this. All four are seq_cst, so either the drain sees the
    // increment and waits for the matching release decrement, or the block's
    // load sees null. Acquire/release alone lets both loads miss the other
    // side's write (store buffering): the drain reads 0 while the block
    // latches the reader that is about to be destroyed.
    std::atomic<int> audioInFlight { 0 };
    struct AudioInFlightScope
    {
        std::atomic<int>& c;
        AudioInFlightScope (std::atomic<int>& a) noexcept : c (a)
            { c.fetch_add (1, std::memory_order_seq_cst); }
        ~AudioInFlightScope() noexcept
            { c.fetch_sub (1, std::memory_order_release); }
    };

    // Message thread: park the audio thread on a null reader and wait for
    // any in-flight process() to leave. False when the drain timed out -
    // the caller must then leave the shared state untouched.
    bool parkAndWaitForAudio();

    bool updateResampleState();         // message thread, only after a
                                        // successful parkAndWaitForAudio

    MasteringTransport transport;
};
} // namespace duskstudio
