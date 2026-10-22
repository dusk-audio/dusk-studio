#include "PlaybackEngine.h"
#include "Transport.h"
#include "../foundation/Decibels.h"
#include "../session/RegionEnvelope.h"
#include "../session/TakeComp.h"
#include <algorithm>
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <thread>
#include <utility>

namespace duskstudio
{
namespace
{
template <typename FileType>
std::filesystem::path audioPath (const FileType& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

// Samples pre-cached at the loop start per region (~0.68 s at 48 kHz) -
// comfortably longer than the prefetch thread needs to re-warm a reader
// after the backward seek at a loop wrap.
constexpr int kLoopCacheSamples = 32768;

constexpr float kPi = 3.14159265358979f;

// Declick ramp applied on both sides of an in-block loop seam. 64 samples
// matches the punch-in click-mask fade (docs/DuskStudio.md §5b).
constexpr int kLoopSeamFade = 64;

// Loops shorter than this fall back to a plain linear read: the split loop
// would degenerate into per-sample spans.
constexpr std::int64_t kMinLoopLenForSplit = 2 * (std::int64_t) kLoopSeamFade;

// A track rebuilt while the transport rolls crosses from its old streams to its
// new ones over this many samples. Equal-gain, since most of what a comp edit
// leaves at the playhead is the same audio on both sides of the swap.
constexpr int kSwapFadeSamples = 512;

// A rebuild waits for its readers to hold this much past the playhead, but no
// longer than kSwapWarmWait: after that it is handed over regardless.
constexpr std::int64_t kSwapWarmFrames = 8192;
constexpr auto kSwapWarmWait = std::chrono::milliseconds (300);

std::atomic<int> liveStreams { 0 };
} // namespace
PlaybackEngine::PlaybackEngine (Session& s) : session (s) {}

PlaybackEngine::PerTrackStream::PerTrackStream() noexcept
{
    liveStreams.fetch_add (1, std::memory_order_relaxed);
}

PlaybackEngine::PerTrackStream::~PerTrackStream()
{
    liveStreams.fetch_sub (1, std::memory_order_relaxed);
}

int PlaybackEngine::liveStreamCountForTest() noexcept
{
    return liveStreams.load (std::memory_order_relaxed);
}

PlaybackEngine::~PlaybackEngine()
{
    stopPlayback();
}

void PlaybackEngine::prepare (int maxBlockSize, double sampleRate)
{
    fillPool.configure (sampleRate);
    // Pre-allocate the stereo scratch buffer once. Channel 1 is unused for
    // mono regions but allocated so the audio-thread read path never has to
    // grow on a stereo region.
    if (! readScratch.setSize (2, std::max (1, maxBlockSize))
        || ! fadeScratch.setSize (2, std::max (1, maxBlockSize)))
        std::fprintf (stderr,
                      "[Dusk Studio/PlaybackEngine] prepare: could not allocate a "
                      "%d-sample read scratch; region playback stays silent until "
                      "the next prepare.\n",
                      std::max (1, maxBlockSize));
}

void PlaybackEngine::beginAudioBlock (std::int64_t playhead, std::int64_t loopStart,
                                      std::int64_t loopEnd) noexcept
{
    AudioInFlightScope guard (audioInFlight);
    if (! streamsActive.load (std::memory_order_seq_cst)) return;
    fillPool.setPositionRt (playhead);
    const bool looping = loopStart >= 0 && loopEnd > loopStart;
    effectiveLoopStart.store (looping ? loopStart : -1, std::memory_order_release);
    fillPool.setLoopRt (looping, loopStart, loopEnd);
}

void PlaybackEngine::primeAt (std::int64_t playhead)
{
    fillPool.primeAt (playhead);
}

bool PlaybackEngine::locateReadyRt (std::int64_t target, int numSamples,
                                    std::int64_t loopStart, std::int64_t loopEnd) noexcept
{
    AudioInFlightScope guard (audioInFlight);
    if (! streamsActive.load (std::memory_order_seq_cst)) return true;
    pinStreamsForLocateBlock = false;
    auto warmFrames = std::max (numSamples, static_cast<int> (kSwapWarmFrames));
    const bool looping = loopStart >= 0 && loopEnd - loopStart >= kMinLoopLenForSplit
        && target >= loopStart && target < loopEnd;
    if (looping)
        warmFrames = (int) std::min<std::int64_t> (warmFrames, loopEnd - target);
    fillPool.setSeekTargetRt (target, warmFrames);
    if (! fillPool.seekReadyRt (target, warmFrames)) return false;
    if (looping)
    {
        if (numSamples > kLoopCacheSamples) return false;
        for (const auto& slot : slots)
            if (const auto* current = slot.current.load (std::memory_order_relaxed))
                if (! current->regions.empty() && current->loopCacheStart != loopStart)
                    return false;
    }
    pinStreamsForLocateBlock = true;
    return true;
}

void PlaybackEngine::finishLocateBlockRt (std::int64_t committedPosition, bool keepSeek) noexcept
{
    AudioInFlightScope guard (audioInFlight);
    if (! streamsActive.load (std::memory_order_seq_cst)) return;
    // Workers acquire a cleared seek before reading this committed position.
    fillPool.setPositionRt (committedPosition);
    if (! keepSeek)
    {
        fillPool.clearSeekTargetRt();
        pinStreamsForLocateBlock = false;
    }
}

void PlaybackEngine::refreshLiveRegionParams()
{
    // Walk every per-track stream and push the latest gainDb / muted
    // from the AudioRegion model into the cached RegionStream. Only
    // applies when the stream still matches the region by file +
    // timeline position + length; structural mismatches (split / join
    // / move) leave the stream untouched and the next preparePlayback
    // rebuilds. Both fields are atomics the audio thread loads per block. A
    // track with a rebuild waiting to be handed over is left alone: the
    // rebuild already carries these values, and the crossfade to it brings
    // them in without a step.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto* stream = slots[(size_t) t].current.load (std::memory_order_acquire);
        if (stream == nullptr || pending[(size_t) t].stream != nullptr
            || t == auditionedTrack.load (std::memory_order_relaxed)) continue;
        const auto& regs = session.track (t).regions;

        // Streams are sorted by timelineStart in preparePlayback; the
        // session.regions vector is NOT sorted. Match each stream by
        // walking regions until we find one with the same identity.
        for (auto& rs : stream->regions)
        {
            for (const auto& region : regs)
            {
                if (audioPath (region.file) != rs.sourcePath
                    || region.timelineStart   != rs.timelineStart
                    || region.lengthInSamples != rs.lengthInSamples) continue;
                rs.gainLinear.store (dusk::audio::decibelsToGain (
                    std::clamp (region.gainDb, -60.0f, 24.0f), -60.0f));
                rs.muted.store (region.muted);
                break;
            }
        }
    }
}

void PlaybackEngine::preparePlayback (Audition audition)
{
    ++rebuilds;
    stopPlayback();
    auditionedTrack.store (-1, std::memory_order_relaxed);
    auditionedTake.store (0, std::memory_order_relaxed);

    const std::int64_t warmAt = transport != nullptr ? transport->getCommittedPlayhead() : 0;
    const auto loopStart = transport != nullptr ? transport->getLoopStart() : -1;
    const auto loopEnd = transport != nullptr ? transport->getLoopEnd() : -1;
    const bool looping = transport != nullptr && transport->isLoopEnabled()
        && loopStart >= 0 && loopEnd > loopStart;
    cachedLoopStart = looping ? loopStart : -1;
    effectiveLoopStart.store (cachedLoopStart, std::memory_order_release);
    nextLoopCacheAttempt = {};
    requestedAudition.fill (audition);
    fillPool.setPositionRt (warmAt);
    fillPool.setLoopRt (looping, loopStart, loopEnd);
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        TakeId auditioned = 0;
        auto stream = buildTrackStream (t, audition, warmAt, auditioned);
        if (stream != nullptr && ! stream->loopCacheReady) cachedLoopStart = -1;
        if (auditioned != 0)
        {
            auditionedTrack.store (t, std::memory_order_relaxed);
            auditionedTake.store (auditioned, std::memory_order_relaxed);
        }
        slots[(size_t) t].current.store (stream.release(), std::memory_order_relaxed);
    }

    primeAt (warmAt);
    streamsActive.store (true, std::memory_order_release);
}

std::unique_ptr<PlaybackEngine::PerTrackStream>
PlaybackEngine::buildTrackStream (int t, Audition audition, std::int64_t warmAt, TakeId& auditioned)
{
    auditioned = 0;
    // A frozen track plays its single baked WAV (frozenRegion) instead of
    // its recorded regions / instrument. For a MIDI track that's the only
    // source of sound; for an audio track it replaces the recorded regions
    // (their audio is baked into the WAV). frozenRegion is populated by
    // commitFreeze / on session load before this runs. Everything downstream
    // (reader open, fades, readForTrack) is identical to a normal region.
    //
    // An auditioned take stands in for the regions the same way, whole and
    // unfaded, unless the track is frozen or the take is gone.
    const auto& takeAudition = session.takeAudition;
    const auto& track = session.track (t);
    const bool frozen = track.frozen.load (std::memory_order_acquire);
    std::vector<AudioRegion> substitute;
    if (frozen)
    {
        substitute.push_back (track.frozenRegion);
    }
    else if (audition == Audition::Honour && takeAudition.trackIdx == t)
    {
        if (const auto* take = findTake (track, takeAudition.takeId))
            if (auto whole = regionFromTake (*take, take->timelineStart,
                                             take->timelineStart + take->lengthInSamples))
            {
                substitute.push_back (std::move (*whole));
                auditioned = take->id;
            }
    }
    const auto& regions = substitute.empty() ? track.regions : substitute;
    if (regions.empty()) return nullptr;

    auto stream = std::make_unique<PerTrackStream>();
    stream->regions.reserve (regions.size());

    for (const auto& region : regions)
    {
        if (! region.file.existsAsFile()) continue;

        auto rawReader = dusk::audio::FileReader::open (audioPath (region.file));
        if (rawReader == nullptr) continue;
        const auto sourceInfo = rawReader->info();

        // 96000 frames is ~1 s at 96 kHz / ~2 s at 44.1 kHz - generous
        // given block sizes are 256-2048 - but a region shorter than that
        // never reads past its own end, and a take-heavy session opens one
        // reader per region, so cap the window at the region length.
        const std::int64_t windowFrames =
            std::clamp (region.lengthInSamples, (std::int64_t) 8192,
                         dusk::audio::BufferedFileReader::kDefaultWindowFrames);
        auto buffered = std::make_unique<dusk::audio::BufferedFileReader> (
            std::move (rawReader), windowFrames,
            dusk::audio::BufferedFileReader::Fill::Manual);
        // Warm where playback will begin, not at the region head - every
        // caller sets the playhead before rebuilding, so Play from mid-song
        // starts warm instead of dropping blocks while the window re-seeks.
        const std::int64_t startInRegion =
            std::clamp (warmAt - region.timelineStart,
                         (std::int64_t) 0, std::max ((std::int64_t) 0, region.lengthInSamples));
        buffered->prefetch (region.sourceOffset + startInRegion);
        if (synchronousReaders)
            buffered->fillNow();

        RegionStream rs;
        rs.reader = decltype(rs.reader) (buffered.release(),
                                         ReaderDeleter { synchronousReaders ? nullptr : &fillPool });
        if (! synchronousReaders)
            fillPool.add (*rs.reader, region.timelineStart,
                          region.timelineStart + region.lengthInSamples, region.sourceOffset);
        rs.sourcePath      = audioPath (region.file);
        rs.timelineStart   = region.timelineStart;
        rs.lengthInSamples = region.lengthInSamples;
        rs.sourceOffset    = region.sourceOffset;
        rs.fadeInSamples   = region.fadeInSamples;
        rs.fadeOutSamples  = region.fadeOutSamples;
        fitFadesToLength (rs.fadeInSamples, rs.fadeOutSamples, rs.lengthInSamples);
        rs.fadeInShape     = region.fadeInShape;
        rs.fadeOutShape    = region.fadeOutShape;
        // Channel count comes from the DECODED file, not the model's copy:
        // region.numChannels can disagree with what is on disk (hand-edited
        // session, file replaced), and reading by it either truncates a
        // stereo take to its left channel or asks a mono file for a right
        // one. Captured before the reader moves into the buffering wrapper.
        rs.numChannels     = std::clamp (sourceInfo.numChannels, 1, 2);
        // Convert dB once on the message thread; the audio loop
        // multiplies by the linear factor per sample. Clamp the
        // dB at extreme values to avoid wild values from a hand-
        // edited session.json producing audible clip on first
        // play. The Alt-drag clamps tighter ([-24, +12]) at the UI.
        rs.gainLinear.store (dusk::audio::decibelsToGain (
            std::clamp (region.gainDb, -60.0f, 24.0f), -60.0f));
        rs.muted.store (region.muted);
        stream->regions.push_back (std::move (rs));
    }

    // Sort by timelineStart so the audio thread can stop iterating as
    // soon as it sees a region beyond the current block. Equal starts
    // preserve insertion order so the most-recently-recorded take wins
    // on overlap (recorder appends to the back of session.regions).
    std::stable_sort (stream->regions.begin(), stream->regions.end(),
                       [] (const RegionStream& a, const RegionStream& b)
                       {
                           return a.timelineStart < b.timelineStart;
                       });

    // Implicit-crossfade overlap detection. Walk adjacent pairs in the
    // sorted list; when region[i-1] extends into region[i], record the
    // overlap length on both sides. The audio thread later uses these
    // to ramp out the leading region + ramp in the trailing region
    // across the overlap, so summed power stays ~unity instead of
    // doubling. Only adjacent pairs are handled here; triple-stacked
    // takes degrade to "newest wins" via existing summation behaviour.
    for (size_t i = 1; i < stream->regions.size(); ++i)
    {
        auto& a = stream->regions[i - 1];
        auto& b = stream->regions[i];
        const std::int64_t overlap = overlapWithNext (a.timelineStart, a.lengthInSamples,
                                                      b.timelineStart, b.lengthInSamples);
        a.overlapNextLen = overlap;
        b.overlapPrevLen = overlap;
    }

    if (stream->regions.empty())
        return nullptr;
    stream->loopCacheReady = primeLoopCache (*stream, effectiveLoopStart.load (std::memory_order_acquire));
    return stream;
}

bool PlaybackEngine::primeLoopCache (PerTrackStream& stream, std::int64_t loopStart)
{
    stream.loopCacheStart = -1;
    if (loopStart < 0) return true;
    bool complete = true;

    for (auto& rs : stream.regions)
    {
        rs.loopCacheTimelineStart = -1;
        rs.loopCacheLen           = 0;

        const std::int64_t regionEnd  = rs.timelineStart + rs.lengthInSamples;
        const std::int64_t cacheStart = std::max (loopStart, rs.timelineStart);
        const std::int64_t cacheEnd   = std::min (loopStart + (std::int64_t) kLoopCacheSamples,
                                                    regionEnd);
        if (cacheEnd <= cacheStart) continue;

        // A fresh plain reader for the fill: the region's own buffered
        // reader would return silence on a cold window, and bumping its
        // window here would fight the audio thread's forward prefetch.
        auto fillReader = dusk::audio::FileReader::open (rs.sourcePath);
        if (fillReader == nullptr) { complete = false; continue; }

        const auto sourceStart = rs.sourceOffset + (cacheStart - rs.timelineStart);
        const int len = static_cast<int> (std::min (cacheEnd - cacheStart,
            std::max<std::int64_t> (0, fillReader->info().numFrames - sourceStart)));
        if (len == 0) continue;
        // A region can claim two channels over a mono file (hand-edited
        // session, file replaced on disk). Reading one channel and letting
        // the audio path duplicate it keeps that case center-panned instead
        // of silencing the right side.
        const bool stereo = (rs.numChannels == 2) && fillReader->info().numChannels >= 2;
        dusk::audio::PlanarBuffer tmp;
        if (! tmp.setSize (2, len)) { complete = false; continue; }
        if (fillReader->read (tmp.data(), stereo ? 2 : 1, sourceStart, len) != len)
        { complete = false; continue; }

        rs.loopCacheL.assign (tmp.channel (0), tmp.channel (0) + len);
        if (stereo)
            rs.loopCacheR.assign (tmp.channel (1), tmp.channel (1) + len);
        else
            rs.loopCacheR.clear();
        rs.loopCacheTimelineStart = cacheStart;
        rs.loopCacheLen           = len;
    }
    if (complete) stream.loopCacheStart = loopStart;
    return complete;
}

void PlaybackEngine::stopPlayback()
{
    streamsActive.store (false, std::memory_order_seq_cst);

    // Neither of these is the audio thread's: pending is message-thread only, and
    // a stream it has not exchanged out of incoming it never will. Dropped before
    // the drain, so a drain that gives up cannot leave a pre-stop rebuild to swap
    // in over the streams the next preparePlayback builds.
    for (auto& swap : pending) swap.stream.reset();
    for (auto& slot : slots)
        delete slot.incoming.exchange (nullptr, std::memory_order_acq_rel);

    // Drain in-flight readForTrack calls before destroying the readers.
    // The audio callback latches the transport state once per block, so
    // it can still be mid-sum when the message thread gets here. Time-
    // bounded drain (a fixed yield count can elapse in microseconds on a
    // contended box - less than one legitimate callback - and bail
    // spuriously). If the audio thread is genuinely stuck past the
    // deadline, leave the streams allocated - streamsActive stays false
    // so no new reads start. A preparePlayback that follows installs fresh
    // streams over them, so they are never freed. Leak beats UAF.
    constexpr auto kDrainTimeout = std::chrono::milliseconds (200);
    const auto drainDeadline = std::chrono::steady_clock::now() + kDrainTimeout;
    while (audioInFlight.load (std::memory_order_seq_cst) > 0)
    {
        if (std::chrono::steady_clock::now() > drainDeadline)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/PlaybackEngine] stopPlayback: audioInFlight=%d "
                          "after %lld ms; BAILING teardown to avoid UAF. Its streams "
                          "are leaked.\n",
                          audioInFlight.load (std::memory_order_relaxed),
                          (long long) kDrainTimeout.count());
            return;
        }
        std::this_thread::yield();
    }

    fillPool.clearSeekTargetRt();
    pinStreamsForLocateBlock = false;
    refreshRequests.store (0, std::memory_order_relaxed);
    for (auto& slot : slots) freeSlot (slot);
}

void PlaybackEngine::refreshTrackPlayback (int trackIndex, Audition audition)
{
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks || ! streamsActive.load (std::memory_order_acquire))
        return;
    requestedAudition[(size_t) trackIndex] = audition;
    ++rebuilds;
    const std::int64_t playhead = transport != nullptr ? transport->getCommittedPlayhead() : 0;
    auto& swap = pending[(size_t) trackIndex];
    swap.stream = buildTrackStream (trackIndex, audition, playhead, swap.auditioned);
    // A track left with nothing to play still swaps, to silence.
    if (swap.stream == nullptr)
        swap.stream = std::make_unique<PerTrackStream>();
    if (! swap.stream->loopCacheReady) cachedLoopStart = -1;
    swap.deadline = std::chrono::steady_clock::now() + kSwapWarmWait;
}

void PlaybackEngine::service()
{
    const bool live = streamsActive.load (std::memory_order_acquire);
    const auto refresh = refreshRequests.exchange (0, std::memory_order_acq_rel);
    if (live)
        for (int t = 0; t < Session::kNumTracks; ++t)
            if ((refresh & (std::uint32_t { 1 } << t)) != 0)
                refreshTrackPlayback (t, requestedAudition[(size_t) t]);
    const auto now = std::chrono::steady_clock::now();
    const auto loopStart = effectiveLoopStart.load (std::memory_order_acquire);
    if (live && loopStart != cachedLoopStart && now >= nextLoopCacheAttempt)
    {
        bool complete = true;
        if (loopStart >= 0)
            for (int t = 0; t < Session::kNumTracks; ++t)
            {
                auto& swap = pending[(size_t) t];
                if (swap.stream != nullptr)
                    swap.stream->loopCacheReady = primeLoopCache (*swap.stream, loopStart);
                else
                {
                    const auto& track = session.track (t);
                    if (track.regions.empty() && ! track.frozen.load (std::memory_order_acquire)
                        && ! (requestedAudition[(size_t) t] == Audition::Honour
                              && session.takeAudition.trackIdx == t && session.takeAudition.takeId != 0))
                        continue;
                    refreshTrackPlayback (t, requestedAudition[(size_t) t]);
                }
                complete = complete && swap.stream->loopCacheReady;
            }
        if (complete) cachedLoopStart = loopStart;
        nextLoopCacheAttempt = complete ? std::chrono::steady_clock::time_point {}
                                       : now + std::chrono::milliseconds (100);
    }
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& swap = pending[(size_t) t];
        if (swap.stream == nullptr) continue;
        if (! live)
            swap.stream.reset();
        else
        {
            const auto position = transport != nullptr ? transport->getAutomationPosition()
                                                       : Transport::AutomationPosition { 0, 0 };
            const bool warm = readersHoldFrom (*swap.stream, position.samples);
            if (warm || now >= swap.deadline)
                publish (t, position.samples, position.locateSerial, warm);
        }
    }

    for (auto& slot : slots)
        for (auto& retired : slot.retired)
            delete retired.exchange (nullptr, std::memory_order_acq_rel);
}

bool PlaybackEngine::hasPendingWork() const noexcept
{
    for (const auto& swap : pending)
        if (swap.stream != nullptr) return true;
    for (const auto& slot : slots)
    {
        if (slot.incoming.load (std::memory_order_acquire) != nullptr) return true;
        for (const auto& retired : slot.retired)
            if (retired.load (std::memory_order_acquire) != nullptr) return true;
    }
    return false;
}

void PlaybackEngine::publish (int t, std::int64_t playhead, std::uint32_t locateSerial, bool warm)
{
    auto& swap = pending[(size_t) t];
    swap.stream->warmStart = playhead;
    swap.stream->warmEnd = warm ? playhead + kSwapWarmFrames : playhead;
    swap.stream->warmLocateSerial = locateSerial;
    if (swap.auditioned != 0)
    {
        auditionedTrack.store (t, std::memory_order_relaxed);
        auditionedTake.store (swap.auditioned, std::memory_order_relaxed);
    }
    else if (auditionedTrack.load (std::memory_order_relaxed) == t)
    {
        auditionedTrack.store (-1, std::memory_order_relaxed);
        auditionedTake.store (0, std::memory_order_relaxed);
    }
    ++swaps;
    // Streams the audio thread never took are this thread's to free.
    delete slots[(size_t) t].incoming.exchange (swap.stream.release(), std::memory_order_acq_rel);
}

bool PlaybackEngine::readersHoldFrom (const PerTrackStream& stream, std::int64_t playhead) noexcept
{
    const std::int64_t until = playhead + kSwapWarmFrames;
    for (const auto& r : stream.regions)
    {
        if (r.reader == nullptr) continue;
        const std::int64_t from = std::max (playhead, r.timelineStart);
        const std::int64_t to = std::min (until, r.timelineStart + r.lengthInSamples);
        if (to > from && ! r.reader->holds (r.sourceOffset + (from - r.timelineStart), to - from))
            return false;
    }
    return true;
}

bool PlaybackEngine::canAdopt (const PerTrackStream& stream, std::int64_t playhead, int numSamples,
                                std::int64_t loopStart, std::int64_t loopEnd,
                                std::uint32_t locateSerial) noexcept
{
    if (stream.regions.empty()) return true;
    const auto loopLength = loopEnd - loopStart;
    const bool looping = loopStart >= 0 && loopLength >= kMinLoopLenForSplit && playhead < loopEnd;
    if (looping && stream.loopCacheStart != loopStart) return false;
    const auto cached = [&] (std::int64_t start, int count) noexcept
    {
        return stream.loopCacheStart >= 0 && start >= stream.loopCacheStart
            && count <= kLoopCacheSamples
            && start - stream.loopCacheStart <= kLoopCacheSamples - count;
    };
    const auto resident = [&] (std::int64_t start, int count) noexcept
    {
        return cached (start, count)
            || (stream.warmLocateSerial == locateSerial && start >= stream.warmStart
                && start <= stream.warmEnd && count <= stream.warmEnd - start);
    };
    if (! looping) return resident (playhead, numSamples);
    const auto first = (int) std::min<std::int64_t> (numSamples, loopEnd - playhead);
    return resident (playhead, first)
        && (first == numSamples || cached (loopStart,
            (int) std::min<std::int64_t> (numSamples - first, loopLength)));
}

void PlaybackEngine::freeSlot (TrackSlot& slot)
{
    delete slot.current.exchange (nullptr, std::memory_order_acq_rel);
    delete slot.incoming.exchange (nullptr, std::memory_order_acq_rel);
    for (auto& retired : slot.retired)
        delete retired.exchange (nullptr, std::memory_order_acq_rel);
    delete std::exchange (slot.fadingFrom, nullptr);
    slot.fadePos = -1;
}

void PlaybackEngine::readForTrack (int trackIndex,
                                   std::int64_t playheadSamples,
                                   float* outL,
                                   float* outR,
                                   int numSamples,
                                   std::int64_t loopStart,
                                   std::int64_t loopEnd) noexcept
{
    if (outL == nullptr) return;
    std::memset (outL, 0, sizeof (float) * (size_t) numSamples);
    if (outR != nullptr)
        std::memset (outR, 0, sizeof (float) * (size_t) numSamples);

    // Bump BEFORE checking streamsActive: stopPlayback clears the flag,
    // then drains this counter, so any read that got past the check is
    // waited on before the readers are destroyed. Reads that bump after
    // the flag cleared bail here and output stays silent.
    AudioInFlightScope guard (audioInFlight);
    if (! streamsActive.load (std::memory_order_seq_cst)) return;

    auto& slot = slots[(size_t) trackIndex];
    auto* current = slot.current.load (std::memory_order_relaxed);

    // A rebuild waits for the fade before it, so no streams are ever dropped
    // part-way through fading in or out.
    if (slot.fadePos < 0 && ! pinStreamsForLocateBlock)
        if (auto* next = slot.incoming.exchange (nullptr, std::memory_order_acq_rel))
        {
            const auto serial = transport != nullptr ? transport->getLocateCount() : std::uint32_t { 0 };
            if (canAdopt (*next, playheadSamples, numSamples, loopStart, loopEnd, serial))
            {
                slot.fadingFrom = current;
                slot.fadePos = 0;
                current = next;
                slot.current.store (current, std::memory_order_release);
            }
            else
            {
                // Ownership arrived through exchange; retire stale input without replacing current audio.
                slot.fadingFrom = next;
                slot.fadePos = kSwapFadeSamples;
                refreshRequests.fetch_or (std::uint32_t { 1 } << trackIndex, std::memory_order_release);
            }
        }

    if (current != nullptr)
        readStream (*current, playheadSamples, outL, outR, numSamples, loopStart, loopEnd);

    if (slot.fadePos < 0)
        return;

    if (slot.fadePos < kSwapFadeSamples)
    {
        float* fadeL = nullptr;
        float* fadeR = nullptr;
        int oldAvailable = numSamples;
        if (slot.fadingFrom != nullptr && numSamples <= fadeScratch.numSamples())
        {
            fadeL = fadeScratch.channel (0);
            fadeR = outR != nullptr ? fadeScratch.channel (1) : nullptr;
            std::memset (fadeL, 0, sizeof (float) * (size_t) numSamples);
            if (fadeR != nullptr)
                std::memset (fadeR, 0, sizeof (float) * (size_t) numSamples);
            oldAvailable = readStream (*slot.fadingFrom, playheadSamples, fadeL, fadeR, numSamples, loopStart, loopEnd);
        }
        const int fading = std::min (numSamples, kSwapFadeSamples - slot.fadePos);
        for (int i = 0; i < fading; ++i)
        {
            // A missing old span cannot contribute to the transition.
            const float in = i >= oldAvailable ? 1.0f
                : 0.5f - 0.5f * std::cos (kPi * (float) (slot.fadePos + i) / (float) kSwapFadeSamples);
            const float out = 1.0f - in;
            outL[i] = outL[i] * in + (fadeL != nullptr ? fadeL[i] * out : 0.0f);
            if (outR != nullptr)
                outR[i] = outR[i] * in + (fadeR != nullptr ? fadeR[i] * out : 0.0f);
        }
        slot.fadePos = oldAvailable < fading ? kSwapFadeSamples : slot.fadePos + fading;
    }

    // Faded out: the old streams go back to the message thread to be freed. With
    // every retired entry still taken they wait here, and so does the next rebuild.
    if (slot.fadePos >= kSwapFadeSamples)
    {
        if (slot.fadingFrom == nullptr)
        {
            slot.fadePos = -1;
            return;
        }
        for (auto& retired : slot.retired)
        {
            PerTrackStream* empty = nullptr;
            if (retired.compare_exchange_strong (empty, slot.fadingFrom, std::memory_order_release,
                                                 std::memory_order_relaxed))
            {
                slot.fadingFrom = nullptr;
                slot.fadePos = -1;
                return;
            }
        }
    }
}

int PlaybackEngine::readStream (PerTrackStream& stream, std::int64_t playheadSamples, float* outL, float* outR,
                                 int numSamples, std::int64_t loopStart, std::int64_t loopEnd) noexcept
{
    const std::int64_t loopLen = loopEnd - loopStart;
    const bool splitAtLoop = loopStart >= 0 && loopLen >= kMinLoopLenForSplit
                              && playheadSamples < loopEnd;
    if (! splitAtLoop)
    {
        return readSpanForTrack (stream, playheadSamples, outL, outR, 0, numSamples);
    }

    // Loop-aware read: fill the block piecewise, wrapping the read position
    // exactly like the transport's post-block wrap (loopStart + overshoot),
    // so audio at the seam neither bleeds past loopEnd nor skips the loop
    // downbeat. Seams inside this block get a short declick ramp.
    int seamOffsets[8];
    int numSeams = 0;
    int done = 0;
    int firstMissing = numSamples;
    std::int64_t pos = playheadSamples;
    while (done < numSamples)
    {
        if (pos >= loopEnd)
        {
            pos = loopStart + (pos - loopEnd) % loopLen;
            if (numSeams < 8) seamOffsets[numSeams++] = done;
        }
        const int span = (int) std::min ((std::int64_t) (numSamples - done),
                                            loopEnd - pos);
        const int missing = readSpanForTrack (stream, pos, outL, outR, done, span);
        if (missing < done + span) firstMissing = std::min (firstMissing, missing);
        done += span;
        pos  += span;
    }

    for (int s = 0; s < numSeams; ++s)
    {
        const int seam = seamOffsets[s];
        // Fade out into the seam, fade in out of it - raised-cosine, zero
        // slope at both ends, same shape as the punch click-mask.
        for (int i = 1; i <= kLoopSeamFade; ++i)
        {
            const int idx = seam - i;
            if (idx < 0) break;
            // g rises with distance from the seam: ~0 at the sample next to
            // the wrap, 1 at the far edge of the fade window - so multiply by
            // g directly to silence the seam and leave the far edge untouched.
            const float g = 0.5f - 0.5f * std::cos (kPi * (float) i / (float) kLoopSeamFade);
            outL[idx] *= g;
            if (outR != nullptr) outR[idx] *= g;
        }
        for (int i = 0; i < kLoopSeamFade; ++i)
        {
            const int idx = seam + i;
            if (idx >= numSamples) break;
            const float g = 0.5f - 0.5f * std::cos (kPi * (float) i / (float) kLoopSeamFade);
            outL[idx] *= g;
            if (outR != nullptr) outR[idx] *= g;
        }
    }
    return firstMissing;
}

int PlaybackEngine::readSpanForTrack (PerTrackStream& slotRef,
                                       std::int64_t playheadSamples,
                                       float* outL,
                                       float* outR,
                                       int outBase,
                                       int numSamples) noexcept
{
    outL += outBase;
    if (outR != nullptr) outR += outBase;

    const std::int64_t blockEnd = playheadSamples + numSamples;
    int firstMissing = outBase + numSamples;

    for (auto& r : slotRef.regions)
    {
        if (r.reader == nullptr) continue;
        if (r.muted.load()) continue;

        // Regions are sorted by timelineStart - once we see one that begins
        // past the block, no later region can overlap us either.
        if (r.timelineStart >= blockEnd) break;

        const std::int64_t regionEnd = r.timelineStart + r.lengthInSamples;
        if (regionEnd <= playheadSamples) continue;  // already past

        const std::int64_t firstWithin = std::max (playheadSamples, r.timelineStart);
        const std::int64_t lastWithin  = std::min (blockEnd, regionEnd);
        const int outOffset    = (int) (firstWithin - playheadSamples);
        const int withinSamples = (int) (lastWithin - firstWithin);
        if (withinSamples <= 0) continue;
        // If this fires, prepare() was called with a maxBlockSize smaller than
        // the host's actual block size. Skip silently in release so we don't
        // crash, but make the misconfiguration visible in debug.
        assert (withinSamples <= readScratch.numSamples());
        if (withinSamples > readScratch.numSamples())
        {
            firstMissing = std::min (firstMissing, outBase + outOffset);
            continue;
        }

        const std::int64_t readStart = r.sourceOffset + (firstWithin - r.timelineStart);
        // For mono regions, read L only. For stereo, read both. readScratch is
        // always 2-channel, so the call is safe either way. A stereo region
        // over a mono file reads one channel and duplicates below, rather than
        // taking the reader's zero-filled second channel as silence.
        const bool readStereo = (r.numChannels == 2) && r.reader->info().numChannels >= 2;
        dusk::audio::BufferedFileReader::ReadCoverage coverage;
        const bool resident = r.reader->readRt (readScratch.data(), readStereo ? 2 : 1,
                                                readStart, withinSamples, &coverage);
        auto cached = coverage;
        cached.endFrame = cached.startFrame;

        // Serve the loop-start window from the pre-cache: the forward-only
        // reader misses (returns silence) right after a wrap's backward
        // seek. The cache holds absolute-timeline source samples, so any
        // overlap is valid data whether or not the reader was warm.
        if (r.loopCacheLen > 0
            && firstWithin < r.loopCacheTimelineStart + r.loopCacheLen
            && lastWithin  > r.loopCacheTimelineStart)
        {
            const std::int64_t ovStart = std::max (firstWithin, r.loopCacheTimelineStart);
            const std::int64_t ovEnd   = std::min (lastWithin,
                                                     r.loopCacheTimelineStart
                                                         + (std::int64_t) r.loopCacheLen);
            const int dstOff = (int) (ovStart - firstWithin);
            const int srcOff = (int) (ovStart - r.loopCacheTimelineStart);
            const int n      = (int) (ovEnd - ovStart);
            cached = { readStart + dstOff, readStart + dstOff + n };
            std::memcpy (readScratch.channel (0) + dstOff,
                          r.loopCacheL.data() + srcOff, sizeof (float) * (size_t) n);
            if (readStereo && ! r.loopCacheR.empty())
                std::memcpy (readScratch.channel (1) + dstOff,
                              r.loopCacheR.data() + srcOff, sizeof (float) * (size_t) n);
        }
        if (! resident)
        {
            if (cached.startFrame < coverage.startFrame) std::swap (cached, coverage);
            auto availableUntil = std::max<std::int64_t> (0, readStart);
            if (coverage.startFrame <= availableUntil)
                availableUntil = std::max (availableUntil, coverage.endFrame);
            if (cached.startFrame <= availableUntil)
                availableUntil = std::max (availableUntil, cached.endFrame);
            const auto wantedEnd = std::min (readStart + withinSamples, r.reader->info().numFrames);
            if (availableUntil < wantedEnd)
                firstMissing = std::min (firstMissing, outBase + outOffset + (int) (availableUntil - readStart));
        }

        // SUM (instead of REPLACE) into the output buffer(s) under the
        // region's envelope, the one the join render bakes. Summing lets two
        // regions overlap during a crossfade window. Mono regions duplicate
        // the L channel into outR (when outR is non-null) so the strip's
        // stereo path sees a center-panned signal.
        const auto envelope = regionEnvelope (r.fadeInSamples, r.fadeInShape,
                                              r.fadeOutSamples, r.fadeOutShape,
                                              r.overlapPrevLen, r.overlapNextLen);
        const auto* srcL = readScratch.channel (0);
        const auto* srcR = readStereo ? readScratch.channel (1) : srcL;
        const float regionGain = r.gainLinear.load();
        const std::int64_t sinceStart = firstWithin - r.timelineStart;
        const std::int64_t untilEnd   = regionEnd - firstWithin;
        float* dstL = outL + outOffset;
        float* dstR = outR != nullptr ? outR + outOffset : nullptr;
        const auto faded = [&] (int from, int to) noexcept
        {
            for (int i = from; i < to; ++i)
            {
                const float gain = envelope.gainAt (regionGain, sinceStart + i, untilEnd - i);
                dstL[i] += srcL[i] * gain;
                if (dstR != nullptr)
                    dstR[i] += srcR[i] * gain;
            }
        };
        // Most of a region sits between its fades, where the gain is the
        // region's alone: a plain multiply-add the compiler vectorises.
        const auto unfaded = envelope.unfaded (sinceStart, untilEnd, withinSamples);
        const int first = (int) unfaded.first;
        const int last  = (int) unfaded.last;
        faded (0, first);
        for (int i = first; i < last; ++i)
            dstL[i] += srcL[i] * regionGain;
        if (dstR != nullptr)
            for (int i = first; i < last; ++i)
                dstR[i] += srcR[i] * regionGain;
        faded (last, withinSamples);
    }
    return firstMissing;
}
} // namespace duskstudio
