#include "PlaybackEngine.h"
#include "Transport.h"
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace duskstudio
{
namespace
{
// Samples pre-cached at the loop start per region (~0.68 s at 48 kHz) -
// comfortably longer than the prefetch thread needs to re-warm a reader
// after the backward seek at a loop wrap.
constexpr int kLoopCacheSamples = 32768;

// Declick ramp applied on both sides of an in-block loop seam. 64 samples
// matches the punch-in click-mask fade (DuskStudio.md §5b).
constexpr int kLoopSeamFade = 64;

// Loops shorter than this fall back to a plain linear read: the split loop
// would degenerate into per-sample spans.
constexpr std::int64_t kMinLoopLenForSplit = 2 * (std::int64_t) kLoopSeamFade;
} // namespace
PlaybackEngine::PlaybackEngine (Session& s) : session (s)
{
    formatManager.registerBasicFormats();

    // Start the prefetch thread once at construction. BufferingAudioReader
    // instances created later in preparePlayback() attach themselves via
    // addTimeSliceClient (handled by the BufferingAudioReader ctor) and
    // detach on destruction in stopPlayback().
    bufferingThread.startThread();
}

PlaybackEngine::~PlaybackEngine()
{
    // Tear down readers (which detach from bufferingThread) before letting
    // the thread member go out of scope. stopPlayback handles the readers;
    // ~TimeSliceThread joins.
    stopPlayback();
    bufferingThread.stopThread (2000);
}

void PlaybackEngine::prepare (int maxBlockSize)
{
    // Pre-allocate the stereo scratch buffer once. Channel 1 is unused for
    // mono regions but allocated so the audio-thread read path never has to
    // grow on a stereo region.
    readScratch.setSize (2, std::max (1, maxBlockSize),
                          /*keepExistingContent*/ false,
                          /*clearExtraSpace*/      false,
                          /*avoidReallocating*/    false);
}

void PlaybackEngine::refreshLiveRegionParams()
{
    // Walk every per-track stream and push the latest gainDb / muted
    // from the AudioRegion model into the cached RegionStream. Only
    // applies when the stream still matches the region by file +
    // timeline position + length; structural mismatches (split / join
    // / move) leave the stream untouched and the next preparePlayback
    // rebuilds. Single field overwrites are hardware-atomic for the
    // naturally-aligned scalar types involved.
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        auto& stream = streams[(size_t) t];
        if (stream == nullptr) continue;
        const auto& regs = session.track (t).regions;

        // Streams are sorted by timelineStart in preparePlayback; the
        // session.regions vector is NOT sorted. Match each stream by
        // walking regions until we find one with the same identity.
        for (auto& rs : stream->regions)
        {
            for (const auto& region : regs)
            {
                if (region.file != rs.sourceFile
                    || region.timelineStart   != rs.timelineStart
                    || region.lengthInSamples != rs.lengthInSamples) continue;
                rs.gainLinear = juce::Decibels::decibelsToGain (
                    std::clamp (region.gainDb, -60.0f, 24.0f), -60.0f);
                rs.muted = region.muted;
                break;
            }
        }
    }
}

void PlaybackEngine::preparePlayback()
{
    stopPlayback();

    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        // A frozen track plays its single baked WAV (frozenRegion) instead of
        // its recorded regions / instrument. For a MIDI track that's the only
        // source of sound; for an audio track it replaces the recorded regions
        // (their audio is baked into the WAV). frozenRegion is populated by
        // commitFreeze / on session load before this runs. Everything downstream
        // (reader open, fades, readForTrack) is identical to a normal region.
        const bool frozen = session.track (t).frozen.load (std::memory_order_acquire);
        const std::vector<AudioRegion> frozenOne =
            frozen ? std::vector<AudioRegion> { session.track (t).frozenRegion }
                   : std::vector<AudioRegion> {};
        const auto& regions = frozen ? frozenOne : session.track (t).regions;
        if (regions.empty()) continue;

        auto stream = std::make_unique<PerTrackStream>();
        stream->regions.reserve (regions.size());

        for (const auto& region : regions)
        {
            if (! region.file.existsAsFile()) continue;

            std::unique_ptr<juce::AudioFormatReader> rawReader (
                formatManager.createReaderFor (region.file));
            if (rawReader == nullptr) continue;

            // Wrap in a BufferingAudioReader. Sample rate of the source isn't
            // known at this scope without inspecting `rawReader`, so size by
            // a fixed sample count: 96000 samples is ~1 s at 96 kHz / ~2 s at
            // 44.1 kHz - generous given block sizes are 256-2048. Read
            // timeout 0 keeps the audio thread non-blocking; missed reads
            // return silence until prefetch catches up.
            constexpr int kSamplesToBuffer = 96000;
            auto buffered = std::make_unique<juce::BufferingAudioReader> (
                rawReader.release(), bufferingThread, kSamplesToBuffer);
            buffered->setReadTimeout (0);

            RegionStream rs;
            rs.reader          = std::move (buffered);
            rs.sourceFile      = region.file;
            rs.timelineStart   = region.timelineStart;
            rs.lengthInSamples = region.lengthInSamples;
            rs.sourceOffset    = region.sourceOffset;
            rs.fadeInSamples   = std::max ((std::int64_t) 0, region.fadeInSamples);
            rs.fadeOutSamples  = std::max ((std::int64_t) 0, region.fadeOutSamples);
            rs.fadeInShape     = region.fadeInShape;
            rs.fadeOutShape    = region.fadeOutShape;
            rs.numChannels     = std::clamp (region.numChannels, 1, 2);
            // Convert dB once on the message thread; the audio loop
            // multiplies by the linear factor per sample. Clamp the
            // dB at extreme values to avoid wild values from a hand-
            // edited session.json producing audible clip on first
            // play. The Alt-drag clamps tighter ([-24, +12]) at the UI.
            rs.gainLinear = juce::Decibels::decibelsToGain (
                std::clamp (region.gainDb, -60.0f, 24.0f), -60.0f);
            rs.muted = region.muted;
            // Enforce non-overlap: if fadeIn + fadeOut > length the multiplied
            // ramps produce a gain-notch in the middle. Shrink proportionally
            // so the ramps meet at a single sample instead.
            if (rs.fadeInSamples + rs.fadeOutSamples > rs.lengthInSamples)
            {
                const auto total = rs.fadeInSamples + rs.fadeOutSamples;
                if (total > 0)
                {
                    rs.fadeInSamples = (rs.fadeInSamples * rs.lengthInSamples) / total;
                    rs.fadeOutSamples = rs.lengthInSamples - rs.fadeInSamples;
                }
            }
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
            const std::int64_t aEnd = a.timelineStart + a.lengthInSamples;
            if (aEnd > b.timelineStart)
            {
                const std::int64_t overlap = std::min (
                    aEnd - b.timelineStart,
                    std::min (a.lengthInSamples, b.lengthInSamples));
                a.overlapNextLen = overlap;
                b.overlapPrevLen = overlap;
            }
        }

        if (! stream->regions.empty())
            streams[(size_t) t] = std::move (stream);
    }

    // Pre-cache the loop-start window while the audio thread is guaranteed
    // out (streamsActive still false). Loop points changed while rolling are
    // picked up on the next preparePlayback; until then readSpanForTrack's
    // stale-guard falls back to the plain reader path.
    if (transport != nullptr && transport->isLoopEnabled())
        primeLoopCaches (transport->getLoopStart(), transport->getLoopEnd());

    streamsActive.store (true, std::memory_order_release);
}

void PlaybackEngine::primeLoopCaches (std::int64_t loopStart, std::int64_t loopEnd)
{
    if (loopEnd <= loopStart || loopStart < 0) return;

    for (auto& slot : streams)
    {
        if (slot == nullptr) continue;
        for (auto& rs : slot->regions)
        {
            rs.loopCacheTimelineStart = -1;
            rs.loopCacheLen           = 0;

            const std::int64_t regionEnd  = rs.timelineStart + rs.lengthInSamples;
            const std::int64_t cacheStart = std::max (loopStart, rs.timelineStart);
            const std::int64_t cacheEnd   = std::min (loopStart + (std::int64_t) kLoopCacheSamples,
                                                        regionEnd);
            if (cacheEnd <= cacheStart) continue;

            // A fresh plain reader for the fill: the region's own
            // BufferingAudioReader would return silence on a cold window
            // (timeout 0), and bumping its window here would fight the
            // audio thread's forward prefetch.
            std::unique_ptr<juce::AudioFormatReader> fillReader (
                formatManager.createReaderFor (rs.sourceFile));
            if (fillReader == nullptr) continue;

            const int len = (int) (cacheEnd - cacheStart);
            const bool stereo = (rs.numChannels == 2);
            juce::AudioBuffer<float> tmp (2, len);
            tmp.clear();
            fillReader->read (&tmp, 0, len,
                               rs.sourceOffset + (cacheStart - rs.timelineStart),
                               true, stereo);

            rs.loopCacheL.assign (tmp.getReadPointer (0), tmp.getReadPointer (0) + len);
            if (stereo)
                rs.loopCacheR.assign (tmp.getReadPointer (1), tmp.getReadPointer (1) + len);
            else
                rs.loopCacheR.clear();
            rs.loopCacheTimelineStart = cacheStart;
            rs.loopCacheLen           = len;
        }
    }
}

void PlaybackEngine::stopPlayback()
{
    streamsActive.store (false, std::memory_order_release);

    // Drain in-flight readForTrack calls before destroying the readers.
    // The audio callback latches the transport state once per block, so
    // it can still be mid-sum when the message thread gets here. Time-
    // bounded drain (a fixed yield count can elapse in microseconds on a
    // contended box - less than one legitimate callback - and bail
    // spuriously). If the audio thread is genuinely stuck past the
    // deadline, leave the streams allocated - streamsActive stays false
    // so no new reads start, and the next stopPlayback retries the
    // drain. Leak beats UAF.
    constexpr auto kDrainTimeout = std::chrono::milliseconds (200);
    const auto drainDeadline = std::chrono::steady_clock::now() + kDrainTimeout;
    while (audioInFlight.load (std::memory_order_acquire) > 0)
    {
        if (std::chrono::steady_clock::now() > drainDeadline)
        {
            std::fprintf (stderr,
                          "[Dusk Studio/PlaybackEngine] stopPlayback: audioInFlight=%d "
                          "after %lld ms; BAILING teardown to avoid UAF. Streams "
                          "leak until the next stopPlayback drains.\n",
                          audioInFlight.load (std::memory_order_relaxed),
                          (long long) kDrainTimeout.count());
            return;
        }
        std::this_thread::yield();
    }

    for (auto& s : streams) s.reset();
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
    if (! streamsActive.load (std::memory_order_acquire)) return;

    auto& slot = streams[(size_t) trackIndex];
    if (slot == nullptr) return;

    const std::int64_t loopLen = loopEnd - loopStart;
    const bool splitAtLoop = loopStart >= 0 && loopLen >= kMinLoopLenForSplit
                              && playheadSamples < loopEnd;
    if (! splitAtLoop)
    {
        readSpanForTrack (*slot, playheadSamples, outL, outR, 0, numSamples);
        return;
    }

    // Loop-aware read: fill the block piecewise, wrapping the read position
    // exactly like the transport's post-block wrap (loopStart + overshoot),
    // so audio at the seam neither bleeds past loopEnd nor skips the loop
    // downbeat. Seams inside this block get a short declick ramp.
    int seamOffsets[8];
    int numSeams = 0;
    int done = 0;
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
        readSpanForTrack (*slot, pos, outL, outR, done, span);
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
            const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi
                                                     * (float) i / (float) kLoopSeamFade);
            outL[idx] *= 1.0f - g;
            if (outR != nullptr) outR[idx] *= 1.0f - g;
        }
        for (int i = 0; i < kLoopSeamFade; ++i)
        {
            const int idx = seam + i;
            if (idx >= numSamples) break;
            const float g = 0.5f - 0.5f * std::cos (juce::MathConstants<float>::pi
                                                     * (float) i / (float) kLoopSeamFade);
            outL[idx] *= g;
            if (outR != nullptr) outR[idx] *= g;
        }
    }
}

void PlaybackEngine::readSpanForTrack (PerTrackStream& slotRef,
                                       std::int64_t playheadSamples,
                                       float* outL,
                                       float* outR,
                                       int outBase,
                                       int numSamples) noexcept
{
    outL += outBase;
    if (outR != nullptr) outR += outBase;

    const std::int64_t blockEnd = playheadSamples + numSamples;

    for (auto& r : slotRef.regions)
    {
        if (r.reader == nullptr) continue;
        if (r.muted) continue;

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
        jassert (withinSamples <= readScratch.getNumSamples());
        if (withinSamples > readScratch.getNumSamples()) continue;

        const std::int64_t readStart = r.sourceOffset + (firstWithin - r.timelineStart);
        // For mono regions, read L only. For stereo, read both. The
        // BufferingAudioReader's read(useLeft, useRight) flags do the
        // right thing on either side; we always have a 2-channel
        // readScratch so the call is safe regardless.
        const bool readStereo = (r.numChannels == 2);
        r.reader->read (&readScratch, 0, withinSamples, readStart,
                         /*useLeftChan*/ true,
                         /*useRightChan*/ readStereo);

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
            std::memcpy (readScratch.getWritePointer (0) + dstOff,
                          r.loopCacheL.data() + srcOff, sizeof (float) * (size_t) n);
            if (readStereo && ! r.loopCacheR.empty())
                std::memcpy (readScratch.getWritePointer (1) + dstOff,
                              r.loopCacheR.data() + srcOff, sizeof (float) * (size_t) n);
        }

        // Apply fade-in / fade-out envelope in scratch, then SUM (instead
        // of REPLACE) into the output buffer(s). Summing lets two regions
        // overlap during a crossfade window. Mono regions duplicate the
        // L channel into outR (when outR is non-null) so the strip's
        // stereo path sees a center-panned signal.
        //
        // Effective fade = max(explicit, implicit overlap). Shape uses the
        // user's pick when the explicit length wins, EqualPower otherwise
        // so two adjacent regions sum to constant power across the overlap.
        const std::int64_t explicitIn  = r.fadeInSamples;
        const std::int64_t explicitOut = r.fadeOutSamples;
        const std::int64_t implicitIn  = r.overlapPrevLen;
        const std::int64_t implicitOut = r.overlapNextLen;
        const std::int64_t fadeIn   = std::max (explicitIn,  implicitIn);
        const std::int64_t fadeOut  = std::max (explicitOut, implicitOut);
        const FadeShape fadeInShape  = (explicitIn  >= implicitIn)
                                         ? r.fadeInShape  : FadeShape::EqualPower;
        const FadeShape fadeOutShape = (explicitOut >= implicitOut)
                                         ? r.fadeOutShape : FadeShape::EqualPower;
        const std::int64_t regionStart = r.timelineStart;
        const float fadeInDenom  = (fadeIn  > 0) ? (float) fadeIn  : 1.0f;
        const float fadeOutDenom = (fadeOut > 0) ? (float) fadeOut : 1.0f;
        const auto* srcL = readScratch.getReadPointer (0);
        const auto* srcR = readStereo ? readScratch.getReadPointer (1) : srcL;
        const float regionGain = r.gainLinear;
        for (int i = 0; i < withinSamples; ++i)
        {
            const std::int64_t timelineSample = firstWithin + i;
            float gain = regionGain;
            if (fadeIn > 0)
            {
                const std::int64_t inPos = timelineSample - regionStart;
                if (inPos < fadeIn)
                    gain *= applyFadeShape ((float) inPos / fadeInDenom, fadeInShape);
            }
            if (fadeOut > 0)
            {
                const std::int64_t outPos = regionEnd - timelineSample;
                if (outPos < fadeOut)
                    gain *= applyFadeShape ((float) outPos / fadeOutDenom, fadeOutShape);
            }
            outL[outOffset + i] += srcL[i] * gain;
            if (outR != nullptr)
                outR[outOffset + i] += srcR[i] * gain;
        }
    }
}
} // namespace duskstudio
