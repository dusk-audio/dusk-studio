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
    // Parks currentReader on null and drains any in-flight process() so the
    // readers can't be destroyed under a callback that latched the pointer.
    parkAndWaitForAudio();
    retiredReader.reset();
    ownedReader.reset();
}

bool MasteringPlayer::parkAndWaitForAudio()
{
    currentReader.store (nullptr, std::memory_order_seq_cst);
    // process() bumps audioInFlight BEFORE loading currentReader, so once
    // the counter reaches zero no callback can be touching the reader, the
    // scratch or the kernel (new entries see null and bail). Happy path
    // is sub-ms; the deadline only fires on a stuck/detached audio thread -
    // stale resample state and a still-open file then beat a data race.
    constexpr auto kDrainTimeout = std::chrono::milliseconds (200);
    const auto deadline = std::chrono::steady_clock::now() + kDrainTimeout;
    while (audioInFlight.load (std::memory_order_seq_cst) > 0)
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
    ControlChangeScope change (*this);
    if (! parkAndWaitForAudio()) return;
    retiredReader.reset();
    preparedBlockSize = std::max (1, maxBlockSize);
    preparedDeviceRate = deviceSampleRate;
    resampleReady = updateResampleState();
    currentReader.store (resampleReady ? ownedReader.get() : nullptr, std::memory_order_release);
}

bool MasteringPlayer::updateResampleState()
{
    const double srcRate = ownedReader != nullptr ? ownedReader->info().sampleRate : 0.0;
    const double ratio = srcRate > 0.0 && preparedDeviceRate > 0.0
        ? srcRate / preparedDeviceRate : 1.0;
    const bool resampling = std::abs (ratio - 1.0) >= 1.0e-9;
    const int blockSize = std::max (1, preparedBlockSize);
    const double span = std::ceil ((double) blockSize * std::max (1.0, ratio))
        + (resampling ? 2 * dusk::audio::WindowedSincInterpolator::centeredRadius() + 2 : 0);
    const long double duration = srcRate > 0.0 && preparedDeviceRate > 0.0
        ? (long double) ownedReader->info().numFrames * (long double) preparedDeviceRate / (long double) srcRate
        : 0.0L;
    resampleReadPos = -1;
    resampleFraction = 0.0;
    resampleFramesRemaining = 0;
    resampleSeekRevision = 0;
    if (! std::isfinite (preparedDeviceRate) || preparedDeviceRate < 0.0
        || ! std::isfinite (ratio) || ratio <= 0.0
        || ! std::isfinite (duration) || duration >= 9223372036854775808.0L
        || ! std::isfinite (span)
        || span >= (double) dusk::audio::BufferedFileReader::kDefaultWindowFrames)
        return false;
    if (! readScratch.setSize (2, blockSize)
        || ! inScratch.setSize (2, (int) span)
        || (resampling && ! resampler.prepareCentered (ratio)))
        return false;
    speedRatio.store (ratio, std::memory_order_release);
    return true;
}

bool MasteringPlayer::loadFile (const juce::File& file)
{
    ControlChangeScope change (*this);
    // Closes the current file first, so a load that fails below doesn't
    // leave it open either.
    unloadFile();
    if (! file.existsAsFile()) return false;

    auto raw = dusk::audio::FileReader::open (
        std::filesystem::u8path (file.getFullPathName().toStdString()));
    if (raw == nullptr) return false;

    // Buffered so process() reads from prefetched memory: the default window
    // is 1-2 s of lead, and a miss (right after a seek) returns silence until
    // the prefetch catches up. Same pattern as PlaybackEngine.
    auto r = std::make_unique<dusk::audio::BufferedFileReader> (std::move (raw));

    // unloadFile left the audio thread stopped and parked on null. Drain
    // again: if its drain timed out, a block that latched the old reader may
    // still be using the scratch resized and the kernel prepared below.
    // On a second timeout refuse the load - stale state beats a data race.
    if (! parkAndWaitForAudio())
        return false;

    retiredReader.reset();
    ownedReader = std::move (r);
    loadedFile  = file;
    ++sourceGeneration;
    const auto seek = transport.beginSeek (0);
    transport.finishSeek (seek);
    // Size the resample scratch for this source's rate BEFORE the reader is
    // published - the audio thread is parked on null until the store below.
    resampleReady = updateResampleState();
    if (! resampleReady)
    {
        unloadFile();
        return false;
    }
    currentReader.store (ownedReader.get(), std::memory_order_release);
    return true;
}

void MasteringPlayer::unloadFile()
{
    ControlChangeScope change (*this);
    transport.stop();
    loadedFile = juce::File();
    ++sourceGeneration;
    const auto seek = transport.beginSeek (0);
    transport.finishSeek (seek);

    // Destroying the reader, here on the message thread, is what closes the
    // file - but only once the drain proves no block can still be reading it.
    const bool drained = parkAndWaitForAudio();
    if (ownedReader != nullptr)
        retiredReader = std::move (ownedReader);
    if (drained)
        retiredReader.reset();
}

std::int64_t MasteringPlayer::getLengthSamples() const noexcept
{
    return ownedReader ? ownedReader->info().numFrames : 0;
}

double MasteringPlayer::getSourceSampleRate() const noexcept
{
    return ownedReader ? ownedReader->info().sampleRate : 0.0;
}

bool MasteringPlayer::primeOfflineBlock (int numSamples)
{
    if (ownedReader == nullptr || ! resampleReady || numSamples <= 0
        || numSamples > readScratch.numSamples()) return false;
    ownedReader->setFillMode (dusk::audio::BufferedFileReader::Fill::Manual);
    const auto start = getPlayhead();
    const auto length = ownedReader->info().numFrames;
    if (start < 0) return false;
    if (start >= length) return true;
    const auto ratio = speedRatio.load (std::memory_order_acquire);
    const bool resampling = std::abs (ratio - 1.0) >= 1.0e-9;
    const int radius = resampling ? dusk::audio::WindowedSincInterpolator::centeredRadius() : 0;
    const auto windowStart = start - radius;
    const int needed = resampling ? (int) std::ceil ((double) numSamples * ratio) + 2 * radius + 2
                                  : numSamples;
    const auto first = std::max<std::int64_t> (0, windowStart);
    const auto available = std::min (length - first, (std::int64_t) needed - (first - windowStart));
    ownedReader->prefetch (first);
    ownedReader->fillNow();
    while (! ownedReader->holds (first, available)
           && ownedReader->fillSeekOnce (first, start, available)) {}
    return ownedReader->holds (first, available);
}

void MasteringPlayer::finishOfflineRender()
{
    if (ownedReader != nullptr)
    {
        ownedReader->prefetch (getPlayhead());
        ownedReader->setFillMode (dusk::audio::BufferedFileReader::Fill::Background);
    }
}

MasteringPlayer::BlockInfo MasteringPlayer::process (float* L, float* R, int numSamples) noexcept
{
    if (L == nullptr || R == nullptr || numSamples <= 0) return {};
    BlockInfo info;
    info.revision = controlRevision.load (std::memory_order_seq_cst);
    const bool stableAtEntry = controlsInFlight.load (std::memory_order_seq_cst) == 0;
    const auto finish = [&]() noexcept
    {
        info.stable = stableAtEntry && controlsInFlight.load (std::memory_order_seq_cst) == 0
                      && controlRevision.load (std::memory_order_seq_cst) == info.revision;
        return info;
    };
    std::memset (L, 0, sizeof (float) * (size_t) numSamples);
    std::memset (R, 0, sizeof (float) * (size_t) numSamples);

    info.playing = transport.isPlaying();
    if (! info.playing) return finish();

    // Bump BEFORE loading currentReader: parkAndWaitForAudio clears the
    // pointer then drains this counter, so a block that got past the null
    // check is waited on before the scratch/kernel storage is mutated.
    AudioInFlightScope guard (audioInFlight);

    // Load the reader pointer once and use it for the whole block. It
    // acquires the publishing release-stores and is seq_cst for the park's
    // handshake (see audioInFlight).
    auto* r = currentReader.load (std::memory_order_seq_cst);
    if (r == nullptr)
    {
        info.completeSourceRead = false;
        return finish();
    }

    const auto block = transport.beginBlock();
    info.playing = block.playing;
    if (! info.playing) return finish();
    const std::int64_t start = block.position;
    info.sourceStart = info.sourceEnd = start;
    const std::int64_t length = r->info().numFrames;
    const bool         mono   = r->info().numChannels < 2;
    if (start < 0)
    {
        info.completeSourceRead = false;
        return finish();
    }
    if (start >= length)
    {
        // Past EOF - auto-stop so the UI can flip the Play button back.
        transport.finishBlock (block, start, true);
        return finish();
    }

    const double ratio = speedRatio.load (std::memory_order_acquire);
    if (std::abs (ratio - 1.0) < 1.0e-9)
    {
        const int  available = (int) std::min ((std::int64_t) numSamples,
                                                  (std::int64_t) (length - start));
        if (available > readScratch.numSamples())
        {
            info.completeSourceRead = false;
            return finish();
        }

        // The reader zero-fills destination channels the file doesn't have, so
        // a mono source is duplicated here to keep the stage stereo.
        info.completeSourceRead = r->readRt (readScratch.data(), 2, start, available);
        if (mono)
            std::memcpy (readScratch.channel (1), readScratch.channel (0),
                          sizeof (float) * (size_t) available);

        std::memcpy (L, readScratch.channel (0), sizeof (float) * (size_t) available);
        std::memcpy (R, readScratch.channel (1), sizeof (float) * (size_t) available);

        info.sourceEnd = start + available;
        transport.finishBlock (block, info.sourceEnd, info.sourceEnd >= length);
        return finish();
    }

    if (numSamples > readScratch.numSamples())
    {
        info.completeSourceRead = false;
        return finish();
    }
    const auto seek = block.locateSerial;
    if (start != resampleReadPos || seek != resampleSeekRevision)
    {
        resampleFraction = 0.0;
        // Round the segment once from the original rates. Repeatedly rounding
        // its shrinking floating-point remainder loses half-frame ties.
        const long double duration = std::round ((long double) (length - start)
            * (long double) preparedDeviceRate / (long double) r->info().sampleRate);
        if (! std::isfinite (duration) || duration < 0.0L || duration >= 9223372036854775808.0L)
        {
            info.completeSourceRead = false;
            return finish();
        }
        resampleFramesRemaining = (std::int64_t) duration;
    }
    resampleSeekRevision = seek;

    const int frames = (int) std::min (resampleFramesRemaining, (std::int64_t) numSamples);
    constexpr int radius = dusk::audio::WindowedSincInterpolator::centeredRadius();
    const int needed = (int) std::ceil ((double) numSamples * ratio) + 2 * radius + 2;
    const auto windowStart = start - radius;
    info.completeSourceRead = r->readRt (inScratch.data(), 2, windowStart, needed);
    if (mono)
        std::memcpy (inScratch.channel (1), inScratch.channel (0), sizeof (float) * (size_t) needed);

    double phase = resampleFraction;
    for (int sample = 0; sample < frames; ++sample)
    {
        const int center = (int) phase;
        const double fraction = phase - center;
        L[sample] = resampler.centeredValue (inScratch.channel (0) + center, fraction);
        R[sample] = resampler.centeredValue (inScratch.channel (1) + center, fraction);
        phase += ratio;
    }
    const auto whole = (std::int64_t) phase;
    resampleFramesRemaining -= frames;
    const bool ended = resampleFramesRemaining == 0;
    const auto consumed = ended ? length - start : std::min (whole, length - start);
    resampleFraction = ended ? 0.0 : phase - (double) whole;
    resampleReadPos = start + consumed;
    info.sourceEnd = resampleReadPos;
    transport.finishBlock (block, info.sourceEnd, ended);
    return finish();
}
} // namespace duskstudio
