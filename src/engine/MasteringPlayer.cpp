#include "MasteringPlayer.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>

namespace duskstudio
{
MasteringPlayer::~MasteringPlayer()
{
    currentReader.store (nullptr, std::memory_order_release);
    previousReader.reset();
    ownedReader.reset();
}

bool MasteringPlayer::parkAndWaitForAudio()
{
    currentReader.store (nullptr, std::memory_order_release);
    // process() bumps audioInFlight BEFORE loading currentReader, so once
    // the counter reaches zero no callback can be touching the scratch or
    // interpolators (new entries see null and bail). Happy path is sub-ms;
    // the deadline only fires on a stuck/detached audio thread - stale
    // resample state then beats a data race.
    constexpr auto kDrainTimeout = std::chrono::milliseconds (200);
    const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
    while (audioInFlight.load (std::memory_order_acquire) > 0)
    {
        if (std::chrono::steady_clock::now() > deadline)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/MasteringPlayer] parkAndWaitForAudio: audioInFlight=%d "
                          "after %lld ms; leaving resample state untouched.\n",
                          audioInFlight.load (std::memory_order_relaxed),
                          (long long) kDrainTimeout.count());
            return false;
        }
        std::this_thread::yield();
    }
    return true;
}

void MasteringPlayer::prepare (int maxBlockSize, double deviceSampleRate)
{
    const bool drained = parkAndWaitForAudio();
    if (drained)
        readScratch.setSize (2, std::max (1, maxBlockSize),
                              /*keepExistingContent*/ false,
                              /*clearExtraSpace*/      false,
                              /*avoidReallocating*/    false);
    preparedBlockSize  = std::max (1, maxBlockSize);
    preparedDeviceRate = deviceSampleRate;
    if (drained)
        updateResampleState();
    // Re-publish the reader the park cleared (no-op when none is loaded).
    currentReader.store (ownedReader.get(), std::memory_order_release);
}

void MasteringPlayer::updateResampleState()
{
    const double srcRate = ownedReader != nullptr ? ownedReader->info().sampleRate : 0.0;
    const double ratio   = (srcRate > 0.0 && preparedDeviceRate > 0.0)
                               ? srcRate / preparedDeviceRate : 1.0;
    // Worst-case source samples one output block can consume, plus the
    // interpolator's history margin.
    const int inNeeded = (int) std::ceil ((double) preparedBlockSize
                                           * std::max (1.0, ratio)) + 8;
    inScratch.setSize (2, std::max (1, inNeeded), false, false, false);
    interpL.reset();
    interpR.reset();
    speedRatio.store (ratio, std::memory_order_release);
}

bool MasteringPlayer::loadFile (const juce::File& file)
{
    unloadFile();
    if (! file.existsAsFile()) return false;

    auto raw = dusk::audio::FileReader::open (
        std::filesystem::u8path (file.getFullPathName().toStdString()));
    if (raw == nullptr) return false;

    // Buffered so process() reads from prefetched memory: the default window
    // is 1-2 s of lead, and a miss (right after a seek) returns silence until
    // the prefetch catches up. Same pattern as PlaybackEngine.
    auto r = std::make_unique<dusk::audio::BufferedFileReader> (std::move (raw));

    // Stop playback before swapping the reader. The audio thread reads
    // `playing` first and bails before touching the reader pointer; this
    // store, combined with the release-store of currentReader below, gives
    // the audio thread a consistent view (either old reader + not playing,
    // or new reader + not playing).
    playing.store (false, std::memory_order_relaxed);

    // Park the audio thread on null AND drain any in-flight callback: the
    // scratch resize + interpolator resets below race a block that latched
    // the old reader before the park. On a drain timeout refuse the load -
    // stale state beats a data race.
    if (! parkAndWaitForAudio())
    {
        currentReader.store (ownedReader.get(), std::memory_order_release);
        return false;
    }

    // Move the (now-untouched-by-audio) prior owner into previousReader so
    // its destructor doesn't run until the NEXT loadFile/unloadFile/dtor.
    previousReader = std::move (ownedReader);
    ownedReader    = std::move (r);
    loadedFile     = file;
    playhead.store (0, std::memory_order_relaxed);
    // Size the resample scratch for this source's rate BEFORE the reader is
    // published - the audio thread is parked on null until the store below.
    updateResampleState();
    currentReader.store (ownedReader.get(), std::memory_order_release);
    return true;
}

void MasteringPlayer::unloadFile()
{
    playing.store (false, std::memory_order_relaxed);
    currentReader.store (nullptr, std::memory_order_release);
    previousReader = std::move (ownedReader);  // delays destruction by one publish
    loadedFile = juce::File();
    playhead.store (0, std::memory_order_relaxed);
}

std::int64_t MasteringPlayer::getLengthSamples() const noexcept
{
    return ownedReader ? ownedReader->info().numFrames : 0;
}

double MasteringPlayer::getSourceSampleRate() const noexcept
{
    return ownedReader ? ownedReader->info().sampleRate : 0.0;
}

void MasteringPlayer::process (float* L, float* R, int numSamples) noexcept
{
    if (L == nullptr || R == nullptr) return;
    std::memset (L, 0, sizeof (float) * (size_t) numSamples);
    std::memset (R, 0, sizeof (float) * (size_t) numSamples);

    if (! playing.load (std::memory_order_relaxed)) return;

    // Bump BEFORE loading currentReader: parkAndWaitForAudio clears the
    // pointer then drains this counter, so a block that got past the null
    // check is waited on before the scratch/interpolators are mutated.
    AudioInFlightScope guard (audioInFlight);

    // Acquire-load the reader pointer once and use it for the whole block.
    // Pairs with the release-stores in loadFile/unloadFile.
    auto* r = currentReader.load (std::memory_order_acquire);
    if (r == nullptr) return;

    const std::int64_t start  = playhead.load (std::memory_order_relaxed);
    const std::int64_t length = r->info().numFrames;
    const bool         mono   = r->info().numChannels < 2;
    if (start < 0) return;
    if (start >= length)
    {
        // Past EOF - auto-stop so the UI can flip the Play button back.
        playing.store (false, std::memory_order_relaxed);
        return;
    }

    const double ratio = speedRatio.load (std::memory_order_acquire);
    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        const int  available = (int) std::min ((std::int64_t) numSamples,
                                                  (std::int64_t) (length - start));
        if (available > readScratch.getNumSamples()) return;  // shouldn't happen

        // The reader zero-fills destination channels the file doesn't have, so
        // a mono source is duplicated here to keep the stage stereo.
        float* dest[2] = { readScratch.getWritePointer (0), readScratch.getWritePointer (1) };
        r->readRt (dest, 2, start, available);
        if (mono)
            std::memcpy (dest[1], dest[0], sizeof (float) * (size_t) available);

        std::memcpy (L, readScratch.getReadPointer (0), sizeof (float) * (size_t) available);
        std::memcpy (R, readScratch.getReadPointer (1), sizeof (float) * (size_t) available);

        playhead.fetch_add (available, std::memory_order_relaxed);

        // If we just hit EOF mid-block, auto-stop.
        if (start + available >= length)
            playing.store (false, std::memory_order_relaxed);
        return;
    }

    // Source rate ≠ device rate: Lagrange-resample so the file plays at the
    // right speed and pitch. The playhead advances by SOURCE samples
    // consumed, keeping every UI position/seek in the source domain.
    const int inNeeded = (int) std::ceil ((double) numSamples * ratio) + 8;
    if (inNeeded > inScratch.getNumSamples()) return;  // prepare/load sizes this

    // A seek (or first block after load) breaks read continuity - drop the
    // interpolators' history so the jump doesn't smear.
    if (start != resampleReadPos)
    {
        interpL.reset();
        interpR.reset();
    }

    // Read past-EOF input as silence so the interpolator can flush the tail.
    const int inAvailable = (int) std::min ((std::int64_t) inNeeded,
                                               (std::int64_t) (length - start));
    inScratch.clear();
    if (inAvailable > 0)
    {
        float* dest[2] = { inScratch.getWritePointer (0), inScratch.getWritePointer (1) };
        r->readRt (dest, 2, start, inAvailable);
        if (mono)
            std::memcpy (dest[1], dest[0], sizeof (float) * (size_t) inAvailable);
    }

    const int usedL = interpL.process (ratio, inScratch.getReadPointer (0), L, numSamples);
    const int usedR = interpR.process (ratio, inScratch.getReadPointer (1), R, numSamples);
    juce::ignoreUnused (usedR);   // both consume identically for equal ratios

    const std::int64_t consumed = std::min ((std::int64_t) usedL,
                                              (std::int64_t) (length - start));
    playhead.fetch_add (consumed, std::memory_order_relaxed);
    resampleReadPos = start + consumed;

    if (start + consumed >= length)
        playing.store (false, std::memory_order_relaxed);
}
} // namespace duskstudio
