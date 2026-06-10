#include "MasteringPlayer.h"
#include <cstring>

namespace duskstudio
{
MasteringPlayer::MasteringPlayer()
{
    formatManager.registerBasicFormats();
    bufferingThread.startThread();
}

MasteringPlayer::~MasteringPlayer()
{
    // Readers detach from bufferingThread on destruction; drop them
    // before joining the thread.
    currentReader.store (nullptr, std::memory_order_release);
    previousReader.reset();
    ownedReader.reset();
    bufferingThread.stopThread (2000);
}

void MasteringPlayer::prepare (int maxBlockSize)
{
    readScratch.setSize (2, juce::jmax (1, maxBlockSize),
                          /*keepExistingContent*/ false,
                          /*clearExtraSpace*/      false,
                          /*avoidReallocating*/    false);
}

bool MasteringPlayer::loadFile (const juce::File& file)
{
    unloadFile();
    if (! file.existsAsFile()) return false;

    std::unique_ptr<juce::AudioFormatReader> raw (formatManager.createReaderFor (file));
    if (raw == nullptr) return false;

    // Wrap in a BufferingAudioReader so process() reads from prefetched
    // memory. 96000 samples ≈ 1-2 s of lead; timeout 0 keeps the audio
    // thread non-blocking — a prefetch miss (right after a seek) returns
    // silence until the prefetch catches up. Same pattern as
    // PlaybackEngine.
    constexpr int kSamplesToBuffer = 96000;
    auto r = std::make_unique<juce::BufferingAudioReader> (
        raw.release(), bufferingThread, kSamplesToBuffer);
    r->setReadTimeout (0);

    // Stop playback before swapping the reader. The audio thread reads
    // `playing` first and bails before touching the reader pointer; this
    // store, combined with the release-store of currentReader below, gives
    // the audio thread a consistent view (either old reader + not playing,
    // or new reader + not playing).
    playing.store (false, std::memory_order_relaxed);

    // Park audio thread on null so any in-flight callback bails before we
    // move the previous owner out from under it.
    currentReader.store (nullptr, std::memory_order_release);

    // Move the (now-untouched-by-audio) prior owner into previousReader so
    // its destructor doesn't run until the NEXT loadFile/unloadFile/dtor.
    // That delay covers the audio thread's worst case: it observed the
    // null-store and dropped the old pointer for this block; by the next
    // mutation, at least one block has elapsed.
    previousReader = std::move (ownedReader);
    ownedReader    = std::move (r);
    loadedFile     = file;
    playhead.store (0, std::memory_order_relaxed);
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

juce::int64 MasteringPlayer::getLengthSamples() const noexcept
{
    return ownedReader ? ownedReader->lengthInSamples : 0;
}

double MasteringPlayer::getSourceSampleRate() const noexcept
{
    return ownedReader ? ownedReader->sampleRate : 0.0;
}

void MasteringPlayer::process (float* L, float* R, int numSamples) noexcept
{
    if (L == nullptr || R == nullptr) return;
    std::memset (L, 0, sizeof (float) * (size_t) numSamples);
    std::memset (R, 0, sizeof (float) * (size_t) numSamples);

    if (! playing.load (std::memory_order_relaxed)) return;

    // Acquire-load the reader pointer once and use it for the whole block.
    // Pairs with the release-stores in loadFile/unloadFile.
    auto* r = currentReader.load (std::memory_order_acquire);
    if (r == nullptr) return;

    const juce::int64 start = playhead.load (std::memory_order_relaxed);
    if (start < 0) return;
    if (start >= r->lengthInSamples)
    {
        // Past EOF - auto-stop so the UI can flip the Play button back.
        playing.store (false, std::memory_order_relaxed);
        return;
    }

    const int  available = (int) juce::jmin ((juce::int64) numSamples,
                                              r->lengthInSamples - start);
    if (available > readScratch.getNumSamples()) return;  // shouldn't happen

    // Read into our 2-ch scratch. AudioFormatReader::read with two non-null
    // destination pointers fills both; for mono sources it duplicates the
    // single channel into both, which is exactly what we want.
    r->read (&readScratch, 0, available, start,
              /*useLeftChan*/  true,
              /*useRightChan*/ true);

    std::memcpy (L, readScratch.getReadPointer (0), sizeof (float) * (size_t) available);
    std::memcpy (R, readScratch.getReadPointer (1), sizeof (float) * (size_t) available);

    playhead.fetch_add (available, std::memory_order_relaxed);

    // If we just hit EOF mid-block, auto-stop.
    if (start + available >= r->lengthInSamples)
        playing.store (false, std::memory_order_relaxed);
}
} // namespace duskstudio
