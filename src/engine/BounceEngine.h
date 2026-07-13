#pragma once

#include <juce_audio_devices/juce_audio_devices.h>
#include <juce_audio_formats/juce_audio_formats.h>
#include "../session/Session.h"
#include <array>
#include <atomic>
#include <functional>
#include <vector>

namespace duskstudio
{
class AudioEngine;

// Offline bounce. Detaches the engine from AudioDeviceManager and drives
// audioDeviceIOCallbackWithContext in a tight loop on its own worker
// thread. Real-time playback pauses for the duration. Render-in-place
// is still a follow-up.
class BounceEngine final : private juce::Thread
{
public:
    // MasterMix   : full track / aux / master path. Length = longest
    //               region end + tail.
    // Stems       : single offline pass, one WAV per stem target. A track
    //               stem is the track's post-fader / post-pan output (its
    //               full wet contribution, before the master-vs-bus routing
    //               split); bus groups and aux lanes with anything routed /
    //               sent print as additional stems of their own processed
    //               output. Mute / solo state prints as heard. Stems carry
    //               no master-strip processing, so track+bus+aux stems sum
    //               to the PRE-master mix; a bus-routed track appears both
    //               in its own stem and (processed) in the bus stem. Track
    //               files: `<base>_<NN>_<name>.wav`; units:
    //               `<base>_bus<N>_<name>.wav` / `<base>_aux<N>_<name>.wav`.
    // MasteringChain : MasteringPlayer -> MasteringChain. Length =
    //               player's source file + tail. Engine stage forced
    //               to Mastering for the render.
    enum class Mode { MasterMix, Stems, MasteringChain, FreezeTrack };

    // Output container. Wav = stereo 24-bit (always available). Mp3 = CBR via
    // libmp3lame; only usable when the build found LAME (else start() fails with
    // a clear error). The caller picks the matching file extension.
    enum class Format { Wav, Mp3 };

    // lastError value set when a render is cancelled. Shared so the dialog can
    // tell a user cancel from a real failure without a string literal that
    // silently drifts out of sync with run().
    static constexpr const char* kCancelledError = "Cancelled";

    // <dir>/<base>_<NN>_<safe>.wav. Empty / default ("5") track names
    // fall back to "track" so the user doesn't open 24 files called
    // mix_05_5.wav. Inline so the unit test links without dragging in
    // AudioEngine + Session + the bounce loop.
    static juce::File stemOutputFile (const juce::File& base,
                                        int trackIndexZeroBased,
                                        const juce::String& trackName)
    {
        auto dir  = base.getParentDirectory();
        auto stem = base.getFileNameWithoutExtension();
        if (stem.isEmpty()) stem = "bounce";

        auto safeName = trackName.trim();
        if (safeName.isEmpty() || safeName == juce::String (trackIndexZeroBased + 1))
            safeName = "track";

        safeName = juce::File::createLegalFileName (safeName);
        if (safeName.isEmpty()) safeName = "track";

        // Stems are always WAV regardless of the base file's extension: start()
        // forces Format::Wav for Stems mode (MP3 encoder delay + frame padding
        // would shift each stem and break the sample-accurate alignment a
        // re-import needs).
        const auto idx = juce::String (trackIndexZeroBased + 1).paddedLeft ('0', 2);
        return dir.getChildFile (stem + "_" + idx + "_" + safeName + ".wav");
    }

    // <dir>/<base>_<tag>_<safe>.wav for bus / aux stems (tag = "bus1".."aux4").
    // Falls back to the tag alone when the unit has no usable name.
    static juce::File namedStemOutputFile (const juce::File& base,
                                             const juce::String& tag,
                                             const juce::String& unitName)
    {
        auto dir  = base.getParentDirectory();
        auto stem = base.getFileNameWithoutExtension();
        if (stem.isEmpty()) stem = "bounce";

        auto safeName = juce::File::createLegalFileName (unitName.trim());
        return dir.getChildFile (safeName.isEmpty()
                                    ? stem + "_" + tag + ".wav"
                                    : stem + "_" + tag + "_" + safeName + ".wav");
    }

    // The stem files a Stems bounce against `base` would write, in render
    // order: tracks with content or armed, then buses any of those tracks
    // route into, then aux lanes any of them send to. Message-thread safe
    // (atomic reads only). Used by the render itself and by the UI's
    // overwrite-conflict check so the two can't drift.
    struct StemTarget
    {
        enum class Kind { Track, Bus, Aux };
        Kind kind;
        int  index;        // track / bus / aux index, zero-based
        juce::File file;
    };
    static std::vector<StemTarget> collectStemTargets (const Session& session,
                                                         const juce::File& base)
    {
        std::vector<StemTarget> targets;

        // The per-track send fan-out indexes the per-lane array below.
        static_assert (ChannelStripParams::kNumAuxSends == Session::kNumAuxLanes,
                       "aux-send count must match the aux-lane count");

        std::array<bool, (size_t) Session::kNumBuses>    busActive {};
        std::array<bool, (size_t) Session::kNumAuxLanes> auxActive {};

        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const auto& tr = session.track (t);
            const bool hasContent = ! tr.regions.empty()
                                  || ! tr.midiRegions.current().empty();
            const bool armed = tr.recordArmed.load (std::memory_order_relaxed);
            if (! (hasContent || armed)) continue;

            targets.push_back ({ StemTarget::Kind::Track, t,
                                 stemOutputFile (base, t, tr.name) });

            for (int a = 0; a < Session::kNumBuses; ++a)
                if (tr.strip.busAssign[(size_t) a].load (std::memory_order_relaxed))
                    busActive[(size_t) a] = true;
            for (int a = 0; a < ChannelStripParams::kNumAuxSends; ++a)
                if (tr.strip.auxSendDb[(size_t) a].load (std::memory_order_relaxed)
                        > ChannelStripParams::kFaderInfThreshDb)
                    auxActive[(size_t) a] = true;
        }

        for (int a = 0; a < Session::kNumBuses; ++a)
            if (busActive[(size_t) a])
                targets.push_back ({ StemTarget::Kind::Bus, a,
                                     namedStemOutputFile (base, "bus" + juce::String (a + 1),
                                                            session.bus (a).name) });
        for (int a = 0; a < Session::kNumAuxLanes; ++a)
            if (auxActive[(size_t) a])
                targets.push_back ({ StemTarget::Kind::Aux, a,
                                     namedStemOutputFile (base, "aux" + juce::String (a + 1),
                                                            session.auxLane (a).name) });
        return targets;
    }

    // True when any rendered track's strip or any aux lane has a hardware
    // insert routed. Offline renders can't run the external loop (the device
    // callback is detached), so the insert returns dry or silence - the UI
    // warns before an offline bounce of such a session.
    static bool anyHardwareInsertActive (const Session& session)
    {
        for (int t = 0; t < Session::kNumTracks; ++t)
        {
            const auto& tr = session.track (t);
            const bool hasContent = ! tr.regions.empty()
                                  || ! tr.midiRegions.current().empty();
            const bool armed = tr.recordArmed.load (std::memory_order_relaxed);
            if ((hasContent || armed)
                && tr.hardwareInsert.enabled.load (std::memory_order_relaxed))
                return true;
        }
        for (int a = 0; a < Session::kNumAuxLanes; ++a)
            for (const auto& hw : session.auxLane (a).hardwareInserts)
                if (hw.enabled.load (std::memory_order_relaxed))
                    return true;
        return false;
    }

    BounceEngine (AudioEngine& engine, Session& session,
                   juce::AudioDeviceManager& deviceManager) noexcept;
    ~BounceEngine() override;

    // False if a render is already in flight. sampleRate <= 0 uses
    // engine's current rate. blockSize 1024 amortises overhead vs
    // realtime. tail = silence appended so reverb/comp tails decay.
    // wavBitDepth: 24 (default) or 16 - 16 gets TPDF dither at ±1 LSB
    // before the truncation (CD / distribution target).
    bool start (const juce::File& outputFile,
                double sampleRate = 0.0,
                int blockSize = 1024,
                double tailSeconds = 5.0,
                Mode mode = Mode::MasterMix,
                Format format = Format::Wav,
                int mp3BitrateKbps = 320,
                int wavBitDepth = 24);

    void cancel() noexcept { cancelRequested.store (true, std::memory_order_relaxed); }

    // Synchronous single-track FREEZE render (message thread; transport must be
    // stopped). Bakes trackIndex's PRE-FADER signal - instrument + EQ + comp,
    // before fader/pan/sends - to a 24-bit stereo WAV at outFile, lenSamples
    // long at sampleRate. Reuses the offline drive (detach device, 4× oversample,
    // PDC lead-in trim) like the stem path, but captures the strip's pre-fader
    // tap (ChannelStrip::setFreezeCapture) instead of the master mix. Blocks
    // until done; unlike start() it does NOT spin the worker thread. Returns
    // false on failure (getLastError set); deletes a partial file on failure.
    bool renderFreezeTrack (int trackIndex, const juce::File& outFile,
                            std::int64_t lenSamples, double sampleRate,
                            int blockSize = 1024);

    // Async wrapper around renderFreezeTrack: spins the worker thread (like
    // start()) so the message thread isn't wedged during a long render. Poll
    // isRendering()/getProgress(), cancel() to abort, onFinished fires on the
    // worker thread when done. False if a render is already in flight.
    bool startFreeze (int trackIndex, const juce::File& outFile,
                      std::int64_t lenSamples, double sampleRate, int blockSize = 1024);

    bool         isRendering() const noexcept { return rendering.load (std::memory_order_relaxed); }
    float        getProgress() const noexcept { return progress.load (std::memory_order_relaxed); }
    int          getTotalStemsToRender() const noexcept
        { return totalStemsToRender.load (std::memory_order_relaxed); }
    juce::String getLastError() const
    {
        // juce::String is refcounted; concurrent copy races the refcount.
        const juce::ScopedLock lock (lastErrorLock);
        return lastError;
    }
    std::int64_t  getRenderedSamples() const noexcept { return renderedSamples.load (std::memory_order_relaxed); }

    // Called on the worker thread. Use MessageManager::callAsync for UI.
    std::function<void()>                  onStarted;
    std::function<void(float)>             onProgressUpdated;
    std::function<void(bool, juce::String)> onFinished;

private:
    void run() override;

    // Run fn on the message thread and block the worker until it completes.
    // The offline render detaches the audio device and re-prepares the engine,
    // which reaches every hosted plugin's activate/deactivate - CLAP (and the
    // VST3/LV2 contracts) require those on the MAIN thread, and a live plugin
    // editor keeps pumping its GUI on the real message thread during the bounce,
    // so the (de)activate must be serialised there, not run on this worker.
    // Runs fn inline when already on the message thread (the synchronous freeze
    // path) or when no MessageManager exists (headless tests). Polls so app
    // shutdown (stopThread -> signalThreadShouldExit) can't deadlock the worker
    // against ~BounceEngine's join.
    // Returns true iff fn ran to completion. Returns false when the call is
    // abandoned - the message queue rejected the post (message manager quitting)
    // or shutdown asked the worker to exit before fn ran. A false return means
    // the engine was NOT detached/re-prepared (or re-attached), so the caller
    // must unwind without touching engine state.
    bool runOnMessageThread (std::function<void()> fn);

    AudioEngine& engine;
    Session&     session;
    juce::AudioDeviceManager& deviceManager;

    juce::File   outputFile;
    double       renderSampleRate = 0.0;
    int          renderBlockSize  = 1024;
    double       tailSeconds      = 5.0;
    std::int64_t  totalSamples     = 0;
    Mode         renderMode       = Mode::MasterMix;
    Format       renderFormat     = Format::Wav;
    int          renderBitrateKbps = 320;
    int          renderWavBitDepth = 24;
    int          freezeTrackIndex = -1;   // Mode::FreezeTrack target
    std::int64_t  freezeLenSamples = 0;

    std::atomic<bool>  rendering        { false };
    std::atomic<bool>  cancelRequested  { false };
    std::atomic<float> progress         { 0.0f };
    std::atomic<std::int64_t> renderedSamples { 0 };
    std::atomic<int>   totalStemsToRender { 0 };
    juce::String lastError;
    juce::CriticalSection lastErrorLock;

    std::int64_t computeBounceLength (double sampleRate, double tail) const;

    // Create the WAV or MP3 writer for the chosen format over an already-opened
    // (truncated) output stream. Returns null + sets errOut on failure. Takes
    // ownership of outStream on success; on failure outStream is freed.
    std::unique_ptr<juce::AudioFormatWriter>
        makeWriter (std::unique_ptr<juce::FileOutputStream> outStream, juce::String& errOut) const;

    bool runStemsMode();
};
} // namespace duskstudio
