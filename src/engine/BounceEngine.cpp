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

    if (renderMode == Mode::MasterMix)
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
} // namespace duskstudio
