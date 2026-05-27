#include "BounceEngine.h"
#include "AudioEngine.h"
#include "MasteringPlayer.h"
#include "../session/Session.h"

namespace duskstudio
{
BounceEngine::BounceEngine (AudioEngine& e, Session& s,
                              juce::AudioDeviceManager& dm) noexcept
    : juce::Thread ("Dusk Studio bounce"), engine (e), session (s), deviceManager (dm)
{}

BounceEngine::~BounceEngine()
{
    cancel();
    stopThread (5000);
}

juce::int64 BounceEngine::computeBounceLength (double sampleRate, double tail) const
{
    // Longest region end across all tracks defines the natural bounce end;
    // tail extends that so reverb/comp/EQ ringouts decay before we cut.
    juce::int64 maxRegionEnd = 0;
    for (int t = 0; t < Session::kNumTracks; ++t)
    {
        const auto& regions = session.track (t).regions;
        for (const auto& r : regions)
        {
            const juce::int64 end = r.timelineStart + r.lengthInSamples;
            if (end > maxRegionEnd) maxRegionEnd = end;
        }
    }
    if (maxRegionEnd <= 0) maxRegionEnd = (juce::int64) (sampleRate * 1.0);  // 1 s of silence
    return maxRegionEnd + (juce::int64) (sampleRate * tail);
}

bool BounceEngine::start (const juce::File& outFile, double sr, int bs, double tail,
                            Mode mode)
{
    if (rendering.load (std::memory_order_relaxed)) return false;

    outputFile  = outFile;
    renderSampleRate = (sr > 0.0) ? sr : engine.getCurrentSampleRate();
    if (renderSampleRate <= 0.0) renderSampleRate = 48000.0;
    renderBlockSize = juce::jmax (64, bs);
    tailSeconds     = tail;
    renderMode      = mode;

    if (renderMode == Mode::MasterMix || renderMode == Mode::Stems)
    {
        totalSamples = computeBounceLength (renderSampleRate, tailSeconds);
    }
    else
    {
        // Mastering: render length = player's loaded file length + tail.
        const auto playerLen = engine.getMasteringPlayer().getLengthSamples();
        totalSamples = playerLen + (juce::int64) (renderSampleRate * tailSeconds);
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
        if (stems == 0) return false;  // nothing to render
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

    // Open the WAV writer first - failure here means we don't bother
    // touching the engine state.
    juce::WavAudioFormat wavFormat;
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

    // 24-bit stereo @ session SR - matches the spec default. Bit depth
    // could become a parameter later.
    constexpr int   kBitsPerSample = 24;
    constexpr int   kNumChannels   = 2;
    juce::StringPairArray metadata;

    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (outStream.get(),
                                     renderSampleRate,
                                     (unsigned) kNumChannels,
                                     kBitsPerSample,
                                     metadata,
                                     0));
    if (writer == nullptr)
    {
        juce::String err = "Could not create WAV writer";
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = err;
        }
        rendering.store (false, std::memory_order_relaxed);
        if (onFinished) onFinished (false, err);
        return;
    }
    outStream.release();  // writer now owns the stream

    // Detach the engine from the realtime device - we drive its audio
    // callback ourselves at non-realtime pace. State is restored at the
    // bottom of this function regardless of how we exit.
    deviceManager.removeAudioCallback (&engine);
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
    }
    else
    {
        // Mastering chain: the audio callback's mastering branch reads
        // from MasteringPlayer, so seek the player to 0 + start it.
        engine.setStage (AudioEngine::Stage::Mastering);
        engine.getMasteringPlayer().setPlayhead (0);
        engine.getMasteringPlayer().play();
    }

    juce::int64 done = 0;
    bool succeeded = true;
    while (done < totalSamples && ! cancelRequested.load (std::memory_order_relaxed))
    {
        const int remaining = (int) juce::jmin ((juce::int64) renderBlockSize,
                                                  totalSamples - done);

        // Reset outputs each block.
        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);

        engine.audioDeviceIOCallbackWithContext (inputPtrs.data(), kNumIn,
                                                   outputPtrs.data(), kNumChannels,
                                                   remaining, ctx);

        // Use a temporary AudioBuffer wrapping the channel pointers so the
        // writer's writeFromFloatArrays gets the canonical interface.
        if (! writer->writeFromFloatArrays (outputPtrs.data(), kNumChannels, remaining))
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = "Writer failed mid-render at " + juce::String (done) + " samples";
            succeeded = false;
            break;
        }

        done += remaining;
        renderedSamples.store (done, std::memory_order_relaxed);
        const float p = (float) ((double) done / (double) totalSamples);
        progress.store (p, std::memory_order_relaxed);
        if (onProgressUpdated) onProgressUpdated (p);
    }

    if (cancelRequested.load (std::memory_order_relaxed))
    {
        succeeded = false;
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "Cancelled";
    }

    writer.reset();  // flush + close

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
                                    juce::int64 lenSamples,
                                    float stemFractionStart,
                                    float stemFractionWidth)
{
    juce::WavAudioFormat wavFormat;
    std::unique_ptr<juce::FileOutputStream> outStream (outFile.createOutputStream());
    if (outStream == nullptr)
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "Could not open stem output file " + outFile.getFullPathName();
        return false;
    }
    outStream->setPosition (0);
    outStream->truncate();

    constexpr int kBitsPerSample = 24;
    constexpr int kNumChannels   = 2;
    juce::StringPairArray metadata;
    std::unique_ptr<juce::AudioFormatWriter> writer (
        wavFormat.createWriterFor (outStream.get(),
                                     renderSampleRate,
                                     (unsigned) kNumChannels,
                                     kBitsPerSample,
                                     metadata,
                                     0));
    if (writer == nullptr)
    {
        // createWriterFor failed AFTER we truncated the file above -
        // close our handle and drop the now-zeroed file so we don't
        // leave a 0-byte stem on disk. (createWriterFor only takes
        // ownership of outStream on success, so resetting here is safe.)
        outStream.reset();
        outFile.deleteFile();
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "Could not create WAV writer for stem " + outFile.getFileName();
        return false;
    }
    outStream.release();  // writer now owns the stream

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

    juce::int64 done = 0;
    bool ok = true;
    while (done < lenSamples && ! cancelRequested.load (std::memory_order_relaxed))
    {
        const int remaining = (int) juce::jmin ((juce::int64) renderBlockSize,
                                                  lenSamples - done);

        for (auto& o : outputs) std::fill (o.begin(), o.end(), 0.0f);

        engine.audioDeviceIOCallbackWithContext (inputPtrs.data(), kNumIn,
                                                   outputPtrs.data(), kNumChannels,
                                                   remaining, ctx);

        if (! writer->writeFromFloatArrays (outputPtrs.data(), kNumChannels, remaining))
        {
            const juce::ScopedLock lock (lastErrorLock);
            lastError = "Writer failed mid-stem at "
                        + juce::String (done) + " samples in "
                        + outFile.getFileName();
            ok = false;
            break;
        }

        done += remaining;
        renderedSamples.store (done, std::memory_order_relaxed);
        const float withinStem = (float) ((double) done / (double) lenSamples);
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
    engine.prepareForSelfTest (renderSampleRate, renderBlockSize);

    auto& transport = engine.getTransport();
    const auto savedTransportState = transport.getState();
    const auto savedPlayhead       = transport.getPlayhead();
    const auto savedStage          = engine.getStage();
    engine.setStage (AudioEngine::Stage::Mixing);

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
    deviceManager.addAudioCallback (&engine);

    if (cancelRequested.load (std::memory_order_relaxed))
    {
        const juce::ScopedLock lock (lastErrorLock);
        lastError = "Cancelled";
        succeeded = false;
    }
    return succeeded;
}
} // namespace duskstudio
