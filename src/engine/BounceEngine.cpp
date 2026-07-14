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

namespace
{
// Latches the engine's offline-render flag for the lifetime of a render loop so
// the audio callback suppresses live-MIDI pulls (see AudioEngine's play-along
// overlay). RAII so an early-out or exception still clears it.
struct ScopedOfflineRender
{
    explicit ScopedOfflineRender (AudioEngine& e) noexcept : engine (e)
        { engine.setOfflineRenderActive (true); }
    ~ScopedOfflineRender() { engine.setOfflineRenderActive (false); }
    AudioEngine& engine;
};
} // namespace

BounceEngine::BounceEngine (AudioEngine& e, Session& s,
                              juce::AudioDeviceManager& dm) noexcept
    : juce::Thread ("Dusk Studio bounce"), engine (e), session (s), deviceManager (dm)
{}

BounceEngine::~BounceEngine()
{
    cancel();
    stopThread (5000);
}

bool BounceEngine::runOnMessageThread (std::function<void()> fn)
{
    auto* mm = juce::MessageManager::getInstanceWithoutCreating();
    if (mm == nullptr || mm->isThisTheMessageThread())
    {
        fn();
        return true;
    }

    // The queued lambda owns the sync state and fn. fn captures this, so an
    // abandoned message must never run it: after ~BounceEngine the capture is
    // dangling. The abandoned flag is decisive because the queued lambda and
    // every ~BounceEngine path run on the message thread (serialized), and the
    // worker's store below is published to the message thread by the dtor's
    // stopThread join.
    struct Sync { juce::WaitableEvent done; std::atomic<bool> abandoned { false }; };
    auto sync = std::make_shared<Sync>();
    const bool posted = juce::MessageManager::callAsync ([sync, fn = std::move (fn)]
    {
        if (! sync->abandoned.load (std::memory_order_acquire))
            fn();
        sync->done.signal();
    });

    if (! posted)
    {
        // The queue rejected the post (message manager quitting): the lambda
        // will never run and never signal, so don't wait on it. fn did not run.
        sync->abandoned.store (true, std::memory_order_release);
        return false;
    }

    while (! sync->done.wait (50))
        if (threadShouldExit())
        {
            // App shutdown: don't block ~BounceEngine's stopThread join.
            sync->abandoned.store (true, std::memory_order_release);
            return false;
        }
    return true;
}

std::unique_ptr<juce::AudioFormatWriter>
BounceEngine::makeWriter (std::unique_ptr<juce::FileOutputStream> outStream,
                            std::string& errOut) const
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
    // MIDI regions and freeze WAVs count too - a virtual-instrument song has
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
                            int wavBitDepth, bool realtimeCapture)
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

    renderRealtime = realtimeCapture
                       && (mode == Mode::MasterMix || mode == Mode::Stems);
    if (renderRealtime)
    {
        // The live callback IS the render clock: it runs at the device rate,
        // and a rolling transport would print from wherever it happens to be.
        sr = 0.0;
        if (! engine.getTransport().isStopped())
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = "Stop the transport before a realtime bounce";
            return false;
        }
    }

    outputFile  = outFile;
    renderSampleRate = (sr > 0.0) ? sr : engine.getCurrentSampleRate();
    if (renderSampleRate <= 0.0) renderSampleRate = 48000.0;
    renderBlockSize = juce::jmax (64, bs);
    tailSeconds     = tail;
    renderMode      = mode;
    // Stems must stay WAV: MP3 encoder delay + frame padding shift each stem by
    // a different amount and change its length, breaking the sample-accurate
    // alignment stems need for re-import. Realtime is WAV too: the disk path
    // is ThreadedWriter, which owns its writer outright, so the MP3 encoder's
    // explicit finalize flush has no place to run. Force WAV at the engine
    // boundary regardless of what the caller requested.
    renderFormat    = (mode == Mode::Stems || renderRealtime) ? Format::Wav : format;
    renderBitrateKbps = mp3BitrateKbps;
    // Stems keep 24-bit regardless: they exist for re-import, not delivery.
    // Realtime does too: its disk path streams through ThreadedWriter with no
    // dither stage, so a 16-bit realtime file would truncate undithered (the
    // offline loop is where the TPDF dither lives).
    renderWavBitDepth = (mode != Mode::Stems && ! renderRealtime && wavBitDepth == 16)
                          ? 16 : 24;

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

    // Pre-compute the stem-file count so the UI's "N stems" label has its
    // total available immediately (otherwise the dialog flashes 0 until
    // the worker thread enters its loop).
    int stems = 0;
    if (renderMode == Mode::Stems)
    {
        stems = (int) collectStemTargets (session, outputFile).size();
        if (stems == 0)
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = "No tracks with content or armed for recording";
            return false;
        }
    }
    totalStemsToRender.store (stems, std::memory_order_relaxed);

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

    if (renderRealtime)
    {
        const bool ok = runRealtimeMode();
        rendering.store (false, std::memory_order_relaxed);
        std::string errSnapshot;
        {
            const juce::ScopedLock lock (lastErrorLock);
            errSnapshot = lastError;
        }
        if (onFinished) onFinished (ok, errSnapshot);
        return;
    }

    if (renderMode == Mode::Stems)
    {
        const bool ok = runStemsMode();
        rendering.store (false, std::memory_order_relaxed);
        std::string errSnapshot;
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
        std::string errSnapshot;
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
        const std::string err = ("Could not open output file " + outputFile.getFullPathName()).toStdString();
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
    std::string writerErr;
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
    ScopedOfflineRender offlineGuard (engine);
    // Detach + re-prepare on the message thread: prepareForSelfTest reaches every
    // hosted plugin's (de)activate, which the CLAP contract forbids off the main
    // thread. Render the saturating stages at 4× so the bounce is alias-free;
    // cleared before the live re-prepare below.
    if (! runOnMessageThread ([this]
        {
            deviceManager.removeAudioCallback (&engine);
            engine.setRenderOversamplingOverride (kRenderOversamplingFactor);
            engine.prepareForSelfTest (renderSampleRate, renderBlockSize);
        }))
    {
        // Detach/prepare never ran (shutdown or dead message queue). The engine
        // is still live-attached and unprepared for offline - touch no engine
        // state. Drop the partial file and bail.
        writer.reset();
        outputFile.deleteFile();
        rendering.store (false, std::memory_order_relaxed);
        return;
    }

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
        // stopped - and this offline drive is Playing from the first block.
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
                lastError = "Writer failed mid-render at " + std::to_string (written) + " samples";
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
    // user's previous good bounce was - stems and freeze already do this.
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
    // Back to the user's realtime oversampling, then reattach on the message
    // thread - addAudioCallback re-prepares the engine (plugin (de)activate),
    // which must not run on this worker.
    runOnMessageThread ([this]
    {
        engine.setRenderOversamplingOverride (0);
        deviceManager.addAudioCallback (&engine);
    });

    rendering.store (false, std::memory_order_relaxed);
    std::string errSnapshot;
    {
        const juce::ScopedLock lock (lastErrorLock);
        errSnapshot = lastError;
    }
    if (onFinished) onFinished (succeeded, errSnapshot);
}

std::unique_ptr<juce::AudioFormatWriter>
BounceEngine::openWriterFor (const juce::File& outFile, std::string& errOut) const
{
    std::unique_ptr<juce::FileOutputStream> outStream (outFile.createOutputStream());
    if (outStream == nullptr)
    {
        errOut = ("Could not open output file " + outFile.getFullPathName()).toStdString();
        return nullptr;
    }
    outStream->setPosition (0);
    outStream->truncate();
    auto writer = makeWriter (std::move (outStream), errOut);
    if (writer == nullptr)
        errOut += (" (" + outFile.getFileName() + ")").toStdString();
    return writer;
}

void BounceEngine::armStemTap (const StemTarget& target, float* l, float* r)
{
    switch (target.kind)
    {
        case StemTarget::Kind::Track: engine.getStrip (target.index).setStemCapture (l, r); break;
        case StemTarget::Kind::Bus:   engine.setBusStemCapture (target.index, l, r);        break;
        case StemTarget::Kind::Aux:   engine.setAuxStemCapture (target.index, l, r);        break;
        case StemTarget::Kind::Mix:   break;   // fed straight off the mix bus, no tap
    }
}

void BounceEngine::clearAllStemTaps()
{
    for (int t = 0; t < Session::kNumTracks; ++t)
        engine.getStrip (t).setStemCapture (nullptr, nullptr);
    for (int a = 0; a < Session::kNumBuses; ++a)
        engine.setBusStemCapture (a, nullptr, nullptr);
    for (int a = 0; a < Session::kNumAuxLanes; ++a)
        engine.setAuxStemCapture (a, nullptr, nullptr);
}

std::int64_t BounceEngine::leadInFor (StemTarget::Kind kind) const
{
    const auto trackLead = (std::int64_t) engine.getAggregatePdcLatencySamples();
    if (kind == StemTarget::Kind::Track || kind == StemTarget::Kind::Bus)
        return trackLead;
    return trackLead + (std::int64_t) engine.getMasterDryPdcTargetSamples();
}

bool BounceEngine::runStemsMode()
{
    const auto targets = collectStemTargets (session, outputFile);
    if (targets.empty())
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "No tracks with content or armed for recording";
        return false;
    }
    totalStemsToRender.store ((int) targets.size(), std::memory_order_relaxed);

    ScopedOfflineRender offlineGuard (engine);
    // Detach + re-prepare on the message thread (plugin (de)activate must not run
    // on this worker - see runOnMessageThread). Stems render through the full
    // strip path too, so oversample them 4× for alias-free output; cleared before
    // the live re-prepare below.
    if (! runOnMessageThread ([this]
        {
            deviceManager.removeAudioCallback (&engine);
            engine.setRenderOversamplingOverride (kRenderOversamplingFactor);
            engine.prepareForSelfTest (renderSampleRate, renderBlockSize);
        }))
        return false;   // shutdown before detach/prepare - engine untouched

    auto& transport = engine.getTransport();
    const auto savedTransportState = transport.getState();
    const auto savedPlayhead       = transport.getPlayhead();
    const auto savedStage          = engine.getStage();
    engine.setStage (AudioEngine::Stage::Mixing);
    // Same reason as the master-mix path: the stem drive runs Playing from
    // block 0, so the in-callback (stopped-only) relatch never engages the
    // master-stage aux PDC - latch it while the callback is detached.
    engine.applyMasterPdcTargetsNow();

    const int numStems = (int) targets.size();

    // One stereo capture scratch per stem, registered as taps below. The
    // strips / engine accumulate into them; we clear them before each driven
    // block, so a skipped (silent) unit leaves silence.
    std::vector<std::vector<float>> capL ((size_t) numStems,
                                            std::vector<float> ((size_t) renderBlockSize, 0.0f));
    std::vector<std::vector<float>> capR ((size_t) numStems,
                                            std::vector<float> ((size_t) renderBlockSize, 0.0f));

    // Open every writer up front so a full disk / bad path fails before any
    // rendering.
    bool succeeded = true;
    std::vector<std::unique_ptr<juce::AudioFormatWriter>> writers;
    writers.reserve ((size_t) numStems);
    for (const auto& tgt : targets)
    {
        std::string writerErr;
        auto writer = openWriterFor (tgt.file, writerErr);
        if (writer == nullptr)
        {
            {
                const juce::ScopedLock lock (lastErrorLock);
                lastError = writerErr;
            }
            succeeded = false;
            break;
        }
        writers.push_back (std::move (writer));
    }

    if (succeeded)
    {
        for (int i = 0; i < numStems; ++i)
            armStemTap (targets[(size_t) i],
                        capL[(size_t) i].data(), capR[(size_t) i].data());

        constexpr int kNumChannels = 2;
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

        transport.setPlayhead (0);
        transport.setState (Transport::State::Playing);
        engine.getPlaybackEngine().preparePlayback();

        // Per-kind PDC lead-in trim (see leadInFor). Trimming each stem by
        // its own offset keeps the whole set mutually sample-aligned at
        // sample 0 without cutting real head content off track / bus stems.
        std::vector<std::int64_t> leadIn     ((size_t) numStems, 0);
        std::vector<std::int64_t> droppedFor ((size_t) numStems, 0);
        std::vector<std::int64_t> writtenFor ((size_t) numStems, 0);
        std::int64_t maxLead = 0;
        for (int i = 0; i < numStems; ++i)
        {
            leadIn[(size_t) i] = leadInFor (targets[(size_t) i].kind);
            maxLead = juce::jmax (maxLead, leadIn[(size_t) i]);
        }
        const std::int64_t toRender = totalSamples + maxLead;

        std::int64_t done = 0;
        while (done < toRender && ! cancelRequested.load (std::memory_order_relaxed))
        {
            const int remaining = (int) juce::jmin ((std::int64_t) renderBlockSize,
                                                      toRender - done);

            for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);
            for (int i = 0; i < numStems; ++i)
            {
                juce::FloatVectorOperations::clear (capL[(size_t) i].data(), remaining);
                juce::FloatVectorOperations::clear (capR[(size_t) i].data(), remaining);
            }

            engine.audioDeviceIOCallbackWithContext (inputPtrs.data(), kNumIn,
                                                       outputPtrs.data(), kNumChannels,
                                                       remaining, ctx);

            std::int64_t minWritten = totalSamples;
            for (int i = 0; i < numStems; ++i)
            {
                int writeStart = 0;
                if (droppedFor[(size_t) i] < leadIn[(size_t) i])
                {
                    writeStart = (int) juce::jmin ((std::int64_t) remaining,
                                                     leadIn[(size_t) i] - droppedFor[(size_t) i]);
                    droppedFor[(size_t) i] += writeStart;
                }
                // Shorter-lead stems run out of file before the render loop
                // ends; cap each writer at totalSamples so lengths stay equal.
                const int writeCount = (int) juce::jmin ((std::int64_t) (remaining - writeStart),
                                                           totalSamples - writtenFor[(size_t) i]);
                if (writeCount > 0)
                {
                    const float* ptrs[kNumChannels] = { capL[(size_t) i].data() + writeStart,
                                                        capR[(size_t) i].data() + writeStart };
                    if (! writers[(size_t) i]->writeFromFloatArrays (ptrs, kNumChannels, writeCount))
                    {
                        const juce::ScopedLock lock (lastErrorLock);
                        lastError = "Writer failed mid-stem at "
                                    + std::to_string (writtenFor[(size_t) i]) + " samples in "
                                    + targets[(size_t) i].file.getFileName().toStdString();
                        succeeded = false;
                        break;
                    }
                    writtenFor[(size_t) i] += writeCount;
                }
                minWritten = juce::jmin (minWritten, writtenFor[(size_t) i]);
            }
            if (! succeeded) break;

            done += remaining;
            renderedSamples.store (minWritten, std::memory_order_relaxed);
            const float overall = (float) ((double) minWritten / (double) totalSamples);
            progress.store (overall, std::memory_order_relaxed);
            if (onProgressUpdated) onProgressUpdated (overall);
        }

        engine.getPlaybackEngine().stopPlayback();
    }

    clearAllStemTaps();
    const size_t writersOpened = writers.size();
    writers.clear();  // flush + close before any delete below

    if (cancelRequested.load (std::memory_order_relaxed))
        succeeded = false;
    if (! succeeded)
    {
        // Drop the partial set - half-rendered files are more confusing than
        // no files when the user cancels or a writer fails mid-render. Only
        // files this run actually opened (truncated): targets past a failed
        // open still hold the previous bounce's good stems.
        const size_t touched = juce::jmin (targets.size(), writersOpened + 1);
        for (size_t i = 0; i < touched; ++i)
            targets[i].file.deleteFile();
    }

    // Restore everything - transport, stage, device callback.
    transport.setState (savedTransportState);
    transport.setPlayhead (savedPlayhead);
    engine.setStage (savedStage);
    // Back to the user's realtime oversampling, then reattach on the message
    // thread - addAudioCallback re-prepares the engine (plugin (de)activate),
    // which must not run on this worker.
    runOnMessageThread ([this]
    {
        engine.setRenderOversamplingOverride (0);
        deviceManager.addAudioCallback (&engine);
    });

    if (cancelRequested.load (std::memory_order_relaxed))
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = kCancelledError;
        succeeded = false;
    }
    return succeeded;
}

bool BounceEngine::runRealtimeMode()
{
    // Realtime capture: the engine stays attached and the session PLAYS.
    // The audio callback pushes the capture taps (or the master mix) into
    // ThreadedWriters (see AudioEngine::RtBounceSink); this worker only
    // orchestrates transport state and polls progress. Hardware inserts run
    // their external loop for real - the whole point of this mode.
    std::vector<StemTarget> files;
    if (renderMode == Mode::Stems)
    {
        files = collectStemTargets (session, outputFile);
        if (files.empty())
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = "No tracks with content or armed for recording";
            return false;
        }
        totalStemsToRender.store ((int) files.size(), std::memory_order_relaxed);
    }
    else
    {
        files.push_back ({ StemTarget::Kind::Mix, -1, outputFile });
    }
    const int numFiles = (int) files.size();

    juce::TimeSliceThread diskThread ("Dusk Studio bounce disk");
    diskThread.startThread();

    // Capture scratches sized to the live block; the callback never hands the
    // strips more than the prepared block size (oversized host blocks are
    // guarded upstream).
    const int scratchLen = juce::jmax (engine.getCurrentBlockSize(), 4096);
    std::vector<std::vector<float>> capL, capR;
    if (renderMode == Mode::Stems)
    {
        capL.assign ((size_t) numFiles, std::vector<float> ((size_t) scratchLen, 0.0f));
        capR.assign ((size_t) numFiles, std::vector<float> ((size_t) scratchLen, 0.0f));
    }

    bool succeeded = true;
    std::vector<std::unique_ptr<juce::AudioFormatWriter::ThreadedWriter>> writers;
    writers.reserve ((size_t) numFiles);
    for (const auto& f : files)
    {
        std::string writerErr;
        auto writer = openWriterFor (f.file, writerErr);
        if (writer == nullptr)
        {
            {
                const juce::ScopedLock lock (lastErrorLock);
                lastError = writerErr;
            }
            succeeded = false;
            break;
        }
        writers.push_back (std::make_unique<juce::AudioFormatWriter::ThreadedWriter> (
            writer.release(), diskThread, 1 << 17));
    }

    auto& transport = engine.getTransport();
    const auto savedPlayhead = transport.getPlayhead();
    const bool savedLoop     = transport.isLoopEnabled();

    if (succeeded)
    {
        auto sinks = std::make_unique<AudioEngine::RtBounceSink[]> ((size_t) numFiles);
        for (int i = 0; i < numFiles; ++i)
        {
            auto& sink = sinks[(size_t) i];
            sink.writer = writers[(size_t) i].get();
            sink.cap    = totalSamples;
            sink.leadRemaining = leadInFor (files[(size_t) i].kind);
            if (files[(size_t) i].kind != StemTarget::Kind::Mix)
            {
                auto* l = capL[(size_t) i].data();
                auto* r = capR[(size_t) i].data();
                sink.srcL = l;
                sink.srcR = r;
                sink.wipeL = l;
                sink.wipeR = r;
                armStemTap (files[(size_t) i], l, r);
            }
            // Mix sinks keep srcL null - the callback feeds them the mix bus.
        }

        engine.armRealtimeBounce (sinks.get(), numFiles);

        // Re-check the transport INSIDE the marshaled start: start()'s
        // stopped-gate ran when the dialog opened, but MCU / MIDI-binding
        // transport commands can start playback in the gap - yanking a live
        // take to sample 0 here would corrupt it.
        bool transportWasBusy = false;
        if (! runOnMessageThread ([this, &transport, &transportWasBusy]
            {
                if (! transport.isStopped())
                {
                    transportWasBusy = true;
                    return;
                }
                transport.setLoopEnabled (false);
                transport.setPlayhead (0);
                engine.play();
            }))
        {
            engine.disarmRealtimeBounce();
            succeeded = false;
        }
        else if (transportWasBusy)
        {
            engine.disarmRealtimeBounce();
            {
                const juce::ScopedLock lock (lastErrorLock);
                lastError = "Transport started before the realtime bounce could begin";
            }
            succeeded = false;
        }

        if (succeeded)
        {
            // Poll until every sink has its full length, the user cancels, or
            // the transport stops from under us (device change, user Stop).
            std::int64_t minWritten = 0;
            std::int64_t lastProgressCount = -1;
            juce::uint32 lastProgressMs = juce::Time::getMillisecondCounter();
            while (! cancelRequested.load (std::memory_order_relaxed))
            {
                minWritten = totalSamples;
                bool anyWriteFailed = false;
                for (int i = 0; i < numFiles; ++i)
                {
                    minWritten = juce::jmin (minWritten,
                                             sinks[(size_t) i].written.load (std::memory_order_acquire));
                    anyWriteFailed = anyWriteFailed
                                   || sinks[(size_t) i].writeFailed.load (std::memory_order_acquire);
                }
                if (anyWriteFailed)
                {
                    const juce::ScopedLock lock (lastErrorLock);
                    lastError = "Realtime bounce overran the disk writer (disk too slow)";
                    succeeded = false;
                    break;
                }
                if (engine.wasRealtimeBounceAborted())
                {
                    // A re-prepare (device change, buffer renegotiation)
                    // invalidated the armed scratches; the engine disarmed.
                    const juce::ScopedLock lock (lastErrorLock);
                    lastError = "Audio device changed during the realtime bounce";
                    succeeded = false;
                    break;
                }
                renderedSamples.store (minWritten, std::memory_order_relaxed);
                const float p = (float) ((double) minWritten / (double) totalSamples);
                progress.store (p, std::memory_order_relaxed);
                if (onProgressUpdated) onProgressUpdated (p);

                if (minWritten >= totalSamples) break;

                const auto nowMs = juce::Time::getMillisecondCounter();
                if (minWritten != lastProgressCount)
                {
                    lastProgressCount = minWritten;
                    lastProgressMs = nowMs;
                }
                else if (nowMs - lastProgressMs > 5000)
                {
                    const juce::ScopedLock lock (lastErrorLock);
                    lastError = transport.isPlaying()
                                  ? "Realtime bounce stalled (no audio callbacks)"
                                  : "Transport stopped during the realtime bounce";
                    succeeded = false;
                    break;
                }
                wait (50);
            }

            runOnMessageThread ([this] { engine.stop(); });
        }

        engine.disarmRealtimeBounce();
        clearAllStemTaps();
        // A callback that loaded the sink count before the disarm can still
        // be mid-loop over the sinks; the process gate waits for in-flight
        // callbacks to drain, so after this fence nothing references the
        // sink array or the scratches. A fixed sleep can't guarantee that -
        // a device period or scheduler stall can exceed any constant.
        runOnMessageThread ([this]
        {
            engine.suspendProcessing();
            engine.resumeProcessing();
        });
    }

    // ThreadedWriter destructors flush their queues to disk.
    const size_t writersOpened = writers.size();
    writers.clear();
    diskThread.stopThread (2000);

    runOnMessageThread ([&transport, savedPlayhead, savedLoop]
    {
        transport.setLoopEnabled (savedLoop);
        transport.setPlayhead (savedPlayhead);
    });

    if (cancelRequested.load (std::memory_order_relaxed))
    {
        succeeded = false;
        const juce::ScopedLock lock (lastErrorLock);
        lastError = kCancelledError;
    }
    if (! succeeded)
    {
        // Only files this run actually opened (truncated): targets past a
        // failed open still hold the previous bounce's good stems.
        const size_t touched = juce::jmin (files.size(), writersOpened + 1);
        for (size_t i = 0; i < touched; ++i)
            files[i].file.deleteFile();
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
    // clear it here - otherwise isRendering() stays true forever and the dialog
    // wedges. Routes the caller into FreezeDialog's failBeforeStart path.
    if (! startThread())   // -> run() -> renderFreezeTrack on the worker thread
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

    // Open the writer first - a failure here means we never touch engine state.
    std::unique_ptr<juce::FileOutputStream> outStream (outFile.createOutputStream());
    if (outStream == nullptr)
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = ("Could not open freeze file " + outFile.getFullPathName()).toStdString();
        return false;
    }
    outStream->setPosition (0);
    outStream->truncate();

    constexpr int kNumChannels = 2;
    std::string writerErr;
    std::unique_ptr<juce::AudioFormatWriter> writer = makeWriter (std::move (outStream), writerErr);
    if (writer == nullptr)
    {
        outFile.deleteFile();   // drop the 0-byte file we just truncated
        const juce::ScopedLock lock (lastErrorLock);
        lastError = writerErr;
        return false;
    }

    // Detach + offline-prepare, exactly like the stem path (on the message
    // thread - plugin (de)activate must not run on this worker).
    ScopedOfflineRender offlineGuard (engine);
    if (! runOnMessageThread ([this]
        {
            deviceManager.removeAudioCallback (&engine);
            engine.setRenderOversamplingOverride (kRenderOversamplingFactor);
            engine.prepareForSelfTest (renderSampleRate, renderBlockSize);
        }))
    {
        // Detach/prepare never ran (shutdown) - engine untouched. Drop the
        // 0-byte file we truncated above and bail.
        writer.reset();
        outFile.deleteFile();
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
    // latency - and frozen playback reports 0 to PDC, so that delay would be
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
                lastError = "Writer failed mid-freeze at " + std::to_string (written) + " samples";
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

    // Restore everything - solo, transport, stage, oversampling, device callback.
    for (int t = 0; t < Session::kNumTracks; ++t)
        session.setTrackSoloed (t, savedSolo[(size_t) t]);
    transport.setState (savedTransportState);
    transport.setPlayhead (savedPlayhead);
    engine.setStage (savedStage);
    // Reattach on the message thread (addAudioCallback re-prepares the engine,
    // reaching plugin (de)activate - must not run on this worker).
    runOnMessageThread ([this]
    {
        engine.setRenderOversamplingOverride (0);
        deviceManager.addAudioCallback (&engine);
    });

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
