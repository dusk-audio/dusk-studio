#include "BounceEngine.h"
#include <cmath>
#include "AudioEngine.h"
#include "LameMp3Writer.h"
#include "MasteringPlayer.h"
#include "../session/Session.h"

namespace duskstudio
{
// Offline renders force 4× oversampling on the saturating analog stages
// (console / comp / tube / tape) so the printed file is alias-free, while
// realtime monitoring stays at the user's lighter Effect Oversampling setting.
static constexpr int kRenderOversamplingFactor = 4;

BounceEngine::BounceEngine (AudioEngine& e, Session& s,
                              juce::AudioDeviceManager& dm) noexcept
    : juce::Thread ("Dusk Studio bounce"), engine (e), session (s), deviceManager (dm)
{}

BounceEngine::~BounceEngine()
{
    cancel();
    stopThread (5000);
}

std::unique_ptr<juce::AudioFormatWriter>
BounceEngine::makeWriter (std::unique_ptr<juce::FileOutputStream> outStream,
                            juce::String& errOut) const
{
    constexpr unsigned kNumChannels = 2;   // bounce is always stereo

    if (renderFormat == Format::Mp3)
    {
        // LameMp3Writer's base ctor takes ownership of the stream immediately,
        // so hand it the raw pointer up front; on failure the writer's destructor
        // frees it (no double-free with the unique_ptr).
        auto writer = std::make_unique<LameMp3Writer> (outStream.release(),
                                                        renderSampleRate, kNumChannels,
                                                        renderBitrateKbps);
        if (! writer->isOk())
        {
            errOut = "MP3 export is not available - this build has no libmp3lame.";
            return nullptr;
        }
        return writer;
    }

    // WAV: createWriterFor takes ownership of the stream only on success.
    juce::WavAudioFormat wavFormat;
    juce::StringPairArray metadata;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (outStream.get(), renderSampleRate,
                                     kNumChannels, renderWavBitDepth, metadata, 0));
    if (writer == nullptr)
    {
        errOut = "Could not create WAV writer";
        return nullptr;   // outStream still owns + frees the stream
    }
    outStream.release();
    return writer;
}

std::int64_t BounceEngine::computeBounceLength (double sampleRate, double tail) const
{
    // Longest region end across all tracks defines the natural bounce end;
    // tail extends that so reverb/comp/EQ ringouts decay before we cut.
    // MIDI regions and freeze WAVs count too — a virtual-instrument song has
    // no audio regions at all, and MIDI playing past the last audio region
    // must not be truncated.
    std::int64_t maxRegionEnd = 0;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& track = session.track (t);
        for (const auto& r : track.regions)
            maxRegionEnd = juce::jmax (maxRegionEnd, r.timelineStart + r.lengthInSamples);
        for (const auto& mr : track.midiRegions.current())
            maxRegionEnd = juce::jmax (maxRegionEnd, mr.timelineStart + mr.lengthInSamples);
        if (track.frozen.load (std::memory_order_relaxed)
            && track.frozenRegion.lengthInSamples > 0)
            maxRegionEnd = juce::jmax (maxRegionEnd,
                                        track.frozenRegion.timelineStart
                                            + track.frozenRegion.lengthInSamples);
    }
    if (maxRegionEnd <= 0) maxRegionEnd = (std::int64_t) (sampleRate * 1.0);  // 1 s of silence
    return maxRegionEnd + (std::int64_t) (sampleRate * tail);
}

bool BounceEngine::start (const juce::File& outFile, double sr, int bs, double tail,
                            Mode mode, Format format, int mp3BitrateKbps,
                            int wavBitDepth)
{
    if (rendering.load (std::memory_order_relaxed)) return false;

    // FreezeTrack is reachable only through startFreeze(), which initialises
    // freezeTrackIndex / freezeLenSamples. Entering the generic path would run a
    // default-initialised (-1 / 0) freeze render.
    if (mode == Mode::FreezeTrack)
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "FreezeTrack render must go through startFreeze()";
        return false;
    }

    outputFile  = outFile;
    renderSampleRate = (sr > 0.0) ? sr : engine.getCurrentSampleRate();
    if (renderSampleRate <= 0.0) renderSampleRate = 48000.0;
    renderBlockSize = juce::jmax (64, bs);
    tailSeconds     = tail;
    renderMode      = mode;
    // Stems must stay WAV: MP3 encoder delay + frame padding shift each stem by
    // a different amount and change its length, breaking the sample-accurate
    // alignment stems need for re-import. Force WAV at the engine boundary
    // regardless of what the caller requested.
    renderFormat    = (mode == Mode::Stems) ? Format::Wav : format;
    renderBitrateKbps = mp3BitrateKbps;
    // Stems keep 24-bit regardless: they exist for re-import, not delivery.
    renderWavBitDepth = (mode != Mode::Stems && wavBitDepth == 16) ? 16 : 24;

    if (renderMode == Mode::MasterMix || renderMode == Mode::Stems)
    {
        totalSamples = computeBounceLength (renderSampleRate, tailSeconds);
    }
    else
    {
        // Mastering: render length = player's loaded file length + tail.
        // The player's length is in SOURCE samples; the render loop counts
        // DEVICE-rate samples, so scale when the rates differ (the player
        // resamples in process()).
        const auto playerLen = engine.getMasteringPlayer().getLengthSamples();
        const auto sourceSr  = engine.getMasteringPlayer().getSourceSampleRate();
        const auto playerLenAtRender = sourceSr > 0.0
            ? (std::int64_t) std::llround ((double) playerLen * renderSampleRate / sourceSr)
            : playerLen;
        totalSamples = playerLenAtRender + (std::int64_t) (renderSampleRate * tailSeconds);
        if (totalSamples <= 0) return false;  // no file loaded
    }

    // Pre-compute stem count so the UI's "track N of M" label has its
    // total available immediately (otherwise the dialog flashes 0 until
    // the worker thread enters its loop). Tracks render only when they
    // have audio / MIDI regions or are armed for recording — empty
    // unarmed tracks would write silent stems.
    int stems = 0;
    if (renderMode == Mode::Stems)
    {
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const auto& tr = session.track (t);
            const bool hasContent = ! tr.regions.empty()
                                  || ! tr.midiRegions.current().empty();
            const bool armed = tr.recordArmed.load (std::memory_order_relaxed);
            if (hasContent || armed) ++stems;
        }
        if (stems == 0)
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = "No tracks with content or armed for recording";
            return false;
        }
    }
    totalStemsToRender.store (stems, std::memory_order_relaxed);
    currentStemIndex  .store (0,     std::memory_order_relaxed);

    cancelRequested.store (false, std::memory_order_relaxed);
    progress.store (0.0f, std::memory_order_relaxed);
    renderedSamples.store (0, std::memory_order_relaxed);
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError.clear();
    }
    rendering.store (true, std::memory_order_relaxed);

    startThread();
    return true;
}

void BounceEngine::run()
{
    if (onStarted) onStarted();

    if (renderMode == Mode::Stems)
    {
        const bool ok = runStemsMode();
        rendering.store (false, std::memory_order_relaxed);
        juce::String errSnapshot;
        {
            const juce::ScopedLock lock (lastErrorLock);
            errSnapshot = lastError;
        }
        if (onFinished) onFinished (ok, errSnapshot);
        return;
    }

    if (renderMode == Mode::FreezeTrack)
    {
        const bool ok = renderFreezeTrack (freezeTrackIndex, outputFile,
                                            freezeLenSamples, renderSampleRate,
                                            renderBlockSize);
        rendering.store (false, std::memory_order_relaxed);
        juce::String errSnapshot;
        {
            const juce::ScopedLock lock (lastErrorLock);
            errSnapshot = lastError;
        }
        if (onFinished) onFinished (ok, errSnapshot);
        return;
    }

    // Open the writer first - failure here means we don't bother touching the
    // engine state.
    std::unique_ptr<juce::FileOutputStream> outStream (outputFile.createOutputStream());
    if (outStream == nullptr)
    {
        juce::String err = "Could not open output file " + outputFile.getFullPathName();
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = err;
        }
        rendering.store (false, std::memory_order_relaxed);
        if (onFinished) onFinished (false, err);
        return;
    }
    outStream->setPosition (0);
    outStream->truncate();

    constexpr int kNumChannels = 2;   // bounce is always stereo
    juce::String writerErr;
    std::unique_ptr<juce::AudioFormatWriter> writer = makeWriter (std::move (outStream), writerErr);
    if (writer == nullptr)
    {
        // Writer failed AFTER we truncated the file above - drop the now-zeroed
        // file so we don't leave a 0-byte output behind (mirrors renderOneStem).
        outputFile.deleteFile();
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = writerErr;
        }
        rendering.store (false, std::memory_order_relaxed);
        if (onFinished) onFinished (false, writerErr);
        return;
    }

    // Detach the engine from the realtime device - we drive its audio
    // callback ourselves at non-realtime pace. State is restored at the
    // bottom of this function regardless of how we exit.
    deviceManager.removeAudioCallback (&engine);
    // Render the saturating stages at 4× so the bounce is alias-free; cleared
    // before the live re-prepare below.
    engine.setRenderOversamplingOverride (kRenderOversamplingFactor);
    engine.prepareForSelfTest (renderSampleRate, renderBlockSize);

    // Synthetic input buffers - silent, since the bounce is master-mix
    // (we render whatever's on the timeline through the channel strips,
    // not live input). 16 input channels matches the engine's expected
    // numInputChannels at full configuration.
    constexpr int kNumIn = 16;
    std::vector<std::vector<float>> inputs (kNumIn,
                                              std::vector<float> ((size_t) renderBlockSize, 0.0f));
    std::vector<const float*> inputPtrs (kNumIn);
    for (int c = 0; c < kNumIn; ++c) inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs (kNumChannels,
                                               std::vector<float> ((size_t) renderBlockSize, 0.0f));
    std::vector<float*> outputPtrs (kNumChannels);
    for (int c = 0; c < kNumChannels; ++c) outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    juce::AudioIODeviceCallbackContext ctx {};

    // Drive the transport / mastering player from sample 0. We don't go
    // through engine.play() because that's RT-safe-message-thread; we
    // mutate engine state directly here on the worker thread, which is
    // safe because the engine is detached from the device.
    auto& transport = engine.getTransport();
    const auto savedTransportState = transport.getState();
    const auto savedPlayhead       = transport.getPlayhead();
    const auto savedStage          = engine.getStage();

    if (renderMode == Mode::MasterMix)
    {
        // Force Mixing/Recording stage so the audio callback runs the
        // track-mix path even if the user happens to be in Mastering.
        engine.setStage (AudioEngine::Stage::Mixing);
        transport.setPlayhead (0);
        transport.setState (Transport::State::Playing);
        engine.getPlaybackEngine().preparePlayback();
        // The in-callback relatch of the master-stage aux PDC only runs while
        // stopped — and this offline drive is Playing from the first block.
        // Latch the targets here (callback is detached) or the render plays
        // wet returns misaligned against the dry mix.
        engine.applyMasterPdcTargetsNow();
    }
    else
    {
        // Mastering chain: the audio callback's mastering branch reads
        // from MasteringPlayer, so seek the player to 0 + start it.
        engine.setStage (AudioEngine::Stage::Mastering);
        engine.getMasteringPlayer().setPlayhead (0);
        engine.getMasteringPlayer().play();
    }

    // PDC lead-in: cross-track compensation delays the master mix by the
    // deepest track latency, and the master-stage aux PDC delays it again by
    // the deepest aux-lane latency. Render that many extra samples and discard
    // them up front so the file isn't shifted. The MasteringChain path bypasses
    // the channel strips and aux lanes, so it carries no lead-in.
    const std::int64_t leadIn = (renderMode == Mode::MasteringChain)
                                 ? 0
                                 : (std::int64_t) engine.getAggregatePdcLatencySamples()
                                     + (std::int64_t) engine.getMasterDryPdcTargetSamples();
    const std::int64_t toRender = totalSamples + leadIn;

    std::int64_t done    = 0;   // samples processed through the engine
    std::int64_t written = 0;   // samples committed to the file
    std::int64_t dropped = 0;   // lead-in samples discarded
    bool succeeded = true;
    const bool dither16 = renderFormat == Format::Wav && renderWavBitDepth == 16;
    juce::Random ditherRng;
    while (done < toRender && ! cancelRequested.load (std::memory_order_relaxed))
    {
        const int remaining = (int) juce::jmin ((std::int64_t) renderBlockSize,
                                                  toRender - done);

        // Reset outputs each block.
        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);

        engine.audioDeviceIOCallbackWithContext (inputPtrs.data(), kNumIn,
                                                   outputPtrs.data(), kNumChannels,
                                                   remaining, ctx);

        // Drop the leading PDC samples, then write the rest.
        int writeStart = 0;
        if (dropped < leadIn)
        {
            writeStart = (int) juce::jmin ((std::int64_t) remaining, leadIn - dropped);
            dropped += writeStart;
        }
        const int writeCount = remaining - writeStart;
        if (writeCount > 0)
        {
            std::array<float*, kNumChannels> offPtrs {};
            for (int c = 0; c < kNumChannels; ++c) offPtrs[(size_t) c] = outputPtrs[(size_t) c] + writeStart;

            if (dither16)
            {
                // TPDF at ±1 LSB ahead of the 16-bit truncation: decorrelates
                // the quantisation error so fades decay into noise instead of
                // distortion.
                constexpr float lsb = 1.0f / 32768.0f;
                for (int c = 0; c < kNumChannels; ++c)
                {
                    auto* p = offPtrs[(size_t) c];
                    for (int i = 0; i < writeCount; ++i)
                        p[i] += (ditherRng.nextFloat() - ditherRng.nextFloat()) * lsb;
                }
            }
            if (! writer->writeFromFloatArrays (offPtrs.data(), kNumChannels, writeCount))
            {
                const juce::ScopedLock lock (lastErrorLock);
                lastError = "Writer failed mid-render at " + juce::String (written) + " samples";
                succeeded = false;
                break;
            }
            written += writeCount;
        }

        done += remaining;
        renderedSamples.store (written, std::memory_order_relaxed);
        const float p = (float) ((double) written / (double) totalSamples);
        progress.store (p, std::memory_order_relaxed);
        if (onProgressUpdated) onProgressUpdated (p);
    }

    if (cancelRequested.load (std::memory_order_relaxed))
    {
        succeeded = false;
        const juce::ScopedLock lock (lastErrorLock);
        lastError = kCancelledError;
    }

    // MP3: the final frames only hit the stream at flush. Flush explicitly so
    // a disk-full there fails the bounce instead of reporting success.
    if (auto* mp3 = dynamic_cast<LameMp3Writer*> (writer.get());
        mp3 != nullptr && ! mp3->finalize() && succeeded)
    {
        succeeded = false;
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "MP3 encoder flush failed (disk full?)";
    }

    writer.reset();  // flush + close
    // A cancelled or failed render must not leave a truncated file where the
    // user's previous good bounce was — stems and freeze already do this.
    if (! succeeded)
        outputFile.deleteFile();

    // Restore engine state. Each mode reverses what it set up above;
    // anything outside the mode's purview stays untouched.
    if (renderMode == Mode::MasterMix)
    {
        engine.getPlaybackEngine().stopPlayback();
        transport.setState (savedTransportState);
        transport.setPlayhead (savedPlayhead);
    }
    else
    {
        engine.getMasteringPlayer().stop();
        engine.getMasteringPlayer().setPlayhead (0);
    }
    engine.setStage (savedStage);
    // Back to the user's realtime oversampling before the live re-prepare.
    engine.setRenderOversamplingOverride (0);
    deviceManager.addAudioCallback (&engine);

    rendering.store (false, std::memory_order_relaxed);
    juce::String errSnapshot;
    {
        const juce::ScopedLock lock (lastErrorLock);
        errSnapshot = lastError;
    }
    if (onFinished) onFinished (succeeded, errSnapshot);
}

bool BounceEngine::renderOneStem (const juce::File& outFile,
                                    std::int64_t lenSamples,
                                    float stemFractionStart,
                                    float stemFractionWidth)
{
    std::unique_ptr<juce::FileOutputStream> outStream (outFile.createOutputStream());
    if (outStream == nullptr)
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "Could not open stem output file " + outFile.getFullPathName();
        return false;
    }
    outStream->setPosition (0);
    outStream->truncate();

    constexpr int kNumChannels = 2;
    juce::String writerErr;
    std::unique_ptr<juce::AudioFormatWriter> writer = makeWriter (std::move (outStream), writerErr);
    if (writer == nullptr)
    {
        // Writer failed AFTER we truncated the file above - drop the now-zeroed
        // file so we don't leave a 0-byte stem on disk.
        outFile.deleteFile();
        const juce::ScopedLock lock (lastErrorLock);
        lastError = writerErr + " (stem " + outFile.getFileName() + ")";
        return false;
    }

    constexpr int kNumIn = 16;
    std::vector<std::vector<float>> inputs (kNumIn,
                                              std::vector<float> ((size_t) renderBlockSize, 0.0f));
    std::vector<const float*> inputPtrs (kNumIn);
    for (int c = 0; c < kNumIn; ++c) inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs (kNumChannels,
                                               std::vector<float> ((size_t) renderBlockSize, 0.0f));
    std::vector<float*> outputPtrs (kNumChannels);
    for (int c = 0; c < kNumChannels; ++c) outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    juce::AudioIODeviceCallbackContext ctx {};

    auto& transport = engine.getTransport();
    transport.setPlayhead (0);
    transport.setState (Transport::State::Playing);
    engine.getPlaybackEngine().preparePlayback();

    // Same PDC lead-in trim as the master render: each stem runs the full strip
    // path, so it's delayed by the deepest track latency plus the
    // master-stage aux PDC. Drop those leading samples so all stems stay
    // mutually aligned and start at sample 0.
    const std::int64_t leadIn  = (std::int64_t) engine.getAggregatePdcLatencySamples()
                                  + (std::int64_t) engine.getMasterDryPdcTargetSamples();
    const std::int64_t toRender = lenSamples + leadIn;

    std::int64_t done    = 0;
    std::int64_t written = 0;
    std::int64_t dropped = 0;
    bool ok = true;
    while (done < toRender && ! cancelRequested.load (std::memory_order_relaxed))
    {
        const int remaining = (int) juce::jmin ((std::int64_t) renderBlockSize,
                                                  toRender - done);

        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);

        engine.audioDeviceIOCallbackWithContext (inputPtrs.data(), kNumIn,
                                                   outputPtrs.data(), kNumChannels,
                                                   remaining, ctx);

        int writeStart = 0;
        if (dropped < leadIn)
        {
            writeStart = (int) juce::jmin ((std::int64_t) remaining, leadIn - dropped);
            dropped += writeStart;
        }
        const int writeCount = remaining - writeStart;
        std::array<float*, kNumChannels> offPtrs {};
        for (int c = 0; c < kNumChannels; ++c) offPtrs[(size_t) c] = outputPtrs[(size_t) c] + writeStart;
        if (writeCount > 0 && ! writer->writeFromFloatArrays (offPtrs.data(), kNumChannels, writeCount))
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = "Writer failed mid-stem at "
                        + juce::String (written) + " samples in "
                        + outFile.getFileName();
            ok = false;
            break;
        }
        written += juce::jmax (0, writeCount);

        done += remaining;
        renderedSamples.store (written, std::memory_order_relaxed);
        const float withinStem = (float) ((double) written / (double) lenSamples);
        const float overall    = stemFractionStart + withinStem * stemFractionWidth;
        progress.store (overall, std::memory_order_relaxed);
        if (onProgressUpdated) onProgressUpdated (overall);
    }

    engine.getPlaybackEngine().stopPlayback();
    writer.reset();  // flush + close

    if (cancelRequested.load (std::memory_order_relaxed) || ! ok)
    {
        // Drop the partial stem — a half-rendered file is more
        // confusing than no file when the user cancels or the writer
        // fails mid-render. Delete is performed AFTER writer.reset()
        // above so the file handle is closed before we unlink.
        outFile.deleteFile();
        return false;
    }
    return ok;
}

bool BounceEngine::runStemsMode()
{
    // Build the list of tracks to render. Mirrors the predicate in
    // start()'s pre-count so we render exactly the same set.
    std::vector<int> tracksToRender;
    tracksToRender.reserve ((size_t) Session::kNumTracks);
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& tr = session.track (t);
        const bool hasContent = ! tr.regions.empty()
                              || ! tr.midiRegions.current().empty();
        const bool armed = tr.recordArmed.load (std::memory_order_relaxed);
        if (hasContent || armed) tracksToRender.push_back (t);
    }
    if (tracksToRender.empty())
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "No tracks with content or armed for recording";
        return false;
    }

    // Snapshot solo state so we can restore EXACTLY what the user had
    // before the bounce, regardless of how we exit.
    std::array<bool, (size_t) Session::kNumTracks> savedSolo {};
    for (int t = 0; t < Session::kNumTracks; ++t)
        savedSolo[(size_t) t] = session.track (t).strip.solo
                                  .load (std::memory_order_relaxed);

    // Detach engine + prepare for offline rate just once. Each stem
    // re-uses the same prepared state.
    deviceManager.removeAudioCallback (&engine);
    // Stems render through the full strip path too, so oversample them 4× for
    // alias-free output; cleared before the live re-prepare below.
    engine.setRenderOversamplingOverride (kRenderOversamplingFactor);
    engine.prepareForSelfTest (renderSampleRate, renderBlockSize);

    auto& transport = engine.getTransport();
    const auto savedTransportState = transport.getState();
    const auto savedPlayhead       = transport.getPlayhead();
    const auto savedStage          = engine.getStage();
    engine.setStage (AudioEngine::Stage::Mixing);
    // Same reason as the master-mix path: the stem drive runs Playing from
    // block 0, so the in-callback (stopped-only) relatch never engages the
    // master-stage aux PDC — latch it while the callback is detached.
    engine.applyMasterPdcTargetsNow();

    bool succeeded = true;
    const int  numStems = (int) tracksToRender.size();
    const float widthPerStem = (numStems > 0) ? (1.0f / (float) numStems) : 1.0f;

    for (int i = 0; i < numStems; ++i)
    {
        if (cancelRequested.load (std::memory_order_relaxed)) { succeeded = false; break; }

        const int trackIdx = tracksToRender[(size_t) i];

        // Solo exclusively: clear every track's solo, then engage the
        // current one. Using setTrackSoloed keeps soloTrackCount in
        // sync so the audio thread's anyTrackSoloed check fires right
        // away on the next block.
        for (int t = 0; t < Session::kNumTracks; ++t)
            if (t != trackIdx) session.setTrackSoloed (t, false);
        session.setTrackSoloed (trackIdx, true);

        currentStemIndex.store (i + 1, std::memory_order_relaxed);

        const auto outFile = stemOutputFile (outputFile, trackIdx,
                                               session.track (trackIdx).name);
        if (! renderOneStem (outFile, totalSamples,
                             (float) i * widthPerStem,
                             widthPerStem))
        {
            succeeded = false;
            break;
        }
    }

    // Restore everything — solo, transport, stage, device callback.
    for (int t = 0; t < Session::kNumTracks; ++t)
        session.setTrackSoloed (t, savedSolo[(size_t) t]);
    transport.setState (savedTransportState);
    transport.setPlayhead (savedPlayhead);
    engine.setStage (savedStage);
    // Back to the user's realtime oversampling before the live re-prepare.
    engine.setRenderOversamplingOverride (0);
    deviceManager.addAudioCallback (&engine);

    if (cancelRequested.load (std::memory_order_relaxed))
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = kCancelledError;
        succeeded = false;
    }
    return succeeded;
}

bool BounceEngine::startFreeze (int trackIndex, const juce::File& outFile,
                                std::int64_t lenSamples, double sampleRate, int blockSize)
{
    if (rendering.load (std::memory_order_relaxed))
        return false;

    outputFile       = outFile;
    renderSampleRate = (sampleRate > 0.0) ? sampleRate : engine.getCurrentSampleRate();
    if (renderSampleRate <= 0.0) renderSampleRate = 48000.0;
    renderBlockSize  = juce::jmax (16, blockSize);
    renderMode       = Mode::FreezeTrack;
    renderFormat     = Format::Wav;
    freezeTrackIndex = trackIndex;
    freezeLenSamples = lenSamples;

    cancelRequested.store (false, std::memory_order_relaxed);
    progress.store (0.0f, std::memory_order_relaxed);
    renderedSamples.store (0, std::memory_order_relaxed);
    { const juce::ScopedLock lock (lastErrorLock); lastError.clear(); }
    rendering.store (true, std::memory_order_relaxed);

    // run() owns clearing `rendering`, so if the thread never starts we must
    // clear it here — otherwise isRendering() stays true forever and the dialog
    // wedges. Routes the caller into FreezeDialog's failBeforeStart path.
    if (! startThread())   // → run() → renderFreezeTrack on the worker thread
    {
        rendering.store (false, std::memory_order_relaxed);
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "Could not start the freeze render thread";
        return false;
    }
    return true;
}

bool BounceEngine::renderFreezeTrack (int trackIndex, const juce::File& outFile,
                                      std::int64_t lenSamples, double sampleRate, int blockSize)
{
    // Clear any stale error from a prior render at the API boundary: a direct
    // synchronous call (test harness) doesn't go through startFreeze (which clears
    // it), so getLastError() must reflect THIS render's outcome, not the last one.
    { const juce::ScopedLock lock (lastErrorLock); lastError.clear(); }

    // Caller (startFreeze / run, or a direct synchronous test) owns the
    // rendering + cancelRequested flags; this method only does the render.
    if (trackIndex < 0 || trackIndex >= Session::kNumTracks || lenSamples <= 0)
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "Invalid freeze render request";
        return false;
    }

    renderSampleRate = (sampleRate > 0.0) ? sampleRate : renderSampleRate;
    renderBlockSize  = juce::jmax (16, blockSize);
    renderFormat     = Format::Wav;   // freeze is always WAV (sample-accurate re-import)

    // Open the writer first — a failure here means we never touch engine state.
    std::unique_ptr<juce::FileOutputStream> outStream (outFile.createOutputStream());
    if (outStream == nullptr)
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "Could not open freeze file " + outFile.getFullPathName();
        return false;
    }
    outStream->setPosition (0);
    outStream->truncate();

    constexpr int kNumChannels = 2;
    juce::String writerErr;
    std::unique_ptr<juce::AudioFormatWriter> writer = makeWriter (std::move (outStream), writerErr);
    if (writer == nullptr)
    {
        outFile.deleteFile();   // drop the 0-byte file we just truncated
        const juce::ScopedLock lock (lastErrorLock);
        lastError = writerErr;
        return false;
    }

    // Detach + offline-prepare, exactly like the stem path.
    deviceManager.removeAudioCallback (&engine);
    engine.setRenderOversamplingOverride (kRenderOversamplingFactor);
    engine.prepareForSelfTest (renderSampleRate, renderBlockSize);

    constexpr int kNumIn = 16;
    std::vector<std::vector<float>> inputs (kNumIn,
                                              std::vector<float> ((size_t) renderBlockSize, 0.0f));
    std::vector<const float*> inputPtrs (kNumIn);
    for (int c = 0; c < kNumIn; ++c) inputPtrs[(size_t) c] = inputs[(size_t) c].data();

    std::vector<std::vector<float>> outputs (kNumChannels,
                                               std::vector<float> ((size_t) renderBlockSize, 0.0f));
    std::vector<float*> outputPtrs (kNumChannels);
    for (int c = 0; c < kNumChannels; ++c) outputPtrs[(size_t) c] = outputs[(size_t) c].data();

    // Pre-fader capture scratch the target strip fills each block. Cleared
    // per-block so a strip early-return writes silence, never a stale block.
    std::vector<float> capL ((size_t) renderBlockSize, 0.0f);
    std::vector<float> capR ((size_t) renderBlockSize, 0.0f);

    juce::AudioIODeviceCallbackContext ctx {};

    auto& transport = engine.getTransport();
    const auto savedTransportState = transport.getState();
    const auto savedPlayhead       = transport.getPlayhead();
    const auto savedStage          = engine.getStage();

    std::array<bool, (size_t) Session::kNumTracks> savedSolo {};
    for (int t = 0; t < Session::kNumTracks; ++t)
        savedSolo[(size_t) t] = session.track (t).strip.solo.load (std::memory_order_relaxed);

    engine.setStage (AudioEngine::Stage::Mixing);
    // Solo the target exclusively. The capture tap is pre-gate so this isn't
    // strictly required, but it keeps the render deterministic and skips the
    // other tracks' mix contribution.
    for (int t = 0; t < Session::kNumTracks; ++t)
        if (t != trackIndex) session.setTrackSoloed (t, false);
    session.setTrackSoloed (trackIndex, true);

    transport.setPlayhead (0);
    transport.setState (Transport::State::Playing);
    engine.getPlaybackEngine().preparePlayback();

    engine.getChannelStrip (trackIndex).setFreezeCapture (capL.data(), capR.data());

    // The capture tap is post-insert, so an audio track's latent insert
    // (lookahead comp, linear-phase EQ) delays the captured signal by its own
    // latency — and frozen playback reports 0 to PDC, so that delay would be
    // baked in and replayed late. Render that many extra samples and drop them
    // from the head. MIDI instruments are excluded: the scheduler pre-shifts
    // their events by the plugin latency, so their capture is already aligned.
    const bool isMidiTrack =
        session.track (trackIndex).mode.load (std::memory_order_relaxed)
            == (int) Track::Mode::Midi;
    const std::int64_t leadIn = isMidiTrack ? 0
        : (std::int64_t) engine.getChannelStrip (trackIndex)
                              .getPluginSlot().getLatencySamples();
    const std::int64_t toRender = lenSamples + leadIn;

    std::int64_t done = 0, written = 0, dropped = 0;
    bool ok = true;
    while (done < toRender && ! cancelRequested.load (std::memory_order_relaxed))
    {
        const int remaining = (int) juce::jmin ((std::int64_t) renderBlockSize, toRender - done);
        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);
        std::fill (capL.begin(), capL.end(), 0.0f);
        std::fill (capR.begin(), capR.end(), 0.0f);

        engine.audioDeviceIOCallbackWithContext (inputPtrs.data(), kNumIn,
                                                   outputPtrs.data(), kNumChannels,
                                                   remaining, ctx);

        int writeStart = 0;
        if (dropped < leadIn)
        {
            writeStart = (int) juce::jmin ((std::int64_t) remaining, leadIn - dropped);
            dropped += writeStart;
        }
        const int writeCount = remaining - writeStart;
        if (writeCount > 0)
        {
            float* capPtrs[kNumChannels] = { capL.data() + writeStart, capR.data() + writeStart };
            if (! writer->writeFromFloatArrays (capPtrs, kNumChannels, writeCount))
            {
                const juce::ScopedLock lock (lastErrorLock);
                lastError = "Writer failed mid-freeze at " + juce::String (written) + " samples";
                ok = false;
                break;
            }
            written += writeCount;
        }

        done += remaining;
        renderedSamples.store (written, std::memory_order_relaxed);
        const float p = (float) ((double) written / (double) lenSamples);
        progress.store (juce::jlimit (0.0f, 1.0f, p), std::memory_order_relaxed);
        if (onProgressUpdated) onProgressUpdated (progress.load (std::memory_order_relaxed));
    }

    // Drop the capture tap BEFORE re-preparing / re-attaching the live device.
    engine.getChannelStrip (trackIndex).setFreezeCapture (nullptr, nullptr);
    engine.getPlaybackEngine().stopPlayback();
    writer.reset();   // flush + close before any deleteFile

    // Restore everything — solo, transport, stage, oversampling, device callback.
    for (int t = 0; t < Session::kNumTracks; ++t)
        session.setTrackSoloed (t, savedSolo[(size_t) t]);
    transport.setState (savedTransportState);
    transport.setPlayhead (savedPlayhead);
    engine.setStage (savedStage);
    engine.setRenderOversamplingOverride (0);
    deviceManager.addAudioCallback (&engine);

    if (! ok || cancelRequested.load (std::memory_order_relaxed))
    {
        outFile.deleteFile();
        if (cancelRequested.load (std::memory_order_relaxed))
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = kCancelledError;
        }
        return false;
    }
    return true;
}
} // namespace duskstudio
