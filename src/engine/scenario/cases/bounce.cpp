#include "../RecordingMidiBackend.h"
#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../BounceEngine.h"
#include "../../MasteringPlayer.h"
#include "../../builtin/BuiltinRegistry.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../device/DeviceManager.h"
#include "../../device/IODevice.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../foundation/AutoResetEvent.h"
#include "../../../foundation/Fs.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <initializer_list>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <utility>
#include <vector>

#if defined (__linux__)
 #include <dlfcn.h>
#endif

namespace duskstudio::scenario
{
namespace
{
constexpr double kRate = ScenarioContext::kSampleRate;
constexpr int kRenderTimeoutMs = 90000;
constexpr float kPi = 3.14159265358979f;

// The bounce API takes the framework's file type; naming it through Session's
// own getter keeps this file free of the framework header.
using SessionFile = std::decay_t<decltype (std::declval<const Session&>().getSessionDirectory())>;

SessionFile sessionFile (const std::filesystem::path& path)
{
    return SessionFile (path.u8string().c_str());
}

bool writeMono (const std::filesystem::path& path, const std::vector<float>& samples)
{
    dusk::audio::WriteSpec spec;
    spec.sampleRate = kRate;
    spec.numChannels = 1;
    spec.bitsPerSample = 32;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const float* channels[] = { samples.data() };
    return writer != nullptr && writer->write (channels, 1, (std::int64_t) samples.size())
        && writer->flush();
}

#if ! DUSKSTUDIO_HAS_LAME
// Every release build ships the encoder, so a packaged run sets
// DUSKSTUDIO_EXPECT_MP3=1 and a build that lost it fails instead of skipping.
ScenarioResult noMp3Encoder()
{
    const char* expect = std::getenv ("DUSKSTUDIO_EXPECT_MP3");
    if (expect != nullptr && std::string (expect) == "1")
        return ScenarioResult::fail ("built without the MP3 encoder, and DUSKSTUDIO_EXPECT_MP3=1 requires it");
    return ScenarioResult::skip ("built without the MP3 encoder");
}
#endif

std::vector<float> sine (float hz, float amplitude, int frames)
{
    std::vector<float> samples ((std::size_t) frames);
    for (int i = 0; i < frames; ++i)
        samples[(std::size_t) i] = amplitude * std::sin (2.0f * kPi * hz * (float) i / (float) kRate);
    return samples;
}

struct Rendered
{
    dusk::audio::FileInfo info;
    std::vector<float> left, right;

    float peak() const
    {
        float p = 0.0f;
        for (const auto* ch : { &left, &right })
            for (const float s : *ch) p = std::max (p, std::abs (s));
        return p;
    }
};

std::optional<Rendered> readBack (const std::filesystem::path& path)
{
    auto reader = dusk::audio::FileReader::open (path);
    if (reader == nullptr) return std::nullopt;
    Rendered out;
    out.info = reader->info();
    const auto frames = (std::size_t) out.info.numFrames;
    out.left.assign (frames, 0.0f);
    out.right.assign (frames, 0.0f);
    float* dest[] = { out.left.data(), out.right.data() };
    reader->read (dest, std::min (2, out.info.numChannels), 0, out.info.numFrames);
    if (out.info.numChannels == 1) out.right = out.left;
    return out;
}

std::filesystem::path pathOf (const SessionFile& file)
{
    return std::filesystem::u8path (file.getFullPathName().toStdString());
}

std::int64_t argmaxAbs (const std::vector<float>& samples)
{
    std::size_t at = 0;
    for (std::size_t i = 1; i < samples.size(); ++i)
        if (std::abs (samples[i]) > std::abs (samples[at])) at = i;
    return (std::int64_t) at;
}

std::vector<float> impulse (int frames, int at)
{
    std::vector<float> samples ((std::size_t) frames, 0.0f);
    samples[(std::size_t) at] = 0.5f;
    return samples;
}

// A 4x oversampler's latency is 26.5 samples, so each one an integer trim
// rounds can leave an impulse's peak a sample either side of where it was
// placed.
constexpr std::int64_t kFractionalSlack = 1;

// A mono region on a mono track, reading `file` from its first sample.
void placeRegion (ScenarioContext& ctx, int track, const std::filesystem::path& file,
                  std::int64_t start, std::int64_t length)
{
    AudioRegion region;
    region.file = sessionFile (file);
    region.timelineStart = start;
    region.lengthInSamples = length;
    region.numChannels = 1;
    ctx.session().track (track).mode.store ((int) Track::Mode::Mono, std::memory_order_relaxed);
    ctx.session().track (track).regions.push_back (region);
}

struct Render
{
    BounceEngine::Mode mode = BounceEngine::Mode::MasterMix;
    BounceEngine::Format format = BounceEngine::Format::Wav;
    double sampleRate = kRate;
    double tailSeconds = 5.0;
    int wavBitDepth = 24;
    OfflineRenderQuality quality = OfflineRenderQuality::High;
};

// One bounce in flight. The worker hands its device detach and reattach back to
// the message thread, so a scenario waits on it rather than blocking.
struct BounceRun
{
    std::unique_ptr<BounceEngine> bounce;
    std::atomic<bool> finished { false };
    bool ok = false;
    std::string error;
};

std::shared_ptr<BounceRun> startRender (ScenarioContext& ctx, const std::filesystem::path& out,
                                        const Render& render,
                                        std::function<void()> onWorkerStarted = {},
                                        std::function<void(float)> onProgress = {})
{
    auto run = std::make_shared<BounceRun>();
    run->bounce = std::make_unique<BounceEngine> (ctx.engine(), ctx.session(), render.quality);
    run->bounce->onStarted = std::move (onWorkerStarted);
    run->bounce->onProgressUpdated = std::move (onProgress);
    // Joins the worker before the world resets, even when a wait times out.
    ctx.cleanup ([run] { run->bounce.reset(); });
    run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
    {
        raw->ok = ok;
        raw->error = std::move (error);
        raw->finished.store (true, std::memory_order_release);
    };
    if (! run->bounce->start (sessionFile (out), render.sampleRate, 1024, render.tailSeconds,
                              render.mode, render.format, 320, render.wavBitDepth))
    {
        ctx.complete (ScenarioResult::fail ("the bounce refused to start: "
                                            + run->bounce->getLastError()));
        return nullptr;
    }
    return run;
}

void renderThen (ScenarioContext& ctx, const std::filesystem::path& out, const Render& render,
                 std::function<void (bool ok, const std::string& error)> then)
{
    auto run = startRender (ctx, out, render);
    if (run == nullptr) return;
    ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire)
                                  && ! run->bounce->isRendering(); },
                   kRenderTimeoutMs,
                   [run, then] { then (run->ok, run->error); },
                   "the bounce never finished");
}

// -------------------------------------------------------- the master mix file

std::optional<ScenarioResult> finalizationFailures (ScenarioContext& ctx)
{
#if ! defined (__linux__) || ! defined (DUSKSTUDIO_HAS_PIPEWIRE)
    (void) ctx;
    return ScenarioResult::skip ("requires the native PipeWire fault harness");
#else
    const auto arm = reinterpret_cast<void (*) (int, const char*)> (dlsym (RTLD_DEFAULT, "dusk_audiofile_arm"));
    const auto hits = reinterpret_cast<int (*)()> (dlsym (RTLD_DEFAULT, "dusk_audiofile_hits"));
    const auto disarm = reinterpret_cast<void (*)()> (dlsym (RTLD_DEFAULT, "dusk_audiofile_disarm"));
    const char* runtime = std::getenv ("PIPEWIRE_RUNTIME_DIR");
    if (arm == nullptr || hits == nullptr || disarm == nullptr || runtime == nullptr
        || std::string (runtime).find ("/tmp/dusk-pw.") != 0)
        return ScenarioResult::skip ("requires the owned null graph and native fault fixture");
    disarm();
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    ctx.keep (session.oversamplingFactor);
    ctx.keep (session.deviceCaptureChannels);
    ctx.keep (session.track (0).strip.auxSendDb[0]);
    ctx.keep (session.auxLane (0).params.returnLevelDb);
    session.oversamplingFactor.store (1);
    session.track (0).strip.auxSendDb[0].store (0.0f);
    session.auxLane (0).params.returnLevelDb.store (0.0f);
    constexpr int length = 8192;
    const auto source = ctx.tempDir() / "finalization-source.wav";
    if (! writeMono (source, sine (440.0f, 0.125f, length)))
        return ScenarioResult::fail ("could not write the finalization source");
    placeRegion (ctx, 0, source, 0, length);
    ctx.cleanup ([&engine, disarm]
    {
        disarm();
        engine.stop();
        engine.detachAudioCallback();
        engine.getDeviceManager().closeDevice();
        engine.prepareForSelfTest (kRate, ScenarioContext::kBlockSize);
    });
    engine.reattachAudioCallback();
    const auto openError = engine.getDeviceManager().initialise (2, 2, {}, false);
    if (! ctx.expect (openError.empty(), "the private duplex device did not open: " + openError))
        return ctx.verdict();
    auto* device = engine.getDeviceManager().getCurrentDevice();
    if (! ctx.expect (device != nullptr && device->isOpen() && device->isPlaying()
                      && device->getActiveInputChannels().count() == 2,
                      "the finalization fixture is not a live duplex graph")) return ctx.verdict();

    auto next = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakNext = next;
    *next = [&ctx, &engine, &session, arm, hits, disarm, weakNext] (int pass)
    {
        if (pass == 12) { ctx.complete (ctx.verdict()); return; }
        disarm();
        const int kind = pass / 2;
        const bool inject = (pass % 2) != 0;
        const bool realtime = kind >= 4;
        const bool stems = kind == 2 || kind == 5;
        const bool freeze = kind == 3;
        const bool mp3 = kind == 1;
        const auto label = std::string (realtime ? "realtime " : "offline ")
                         + (stems ? "stems" : freeze ? "freeze" : mp3 ? "MP3 mix" : "WAV mix");
        const auto out = ctx.sessionDir() / ("finalization-" + std::to_string (pass) + (mp3 ? ".mp3" : ".wav"));
        std::vector<std::filesystem::path> outputs { out };
        if (stems)
        {
            outputs.clear();
            for (const auto& target : BounceEngine::collectStemTargets (session, sessionFile (out)))
                outputs.push_back (pathOf (target.file));
            if (! ctx.expect (outputs.size() == 2, label + ": the fixture did not name track and aux stems"))
            { ctx.complete (ctx.verdict()); return; }
        }
        auto armed = std::make_shared<bool> (false);
        auto run = std::make_shared<BounceRun>();
        run->bounce = std::make_unique<BounceEngine> (engine, session, OfflineRenderQuality::MatchPlayback);
        ctx.cleanup ([run, disarm] { disarm(); run->bounce.reset(); });
        run->bounce->onProgressUpdated = [arm, armed, inject, mp3, target = outputs.back()] (float progress)
        {
            if (inject && progress >= 1.0f && ! *armed)
            { arm (mp3 ? 5 : 3, target.c_str()); *armed = true; }
        };
        run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
        { raw->ok = ok; raw->error = std::move (error); raw->finished.store (true, std::memory_order_release); };
        const auto mode = stems ? BounceEngine::Mode::Stems : BounceEngine::Mode::MasterMix;
        const auto format = mp3 ? BounceEngine::Format::Mp3 : BounceEngine::Format::Wav;
        const bool started = freeze ? run->bounce->startFreeze (0, sessionFile (out), length, kRate, 1024)
            : run->bounce->start (sessionFile (out), kRate, 1024, 0.0, mode, format, 320, 24, realtime);
        if (! ctx.expect (started, label + ": the render refused to start"))
        { ctx.complete (ctx.verdict()); return; }
        auto self = weakNext.lock();
        ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering(); },
            kRenderTimeoutMs, [&ctx, run, self, armed, outputs, hits, disarm, pass, inject, mp3, label]
            {
                const int faultCount = hits();
                disarm();
                if (inject)
                {
                    ctx.expect (*armed && faultCount == 1, label + ": finalization did not hit the native fault");
                    ctx.expect (! run->ok && ! run->error.empty(), label + ": finalization failure reported success");
                    for (const auto& output : outputs)
                        ctx.expect (! std::filesystem::exists (output), label + ": finalization failure kept an opened output");
                }
                else
                {
                    ctx.expect (run->ok && run->error.empty(), label + ": normal finalization failed: " + run->error);
                    for (const auto& output : outputs)
                    {
                        if (mp3)
                        {
                            const auto bytes = dusk::fs::loadFileAsString (output);
                            ctx.expect (bytes.size() > 1000 && bytes.find ("Info") != std::string::npos,
                                        label + ": the MP3 header was not finalized");
                        }
                        else
                        {
                            const auto audio = readBack (output);
                            ctx.expect (audio && audio->info.numFrames == length && audio->peak() > 0.01f,
                                        label + ": the finalized WAV lost its duration or samples");
                        }
                    }
                }
                if (self) (*self) (pass + 1);
            }, label + ": the render did not finish");
    };
    (*next) (0);
    return std::nullopt;
#endif
}

// The default bounce: stereo 24-bit WAV at the session rate, as long as the
// last region plus a fixed 5 s tail, with the timeline in place from sample 0.
std::optional<ScenarioResult> masterMixFile (ScenarioContext& ctx)
{
    static constexpr std::int64_t kStart = 12000;
    static constexpr int kLength = 24000;
    const auto source = ctx.tempDir() / "tone.wav";
    if (! writeMono (source, sine (440.0f, 0.25f, kLength)))
        return ScenarioResult::fail ("could not write the source take");
    placeRegion (ctx, 0, source, kStart, kLength);

    const auto out = ctx.sessionDir() / "bounce.wav";
    renderThen (ctx, out, Render {}, [&ctx, out] (bool ok, const std::string& error)
    {
        if (! ctx.expect (ok, "the bounce failed: " + error))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        const auto mix = readBack (out);
        if (! ctx.expect (mix.has_value(), "the bounce wrote no readable file"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        const auto& info = mix->info;
        ctx.note ("rate " + std::to_string (info.sampleRate) + ", channels "
                  + std::to_string (info.numChannels) + ", bits " + std::to_string (info.bitsPerSample)
                  + ", frames " + std::to_string (info.numFrames));
        ctx.expect (std::abs (info.sampleRate - kRate) < 0.5, "the bounce is not at the session rate");
        ctx.expect (info.numChannels == 2, "the bounce is not stereo");
        ctx.expect (info.bitsPerSample == 24 && ! info.isFloat, "the bounce is not 24-bit PCM");
        ctx.expect (info.numFrames == kStart + kLength + (std::int64_t) (5.0 * kRate),
                    "the bounce is not the last region's end plus a 5 s tail");

        float before = 0.0f, during = 0.0f, tail = 0.0f;
        for (std::int64_t i = 0; i < info.numFrames; ++i)
        {
            const float s = std::abs (mix->left[(std::size_t) i]);
            if (i < kStart - 256) before = std::max (before, s);
            else if (i >= kStart + 256 && i < kStart + kLength - 256) during = std::max (during, s);
            else if (i >= kStart + kLength + 4800) tail = std::max (tail, s);
        }
        ctx.note ("peaks: before " + std::to_string (before) + ", during " + std::to_string (during)
                  + ", tail " + std::to_string (tail));
        ctx.expect (before < 1.0e-4f, "audio printed before the region starts");
        ctx.expect (during > 0.05f, "the region did not print");
        ctx.expect (tail < 1.0e-4f, "the tail is not silent after a dry tone");
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}

// Naming the bounce .mp3 picks the MP3 container.
std::optional<ScenarioResult> mp3Bounce (ScenarioContext& ctx)
{
   #if DUSKSTUDIO_HAS_LAME
    const auto source = ctx.tempDir() / "tone.wav";
    if (! writeMono (source, sine (440.0f, 0.25f, 24000)))
        return ScenarioResult::fail ("could not write the source take");
    placeRegion (ctx, 0, source, 0, 24000);

    const auto out = ctx.sessionDir() / "bounce.mp3";
    Render render;
    render.format = BounceEngine::Format::Mp3;
    render.tailSeconds = 0.5;
    renderThen (ctx, out, render, [&ctx, out] (bool ok, const std::string& error)
    {
        ctx.expect (ok, "the MP3 bounce failed: " + error);
        std::FILE* file = std::fopen (out.u8string().c_str(), "rb");
        unsigned char head[3] {};
        const auto got = file != nullptr ? std::fread (head, 1, 3, file) : 0;
        if (file != nullptr) std::fclose (file);
        const bool id3 = got == 3 && head[0] == 'I' && head[1] == 'D' && head[2] == '3';
        const bool frameSync = got >= 2 && head[0] == 0xFF && (head[1] & 0xE0) == 0xE0;
        ctx.expect (id3 || frameSync, "the .mp3 bounce does not start like an MP3 stream");
        std::error_code fsError;
        const auto size = std::filesystem::file_size (out, fsError);
        ctx.expect (! fsError && size > 4096, "the MP3 bounce is missing or nearly empty");
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
   #else
    (void) ctx;
    return noMp3Encoder();
   #endif
}

// ------------------------------------------------------------- the metronome

// The click is audible while rolling, and absent from every render.
std::optional<ScenarioResult> metronomeNeverPrints (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const bool wasEnabled = session.metronomeEnabled.load (std::memory_order_relaxed);
    const bool wasWhilePlaying = session.metronomeClickWhilePlaying.load (std::memory_order_relaxed);
    ctx.cleanup ([&session, wasEnabled, wasWhilePlaying]
    {
        session.metronomeEnabled.store (wasEnabled, std::memory_order_relaxed);
        session.metronomeClickWhilePlaying.store (wasWhilePlaying, std::memory_order_relaxed);
    });
    // Playback clicks only with "click while playing" on, and a bounce plays.
    session.metronomeEnabled.store (true, std::memory_order_relaxed);
    session.metronomeClickWhilePlaying.store (true, std::memory_order_relaxed);

    engine.play();
    const float live = ctx.pump ((int) (kRate / ScenarioContext::kBlockSize) + 2);
    engine.stop();
    ctx.note ("live peak with the click on: " + std::to_string (live));
    if (! ctx.expect (live > 0.01f, "the click did not sound while rolling, so there is nothing to keep out"))
        return ctx.verdict();

    const auto out = ctx.sessionDir() / "click.wav";
    Render render;
    render.tailSeconds = 1.0;
    renderThen (ctx, out, render, [&ctx, out] (bool ok, const std::string& error)
    {
        ctx.expect (ok, "the bounce failed: " + error);
        const auto mix = readBack (out);
        if (ctx.expect (mix.has_value() && mix->info.numFrames > 0, "the bounce wrote nothing"))
            ctx.expect (mix->peak() <= 0.0f,
                        "the click printed into the bounce (peak " + std::to_string (mix->peak()) + ")");
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}

// The bounce is taken after the master fader: 6 dB down on the master halves
// the file.
std::optional<ScenarioResult> capturesPostMasterFader (ScenarioContext& ctx)
{
    const auto source = ctx.tempDir() / "tone.wav";
    if (! writeMono (source, sine (440.0f, 0.25f, 24000)))
        return ScenarioResult::fail ("could not write the source take");
    placeRegion (ctx, 0, source, 4800, 24000);

    auto& fader = ctx.session().master().faderDb;
    const float savedFader = fader.load (std::memory_order_relaxed);
    ctx.cleanup ([&fader, savedFader] { fader.store (savedFader, std::memory_order_relaxed); });

    Render render;
    render.tailSeconds = 0.2;
    const auto unity = ctx.sessionDir() / "unity.wav";
    const auto quiet = ctx.sessionDir() / "minus6.wav";
    renderThen (ctx, unity, render, [&ctx, &fader, render, unity, quiet] (bool ok, const std::string& error)
    {
        const auto full = readBack (unity);
        if (! ctx.expect (ok && full.has_value(), "the unity bounce failed: " + error))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        const float fullPeak = full->peak();
        fader.store (-6.0206f, std::memory_order_relaxed);
        renderThen (ctx, quiet, render, [&ctx, quiet, fullPeak] (bool quietOk, const std::string& quietError)
        {
            const auto down = readBack (quiet);
            if (ctx.expect (quietOk && down.has_value(), "the -6 dB bounce failed: " + quietError))
            {
                ctx.note ("peak at 0 dB " + std::to_string (fullPeak) + ", at -6 dB "
                          + std::to_string (down->peak()));
                ctx.expect (fullPeak > 0.05f, "the unity bounce is silent");
                ctx.expect (std::abs (down->peak() - 0.5f * fullPeak) < 1.0e-3f,
                            "the bounce did not follow the master fader, so it is not taken after it");
            }
            ctx.complete (ctx.verdict());
        });
    });
    return std::nullopt;
}

// ---------------------------------------------------------------- cancel

// Cancel ends the render and leaves no truncated file behind.
std::optional<ScenarioResult> cancelLeavesNoFile (ScenarioContext& ctx)
{
    const auto source = ctx.tempDir() / "tone.wav";
    if (! writeMono (source, sine (440.0f, 0.25f, 48000)))
        return ScenarioResult::fail ("could not write the source take");
    placeRegion (ctx, 0, source, (std::int64_t) (120.0 * kRate), 48000);

    const auto out = ctx.sessionDir() / "cancelled.wav";
    auto run = startRender (ctx, out, Render {});
    if (run == nullptr) return std::nullopt;
    run->bounce->cancel();
    ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire)
                                  && ! run->bounce->isRendering(); },
                   kRenderTimeoutMs,
                   [&ctx, run, out]
                   {
                       ctx.expect (! run->ok, "a cancelled bounce reported success");
                       ctx.expect (run->error == BounceEngine::kCancelledError,
                                   "a cancelled bounce reported '" + run->error + "'");
                       std::error_code fsError;
                       ctx.expect (! std::filesystem::exists (out, fsError),
                                   "a cancelled bounce left its file behind");
                       ctx.complete (ctx.verdict());
                   },
                   "the cancelled bounce never finished");
    return std::nullopt;
}

std::optional<ScenarioResult> wholeSongIgnoresPlaybackLoop (ScenarioContext& ctx)
{
    static constexpr int length = 32000;
    static constexpr std::int64_t loopStart = 8000, loopEnd = 16000, savedPlayhead = 777;
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const bool loopWas = transport.isLoopEnabled();
    const auto startWas = transport.getLoopStart(), endWas = transport.getLoopEnd();
    const auto playheadWas = transport.getPlayhead();
    ctx.cleanup ([&transport, loopWas, startWas, endWas, playheadWas]
    {
        transport.setLoopEnabled (false);
        transport.setLoopRange (startWas, endWas);
        transport.setPlayhead (playheadWas);
        transport.setLoopEnabled (loopWas);
    });
    ctx.keep (ctx.session().oversamplingFactor);
    ctx.session().oversamplingFactor.store (1, std::memory_order_relaxed);

    std::vector<float> samples (length, 0.0f);
    samples[4096] = 0.125f;
    samples[12288] = -0.25f;
    samples[24576] = 0.5f;
    const auto source = ctx.tempDir() / "linear-song.wav";
    if (! writeMono (source, samples))
        return ScenarioResult::fail ("could not write the three-impulse song");
    placeRegion (ctx, 0, source, 0, length);

    struct Observed
    {
        int blocks = 0;
        bool looped = false;
        bool wentBackwards = false;
        bool restoredAtFinish = false;
        std::int64_t previousPosition = -1;
    };
    auto reference = std::make_shared<std::array<std::optional<Rendered>, 3>>();
    auto next = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakNext = next;
    *next = [&ctx, &engine, &transport, reference, weakNext] (int pass)
    {
        if (pass == 12) { ctx.complete (ctx.verdict()); return; }
        const int kind = pass / 4, leg = pass % 4;
        const bool loopEnabled = leg != 0;
        const auto mode = kind == 0 ? BounceEngine::Mode::MasterMix
                        : kind == 1 ? BounceEngine::Mode::Stems : BounceEngine::Mode::FreezeTrack;
        const auto label = std::string (kind == 0 ? "mix" : kind == 1 ? "stems" : "freeze")
                         + (leg == 0 ? ", loop off" : leg == 1 ? ", loop on"
                                                 : leg == 2 ? ", cancelled" : ", refused target");
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (loopEnabled);
        transport.setPlayhead (savedPlayhead);
        const auto out = ctx.sessionDir() / ("linear-" + std::to_string (pass) + ".wav");
        auto output = out;
        if (mode == BounceEngine::Mode::Stems)
        {
            const auto targets = BounceEngine::collectStemTargets (ctx.session(), sessionFile (out));
            if (! ctx.expect (targets.size() == 1, "the loop fixture did not name one track stem"))
            { ctx.complete (ctx.verdict()); return; }
            output = pathOf (targets.front().file);
        }
        if (leg == 3)
        {
            std::error_code error;
            if (! std::filesystem::create_directory (output, error))
            { ctx.complete (ScenarioResult::fail ("could not block the render target")); return; }
        }

        auto observed = std::make_shared<Observed>();
        auto run = std::make_shared<BounceRun>();
        run->bounce = std::make_unique<BounceEngine> (engine, ctx.session(), OfflineRenderQuality::MatchPlayback);
        ctx.cleanup ([run] { run->bounce.reset(); });
        run->bounce->onProgressUpdated = [&transport, observed, raw = run->bounce.get(), leg] (float)
        {
            ++observed->blocks;
            observed->looped |= transport.isLoopEnabled();
            const auto position = transport.getCommittedPlayhead();
            observed->wentBackwards |= position < observed->previousPosition;
            observed->previousPosition = position;
            if (leg == 2) raw->cancel();
        };
        run->bounce->onFinished = [raw = run.get(), observed, &transport, loopEnabled] (bool ok, std::string error)
        {
            observed->restoredAtFinish = transport.isLoopEnabled() == loopEnabled
                && transport.getLoopStart() == loopStart && transport.getLoopEnd() == loopEnd;
            raw->ok = ok;
            raw->error = std::move (error);
            raw->finished.store (true, std::memory_order_release);
        };
        const bool started = mode == BounceEngine::Mode::FreezeTrack
            ? run->bounce->startFreeze (0, sessionFile (out), length, kRate, 1024)
            : run->bounce->start (sessionFile (out), kRate, 1024, 0.0, mode);
        if (! ctx.expect (started, label + ": the render did not start"))
        { ctx.complete (ctx.verdict()); return; }
        auto self = weakNext.lock();
        ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire)
                                      && ! run->bounce->isRendering(); }, kRenderTimeoutMs,
            [&ctx, &transport, reference, self, run, observed, pass, kind, leg, loopEnabled, output, label]
            {
                ctx.expect (observed->restoredAtFinish, label + ": loop state/range was not restored before completion");
                ctx.expect (transport.isLoopEnabled() == loopEnabled
                            && transport.getLoopStart() == loopStart && transport.getLoopEnd() == loopEnd,
                            label + ": the render changed the loop state/range");
                ctx.expect (transport.getPlayhead() == savedPlayhead, label + ": the render changed the playhead");
                if (leg < 2)
                {
                    const auto audio = readBack (output);
                    if (! ctx.expect (run->ok && audio.has_value(), label + ": render failed: " + run->error))
                    { ctx.complete (ctx.verdict()); return; }
                    ctx.expect (audio->info.numFrames == length, label + ": the whole song was not rendered");
                    for (const int at : { 4096, 12288, 24576 })
                    {
                        float peak = 0.0f;
                        for (int i = at - 256; i < at + 256 && i < (int) audio->left.size(); ++i)
                            peak = std::max (peak, std::abs (audio->left[(std::size_t) i]));
                        ctx.expect (peak > 0.01f, label + ": impulse missing near sample " + std::to_string (at));
                    }
                    if (leg == 0) (*reference)[(std::size_t) kind] = *audio;
                    else
                    {
                        const auto& expected = *(*reference)[(std::size_t) kind];
                        float difference = 0.0f;
                        for (std::size_t i = 0; i < std::min (audio->left.size(), expected.left.size()); ++i)
                            difference = std::max ({ difference, std::abs (audio->left[i] - expected.left[i]),
                                                                  std::abs (audio->right[i] - expected.right[i]) });
                        ctx.note (label + ": max sample difference " + std::to_string (difference));
                        ctx.expect (difference < 1.0e-5f, label + ": enabling the playback loop changed the rendered samples");
                    }
                }
                else
                {
                    ctx.expect (! run->ok, label + ": an incomplete render reported success");
                    ctx.expect (leg == 2 ? run->error == BounceEngine::kCancelledError : ! run->error.empty(),
                                label + ": the render returned the wrong error");
                }
                if (leg != 3)
                {
                    ctx.expect (observed->blocks > 0, label + ": no block reached the render path");
                    ctx.expect (! observed->looped, label + ": the render exposed the playback loop to the engine");
                    ctx.expect (! observed->wentBackwards, label + ": committed plugin/timeline position wrapped");
                }
                if (self != nullptr) (*self) (pass + 1);
            }, "the loop-isolation render never finished");
    };
    (*next) (0);
    return std::nullopt;
}

std::optional<ScenarioResult> offlineMidiIsolation (ScenarioContext& ctx)
{
    static constexpr int length = 32000;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& strip = session.track (0).strip;
    const int input = engine.getVirtualKeyboardInputIndex();
    if (input < 0) return ScenarioResult::fail ("the virtual MIDI input is missing");
    const bool hardware = engine.setHardwareMidiInputsEnabledForScenario (false);
    const bool takeover = engine.getMidiSoftTakeover();
    engine.setMidiSoftTakeover (false);
    ctx.cleanup ([&engine, hardware, takeover]
    {
        engine.setOfflineRenderActive (false);
        engine.setHardwareMidiInputsEnabledForScenario (hardware);
        engine.setMidiSoftTakeover (takeover);
    });
    ctx.keep (session.oversamplingFactor); ctx.keep (session.tempoBpm);
    ctx.keep (strip.faderDb); ctx.keep (strip.pan);
    ctx.keep (session.syncSourceInputIdx); ctx.keep (session.syncOutputIdx);
    ctx.keep (session.externalSyncFollowsTempo); ctx.keep (session.externalSyncChasesTransport);
    ctx.keep (session.externalTimeCodeChasesTransport);
    ctx.keep (session.syncOutputEmitClock); ctx.keep (session.syncOutputEmitTimeCode);
    ctx.keep (session.mcu.resolvedInputIdx); ctx.keep (session.mcu.resolvedOutputIdx);
    ctx.keep (session.mcu.bank);
    ctx.keep (session.pendingTransportAction); ctx.keep (session.pendingTransportPlayhead);
    ctx.keep (session.externalBpm); ctx.keep (session.externalSyncRolling);
    ctx.keep (session.externalTimeCodeFrames); ctx.keep (session.externalTimeCodeRolling);
    ctx.keep (session.externalTimeCodeReversed); ctx.keep (session.externalTimeCodeFrameRate);
    ctx.keep (session.track (1).midiOutputIndex);
    ctx.keep (session.heldMidiNotes[60]);
    session.oversamplingFactor.store (1);
    session.syncSourceInputIdx.store (input);
    session.externalSyncFollowsTempo.store (true);
    session.externalSyncChasesTransport.store (true);
    session.externalTimeCodeChasesTransport.store (true);
    session.syncOutputIdx.store (1);
    session.syncOutputEmitClock.store (true);
    session.syncOutputEmitTimeCode.store (true);
    session.mcu.resolvedInputIdx.store (input);
    session.mcu.resolvedOutputIdx.store (-1);
    session.mcu.bank.store (0);
    session.track (1).mode.store ((int) Track::Mode::Midi);
    session.track (1).midiOutputIndex.store (0);
    MidiRegion region;
    region.timelineStart = 0;
    region.lengthInSamples = length;
    region.lengthInTicks = kMidiTicksPerQuarter;
    region.notes.push_back ({ 1, 64, 100, 0, kMidiTicksPerQuarter / 4 });
    session.track (1).midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, region));
    std::vector<MidiBinding> bindings;
    if (const auto* current = session.midiBindings.read()) bindings = *current;
    ctx.cleanup ([&session, bindings]
    { session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (bindings)); });
    MidiBinding binding;
    binding.trigger = MidiBindingTrigger::CC;
    binding.channel = 1;
    binding.dataNumber = 23;
    binding.target = MidiBindingTarget::TrackPan;
    binding.targetIndex = 0;
    session.midiBindings.publish (std::make_unique<std::vector<MidiBinding>> (1, binding));
    const auto source = ctx.tempDir() / "isolated-midi.wav";
    if (! writeMono (source, sine (440.0f, 0.1f, length)))
        return ScenarioResult::fail ("could not write the MIDI isolation source");
    placeRegion (ctx, 0, source, 0, length);
    auto owned = std::make_unique<RecordingMidiBackend> (2, 32768);
    auto* recorder = owned.get();
    engine.installMidiOutputBackend (std::move (owned));
    if (! engine.ensureMidiOutputOpen (0) || ! engine.ensureMidiOutputOpen (1))
        return ScenarioResult::fail ("the MIDI isolation outputs would not open");
    const auto traffic = []
    {
        dusk::MidiBuffer events;
        const std::uint8_t start = 0xFA, clock = 0xF8;
        const std::uint8_t fader[] { 0xE0, 0, 0 }, pan[] { 0xB0, 23, 127 }, note[] { 0x90, 60, 100 };
        events.addEvent (&start, 1, 0);
        events.addEvent (fader, 3, 0); events.addEvent (pan, 3, 0); events.addEvent (note, 3, 72);
        for (int n = 0; n < 8; ++n)
        {
            const std::uint8_t qf[] { 0xF1, (std::uint8_t) ((n << 4) | (n == 7 ? 6 : 0)) };
            events.addEvent (qf, 2, n * 8);
        }
        events.addEvent (&clock, 1, 64); events.addEvent (&clock, 1, 192);
        return events;
    };
    auto settle = std::make_shared<std::function<void (std::function<void()>)>>();
    *settle = [&ctx, recorder] (std::function<void()> done)
    {
        auto last = std::make_shared<std::pair<std::size_t, int>> (recorder->count(), 0);
        ctx.waitUntil ([last, recorder]
        {
            const auto count = recorder->count();
            last->second = count == last->first ? last->second + 1 : 0;
            last->first = count;
            return last->second >= 3;
        }, 5000, std::move (done), "the MIDI isolation outputs did not settle");
    };
    auto reference = std::make_shared<std::array<std::vector<Rendered>, 3>>();
    auto next = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakNext = next;
    *next = [&ctx, &engine, &session, &transport, &strip, input, recorder, traffic, settle, reference, weakNext] (int pass)
    {
        if (pass == 6)
        {
            (*settle) ([&ctx, &engine, &session, &transport, &strip, input, recorder, traffic, settle]
            {
                ctx.expect (recorder->count() == 0, "offline renders sent physical MIDI notes or sync bytes");
                session.tempoBpm.store (120.0f); strip.faderDb.store (0.0f); strip.pan.store (0.0f);
                session.pendingTransportAction.store ((int) PendingTransportAction::None);
                session.pendingTransportPlayhead.store (-1);
                ctx.pump (1);
                transport.setPlayhead (0);
                engine.play();
                ctx.pumpWithMidi (input, traffic());
                ctx.expect (session.tempoBpm.load() > 200.0f, "realtime MIDI Clock did not follow tempo");
                ctx.expect (strip.faderDb.load() < -40.0f, "realtime MCU fader input was suppressed");
                ctx.expect (strip.pan.load() > 0.9f, "realtime controller binding was suppressed");
                ctx.expect (session.externalTimeCodeRolling.load(), "realtime MTC input was suppressed");
                ctx.pump (4);
                (*settle) ([&ctx, &engine, &session, &transport, &strip, input, recorder, traffic, settle]
                {
                    bool notes = false, clock = false, timecode = false;
                    for (const auto& message : recorder->captured())
                    {
                        notes |= message.port == 0 && (message.bytes[0] & 0xF0) == 0x90;
                        clock |= message.port == 1 && message.bytes[0] == 0xF8;
                        timecode |= message.port == 1 && (message.bytes[0] == 0xF1 || message.bytes[0] == 0xF0);
                    }
                    ctx.expect (notes && clock && timecode, "realtime physical MIDI/sync output was suppressed");
                    const auto sent = recorder->count();
                    engine.setOfflineRenderActive (true);
                    session.tempoBpm.store (120.0f); strip.faderDb.store (0.0f); strip.pan.store (0.0f);
                    session.pendingTransportAction.store ((int) PendingTransportAction::None);
                    session.pendingTransportPlayhead.store (-1);
                    const auto frames = session.externalTimeCodeFrames.load();
                    ctx.pumpWithMidi (input, traffic());
                    ctx.pump (200);
                    ctx.expect (session.externalTimeCodeRolling.load() && session.externalTimeCodeFrames.load() == frames,
                                "offline blocks advanced the external MTC receiver/watchdog");
                    ctx.expect (session.pendingTransportAction.load() == (int) PendingTransportAction::None,
                                "offline blocks queued an external transport action");
                    ctx.expect (session.tempoBpm.load() == 120.0f && strip.faderDb.load() == 0.0f && strip.pan.load() == 0.0f,
                                "offline ownership admitted live control changes");
                    (*settle) ([&ctx, &engine, &session, &transport, &strip, recorder, sent]
                    {
                        ctx.expect (recorder->count() == sent, "offline ownership sent physical MIDI/sync output");
                        session.syncOutputEmitClock.store (false); session.syncOutputEmitTimeCode.store (false);
                        session.track (1).midiOutputIndex.store (-1);
                        const auto position = transport.getPlayhead();
                        session.pendingTransportAction.store ((int) PendingTransportAction::SyncStop);
                        session.pendingTransportPlayhead.store (45000);
                        engine.setOfflineRenderActive (false);
                        const auto serviced = engine.serviceTransportRequests();
                        ctx.expect (! serviced.stopped && transport.isPlaying() && transport.getPlayhead() == position,
                                    "a live transport request escaped the end of offline ownership");
                        ctx.expect (session.pendingTransportAction.load() == (int) PendingTransportAction::None
                                    && session.pendingTransportPlayhead.load() == -1,
                                    "the offline ownership boundary retained a stale transport request");
                        ctx.pump (1);
                        ctx.expect (strip.faderDb.load() == 0.0f && strip.pan.load() == 0.0f,
                                    "discarded MIDI controls burst after offline rendering");
                        ctx.expect (! recorder->hasDropped(), "the MIDI isolation recorder overflowed");
                        ctx.complete (ctx.verdict());
                    });
                });
            });
            return;
        }
        const int kind = pass / 2;
        const bool inject = (pass % 2) != 0;
        const auto label = std::string (kind == 0 ? "mix" : kind == 1 ? "stems" : "freeze")
                         + (inject ? ", live traffic" : ", reference");
        session.tempoBpm.store (120.0f); strip.faderDb.store (0.0f); strip.pan.store (0.0f);
        session.heldMidiNotes[60].store (false);
        session.pendingTransportAction.store ((int) PendingTransportAction::None);
        session.pendingTransportPlayhead.store (-1);
        transport.setPlayhead (777);
        const auto out = ctx.sessionDir() / ("midi-" + std::to_string (pass) + ".wav");
        std::vector<std::filesystem::path> outputs { out };
        const auto mode = kind == 0 ? BounceEngine::Mode::MasterMix
                        : kind == 1 ? BounceEngine::Mode::Stems : BounceEngine::Mode::FreezeTrack;
        if (kind == 1)
        {
            outputs.clear();
            for (const auto& target : BounceEngine::collectStemTargets (session, sessionFile (out)))
                outputs.push_back (pathOf (target.file));
        }
        struct Progress
        {
            std::atomic<bool> reached { false };
            dusk::AutoResetEvent gate;
            bool changed = false;
        };
        auto observed = std::make_shared<Progress>();
        auto run = std::make_shared<BounceRun>();
        run->bounce = std::make_unique<BounceEngine> (engine, session, OfflineRenderQuality::MatchPlayback);
        ctx.cleanup ([run, observed] { observed->gate.signal(); run->bounce.reset(); });
        run->bounce->onProgressUpdated = [&engine, &session, &strip, observed, inject, input, traffic] (float progress)
        {
            observed->changed |= session.tempoBpm.load() != 120.0f || strip.faderDb.load() != 0.0f
                || strip.pan.load() != 0.0f || session.heldMidiNotes[60].load();
            if (inject && ! observed->reached.exchange (true, std::memory_order_acq_rel))
                observed->gate.wait (5000);
            if (inject && progress < 1.0f) engine.stageTestMidiInjection (input, traffic());
        };
        run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
        { raw->ok = ok; raw->error = std::move (error); raw->finished.store (true, std::memory_order_release); };
        if (inject) engine.stageTestMidiInjection (input, traffic());
        const bool started = kind == 2 ? run->bounce->startFreeze (0, sessionFile (out), length, kRate, 1024)
            : run->bounce->start (sessionFile (out), kRate, 1024, 0.0, mode);
        if (! ctx.expect (started, label + ": render did not start")) { ctx.complete (ctx.verdict()); return; }
        auto self = weakNext.lock();
        const auto waitForFinish = [&ctx, &transport, observed, run, reference, self, outputs, kind, inject, pass, label]
        {
            ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering(); },
                kRenderTimeoutMs, [&ctx, &transport, observed, run, reference, self, outputs, kind, inject, pass, label]
                {
                    ctx.expect (run->ok, label + ": render failed: " + run->error);
                    ctx.expect (! observed->changed, label + ": live MIDI changed the render controls");
                    ctx.expect (transport.getPlayhead() == 777 && transport.isStopped(), label + ": transport was not restored");
                    for (std::size_t file = 0; file < outputs.size(); ++file)
                    {
                        const auto audio = readBack (outputs[file]);
                        if (! ctx.expect (audio.has_value(), label + ": output could not be read")) continue;
                        if (! inject) (*reference)[(std::size_t) kind].push_back (*audio);
                        else if (file < (*reference)[(std::size_t) kind].size())
                        {
                            const auto& expected = (*reference)[(std::size_t) kind][file];
                            float difference = 0.0f;
                            for (std::size_t n = 0; n < std::min (audio->left.size(), expected.left.size()); ++n)
                                difference = std::max ({ difference, std::abs (audio->left[n] - expected.left[n]),
                                                                      std::abs (audio->right[n] - expected.right[n]) });
                            ctx.expect (audio->info.numFrames == expected.info.numFrames && difference < 1.0e-5f,
                                        label + ": live input changed the rendered samples");
                        }
                    }
                    if (self) (*self) (pass + 1);
                }, "the MIDI isolation render did not finish");
        };
        if (! inject) { waitForFinish(); return; }
        ctx.waitUntil ([observed] { return observed->reached.load (std::memory_order_acquire); }, kRenderTimeoutMs,
            [&ctx, &engine, &session, &transport, observed, waitForFinish, label]
            {
                const auto position = transport.getPlayhead();
                session.pendingTransportAction.store ((int) PendingTransportAction::SyncStop);
                session.pendingTransportPlayhead.store (45000);
                const auto result = engine.serviceTransportRequests();
                ctx.expect (! result.stopped && ! result.recordRequested && transport.isPlaying()
                            && transport.getPlayhead() == position, label + ": message-thread transport service interrupted the render");
                ctx.expect (session.pendingTransportAction.load() == (int) PendingTransportAction::None
                            && session.pendingTransportPlayhead.load() == -1,
                            label + ": suppressed transport requests were left queued");
                observed->gate.signal();
                waitForFinish();
            }, "the MIDI isolation render did not reach its first block");
    };
    (*next) (0);
    return std::nullopt;
}

ScenarioResult hardwareOfflineCallbackProbe (ScenarioContext& ctx)
{
    constexpr int frames = 256;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    auto& trackHardware = session.track (0).hardwareInsert;
    auto& auxHardware = session.auxLane (0).hardwareInserts[0];
    std::array<std::array<float, frames>, 2> input {};
    std::array<std::array<float, frames>, 8> output {};
    std::array<const float*, 2> inputs {};
    std::array<float*, 8> outputs {};
    std::array<std::array<float, frames>, 4> taps {};
    for (int channel = 0; channel < 2; ++channel)
    {
        for (int sample = 0; sample < frames; ++sample)
            input[(std::size_t) channel][(std::size_t) sample] = (channel == 0 ? 0.125f : -0.0625f)
                * std::sin (2.0f * kPi * (float) sample / 32.0f);
        inputs[(std::size_t) channel] = input[(std::size_t) channel].data();
    }
    for (int channel = 0; channel < 8; ++channel)
        outputs[(std::size_t) channel] = output[(std::size_t) channel].data();
    auto& strip = engine.getChannelStrip (0);
    strip.setStemCapture (taps[0].data(), taps[1].data());
    engine.setAuxStemCapture (0, taps[2].data(), taps[3].data());
    ctx.cleanup ([&engine, &strip]
    {
        strip.setStemCapture (nullptr, nullptr);
        engine.setAuxStemCapture (0, nullptr, nullptr);
        engine.setOfflineRenderActive (false);
        engine.stop();
    });
    engine.setOfflineRenderActive (true);
    engine.prepareForSelfTest (kRate, frames);
    transport.setPlayhead (0);
    transport.setState (Transport::State::Playing);
    engine.getPlaybackEngine().preparePlayback();
    trackHardware.pingPending.store (true); auxHardware.pingPending.store (true);
    trackHardware.pingResult.store (731); auxHardware.pingResult.store (731);
    device::CallbackContext offline;
    offline.offlineRender = true;
    float sendPeak = 0.0f, dryPeak = 0.0f;
    for (int block = 0; block < 256; ++block)
    {
        for (auto& channel : taps) channel.fill (0.0f);
        engine.getPlaybackEngine().primeAt (transport.getPlayhead());
        engine.audioDeviceIOCallback (inputs.data(), 2, outputs.data(), 8, frames, offline);
        for (int channel = 2; channel < 6; ++channel)
            for (const float sample : output[(std::size_t) channel]) sendPeak = std::max (sendPeak, std::abs (sample));
        for (const auto& channel : taps)
            for (const float sample : channel) dryPeak = std::max (dryPeak, std::abs (sample));
    }
    ctx.expect (sendPeak < 1.0e-8f, "offline callbacks emitted a hardware send or ping");
    ctx.expect (dryPeak > 0.01f, "offline callbacks did not process dry audio");
    ctx.expect (trackHardware.pingPending.load() && auxHardware.pingPending.load()
                && trackHardware.pingResult.load() == 731 && auxHardware.pingResult.load() == 731,
                "offline callbacks advanced the hardware ping state");
    engine.stop();
    engine.setOfflineRenderActive (false);
    trackHardware.pingPending.store (false); auxHardware.pingPending.store (false);
    engine.prepareForSelfTest (kRate, frames);
    transport.setPlayhead (0);
    engine.play();
    std::array<float, 2> liveSendPeaks {};
    float wetPeak = 0.0f;
    for (int block = 0; block < 16; ++block)
    {
        for (auto& channel : taps) channel.fill (0.0f);
        engine.getPlaybackEngine().primeAt (transport.getPlayhead());
        engine.audioDeviceIOCallback (inputs.data(), 2, outputs.data(), 8, frames, {});
        for (int channel = 2; channel < 6; ++channel)
            for (const float sample : output[(std::size_t) channel])
                liveSendPeaks[(std::size_t) (channel - 2) / 2] = std::max (liveSendPeaks[(std::size_t) (channel - 2) / 2], std::abs (sample));
        if (block > 8)
            for (const auto& channel : taps)
                for (const float sample : channel) wetPeak = std::max (wetPeak, std::abs (sample));
    }
    ctx.expect (liveSendPeaks[0] > 0.01f && liveSendPeaks[1] > 0.01f && wetPeak > 0.01f,
                "realtime hardware sends or returns were bypassed");
    trackHardware.pingPending.store (true); auxHardware.pingPending.store (true);
    float pingPeak = 0.0f;
    for (int block = 0; block < 8; ++block)
    {
        engine.getPlaybackEngine().primeAt (transport.getPlayhead());
        engine.audioDeviceIOCallback (inputs.data(), 2, outputs.data(), 8, frames, {});
        for (int channel = 2; channel < 6; ++channel)
            for (const float sample : output[(std::size_t) channel]) pingPeak = std::max (pingPeak, std::abs (sample));
    }
    ctx.expect (pingPeak > 0.1f, "hardware pings did not resume in realtime");
    strip.setStemCapture (nullptr, nullptr);
    engine.setAuxStemCapture (0, nullptr, nullptr);
    engine.stop();
    return ctx.verdict();
}

std::optional<ScenarioResult> offlineHardwareIsDry (ScenarioContext& ctx)
{
    static constexpr int length = 65536;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& strip = engine.getChannelStrip (0);
    auto& aux = engine.getAuxLaneStrip (0);
    auto& transport = engine.getTransport();
    const bool loopWas = transport.isLoopEnabled();
    const auto playheadWas = transport.getPlayhead();
    ctx.cleanup ([&engine, &transport, loopWas, playheadWas]
    {
        engine.stop();
        transport.setLoopEnabled (loopWas);
        transport.setPlayhead (playheadWas);
    });
    transport.setLoopEnabled (false);
    ctx.keep (session.oversamplingFactor);
    ctx.keep (session.track (0).strip.auxSendDb[0]);
    ctx.keep (session.auxLane (0).params.returnLevelDb);
    ctx.keep (session.auxLane (0).params.outputPair);
    ctx.keep (strip.insertMode); ctx.keep (aux.insertMode[0]);
    session.oversamplingFactor.store (1);
    session.track (0).strip.auxSendDb[0].store (0.0f);
    session.auxLane (0).params.returnLevelDb.store (0.0f);
    session.auxLane (0).params.outputPair.store (-1);
    std::array<HardwareInsertParams*, 2> hardware { &session.track (0).hardwareInsert,
                                                 &session.auxLane (0).hardwareInserts[0] };
    for (std::size_t index = 0; index < hardware.size(); ++index)
    {
        auto& params = *hardware[index];
        const auto saved = params.routing.current();
        ctx.cleanup ([&params, saved]
        { params.routing.publish (std::make_unique<HardwareInsertRouting> (saved)); });
        ctx.keep (params.enabled); ctx.keep (params.dryWet);
        ctx.keep (params.outputGainDb); ctx.keep (params.inputGainDb);
        ctx.keep (params.pingPending); ctx.keep (params.pingResult);
        HardwareInsertRouting routing;
        routing.outputChL = 2 + 2 * (int) index; routing.outputChR = 3 + 2 * (int) index;
        routing.inputChL = 0; routing.inputChR = 1;
        routing.latencySamples = index == 0 ? 577 : 911;
        params.routing.publish (std::make_unique<HardwareInsertRouting> (routing));
        params.outputGainDb.store (6.0f); params.inputGainDb.store (-6.0f);
        params.dryWet.store (1.0f);
    }
    const auto source = ctx.tempDir() / "hardware-dry-source.wav";
    std::vector<float> samples (length, 0.0f);
    samples[128] = 0.125f; samples[24576] = -0.25f; samples[length - 128] = 0.5f;
    if (! writeMono (source, samples)) return ScenarioResult::fail ("could not write the dry hardware source");
    placeRegion (ctx, 0, source, 0, length);
    auto reference = std::make_shared<std::array<std::vector<Rendered>, 3>>();
    auto next = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakNext = next;
    *next = [&ctx, &engine, &session, &strip, &aux, hardware, reference, weakNext] (int pass)
    {
        if (pass == 9) { ctx.complete (hardwareOfflineCallbackProbe (ctx)); return; }
        const int kind = pass / 3, leg = pass % 3;
        const bool enabled = leg != 0;
        strip.insertMode.store (enabled ? ChannelStrip::kInsertHardware : ChannelStrip::kInsertEmpty);
        aux.insertMode[0].store (enabled ? AuxLaneStrip::kInsertHardware : AuxLaneStrip::kInsertEmpty);
        for (auto* params : hardware)
        {
            params->enabled.store (enabled);
            params->dryWet.store (leg == 2 ? 1.0f : 0.0f);
            params->pingPending.store (enabled);
            params->pingResult.store (731);
        }
        const auto out = ctx.sessionDir() / ("hardware-dry-" + std::to_string (pass) + ".wav");
        const auto mode = kind == 0 ? BounceEngine::Mode::MasterMix
                        : kind == 1 ? BounceEngine::Mode::Stems : BounceEngine::Mode::FreezeTrack;
        std::vector<std::filesystem::path> outputs { out };
        if (kind == 1)
        {
            outputs.clear();
            for (const auto& target : BounceEngine::collectStemTargets (session, sessionFile (out)))
                outputs.push_back (pathOf (target.file));
        }
        auto run = std::make_shared<BounceRun>();
        run->bounce = std::make_unique<BounceEngine> (engine, session, OfflineRenderQuality::MatchPlayback);
        ctx.cleanup ([run] { run->bounce.reset(); });
        run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
        { raw->ok = ok; raw->error = std::move (error); raw->finished.store (true, std::memory_order_release); };
        const bool started = kind == 2 ? run->bounce->startFreeze (0, sessionFile (out), length, kRate, 1024)
            : run->bounce->start (sessionFile (out), kRate, 1024, 0.0, mode);
        if (! started) { ctx.complete (ScenarioResult::fail ("the dry hardware render did not start")); return; }
        auto self = weakNext.lock();
        ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering(); },
            kRenderTimeoutMs, [&ctx, reference, self, run, outputs, hardware, enabled, kind, leg, pass]
            {
                const auto label = std::string (kind == 0 ? "mix" : kind == 1 ? "stems" : "freeze")
                    + (leg == 0 ? ", no insert" : leg == 1 ? ", delayed dry" : ", offline full wet");
                ctx.expect (run->ok, label + ": dry hardware render failed: " + run->error);
                for (std::size_t file = 0; file < outputs.size(); ++file)
                {
                    const auto audio = readBack (outputs[file]);
                    if (! ctx.expect (audio.has_value(), label + ": rendered file is missing")) continue;
                    ctx.expect (audio->info.numFrames == length, label + ": hardware changed the render length");
                    for (const int at : { 128, 24576, length - 128 })
                    {
                        float peak = 0.0f;
                        int peakAt = -1;
                        for (int n = at - 64; n < at + 64 && n < (int) audio->left.size(); ++n)
                            if (const float magnitude = std::abs (audio->left[(std::size_t) n]); magnitude > peak)
                            { peak = magnitude; peakAt = n; }
                        ctx.expect (peak > (leg == 0 ? 0.0001f : 0.01f), label + ": hardware lost a dry head/tail impulse");
                        ctx.expect (peakAt == at, label + ": dry impulse landed at " + std::to_string (peakAt)
                                                + " instead of " + std::to_string (at));
                    }
                    if (leg == 1) (*reference)[(std::size_t) kind].push_back (*audio);
                    else if (leg == 2 && ctx.expect (file < (*reference)[(std::size_t) kind].size(), "missing dry reference"))
                    {
                        const auto& expected = (*reference)[(std::size_t) kind][file];
                        float difference = 0.0f;
                        for (std::size_t n = 0; n < std::min (audio->left.size(), expected.left.size()); ++n)
                            difference = std::max ({ difference, std::abs (audio->left[n] - expected.left[n]),
                                                                  std::abs (audio->right[n] - expected.right[n]) });
                        ctx.expect (difference < 1.0e-5f, label + ": positive hardware latency changed aligned dry samples");
                    }
                }
                if (enabled)
                    for (auto* params : hardware)
                        ctx.expect (params->pingPending.load() && params->pingResult.load() == 731,
                                    label + ": offline rendering advanced a hardware ping");
                if (self) (*self) (pass + 1);
            }, "the dry hardware render did not finish");
    };
    (*next) (0);
    return std::nullopt;
}

std::optional<ScenarioResult> offlineCueDoesNotPrint (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    auto& aux = session.auxLane (0).params;
    ctx.keep (session.oversamplingFactor);
    ctx.keep (session.track (0).strip.auxSendDb[0]);
    ctx.keep (aux.returnLevelDb);
    ctx.keep (aux.mute);
    ctx.keep (aux.outputPair);
    session.oversamplingFactor.store (1);
    session.track (0).strip.auxSendDb[0].store (0.0f);
    aux.returnLevelDb.store (-6.0f);
    const auto source = ctx.tempDir() / "cue-source.wav";
    if (! writeMono (source, sine (440.0f, 0.125f, 32768)))
        return ScenarioResult::fail ("could not write the cue source");
    placeRegion (ctx, 0, source, 0, 32768);
    auto references = std::make_shared<std::array<std::optional<Rendered>, 2>>();
    auto next = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakNext = next;
    *next = [&ctx, &aux, references, weakNext] (int pass)
    {
        if (pass == 4) { ctx.complete (ctx.verdict()); return; }
        const bool muted = pass >= 2;
        const bool routed = (pass % 2) != 0;
        aux.mute.store (muted);
        aux.outputPair.store (routed ? 2 : -1);
        Render render;
        render.tailSeconds = 0.0;
        render.wavBitDepth = 32;
        render.quality = OfflineRenderQuality::MatchPlayback;
        const auto out = ctx.sessionDir() / ("cue-" + std::to_string (pass) + ".wav");
        auto self = weakNext.lock();
        renderThen (ctx, out, render, [&ctx, references, self, out, pass, muted, routed]
            (bool ok, const std::string& error)
        {
            if (! ctx.expect (ok, "cue comparison render failed: " + error))
            { ctx.complete (ctx.verdict()); return; }
            auto rendered = readBack (out);
            if (! ctx.expect (rendered.has_value(), "cue comparison file was unreadable"))
            { ctx.complete (ctx.verdict()); return; }
            auto& reference = (*references)[muted ? 1 : 0];
            if (! routed)
            {
                ctx.expect (rendered->peak() > 0.01f, "cue reference was silent");
                reference = std::move (*rendered);
            }
            else if (ctx.expect (reference && reference->left.size() == rendered->left.size(),
                                  "cue routing changed the render length"))
            {
                float difference = 0.0f;
                for (std::size_t i = 0; i < rendered->left.size(); ++i)
                {
                    difference = std::max (difference, std::abs (rendered->left[i] - reference->left[i]));
                    difference = std::max (difference, std::abs (rendered->right[i] - reference->right[i]));
                }
                ctx.expect (difference < 1.0e-6f, muted
                    ? "a muted aux's physical cue printed into the offline mix"
                    : "an aux's physical cue printed into the offline mix");
            }
            (*self) (pass + 1);
        });
    };
    (*next) (0);
    return std::nullopt;
}

std::optional<ScenarioResult> freezeIgnoresMixGate (ScenarioContext& ctx)
{
    static constexpr int length = 32768, blockSize = 1024;
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& params = session.track (0).strip;
    auto& strip = engine.getChannelStrip (0);
    ctx.cleanup ([&engine, &session, &strip]
    {
        strip.setStemCapture (nullptr, nullptr);
        engine.setBusStemCapture (1, nullptr, nullptr);
        engine.setAuxStemCapture (0, nullptr, nullptr);
        session.recomputeRtCounters();
    });
    ctx.keep (session.oversamplingFactor); ctx.keep (params.mute);
    ctx.keep (params.auxSendDb[0]);
    for (int bus = 0; bus < Session::kNumBuses; ++bus)
    {
        ctx.keep (params.busAssign[(std::size_t) bus]);
        ctx.keep (session.bus (bus).strip.solo);
        params.busAssign[(std::size_t) bus].store (false);
        session.bus (bus).strip.solo.store (false);
    }
    session.oversamplingFactor.store (1);
    params.busAssign[1].store (true);
    params.auxSendDb[0].store (0.0f);
    const auto source = ctx.tempDir() / "freeze-gate-source.wav";
    std::vector<float> samples (length, 0.0f);
    samples[512] = 0.125f; samples[24576] = -0.25f; samples[length - 512] = 0.5f;
    if (! writeMono (source, samples)) return ScenarioResult::fail ("could not write the gated freeze source");
    placeRegion (ctx, 0, source, 0, length);
    auto reference = std::make_shared<std::optional<Rendered>>();
    auto next = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakNext = next;
    *next = [&ctx, &engine, &session, &params, &strip, reference, weakNext] (int pass)
    {
        if (pass == 3) { ctx.complete (ctx.verdict()); return; }
        params.mute.store (pass == 1);
        session.bus (0).strip.solo.store (pass == 2);
        session.recomputeRtCounters();
        struct Taps
        {
            std::array<std::array<float, blockSize>, 6> audio {};
            std::array<float, 3> peaks {};
        };
        auto taps = std::make_shared<Taps>();
        strip.setStemCapture (taps->audio[0].data(), taps->audio[1].data());
        engine.setBusStemCapture (1, taps->audio[2].data(), taps->audio[3].data());
        engine.setAuxStemCapture (0, taps->audio[4].data(), taps->audio[5].data());
        auto run = std::make_shared<BounceRun>();
        run->bounce = std::make_unique<BounceEngine> (engine, session, OfflineRenderQuality::MatchPlayback);
        ctx.cleanup ([run, taps] { run->bounce.reset(); });
        run->bounce->onProgressUpdated = [taps] (float)
        {
            for (std::size_t channel = 0; channel < taps->audio.size(); ++channel)
                for (auto& sample : taps->audio[channel])
                {
                    taps->peaks[channel / 2] = std::max (taps->peaks[channel / 2], std::abs (sample));
                    sample = 0.0f;
                }
        };
        run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
        { raw->ok = ok; raw->error = std::move (error); raw->finished.store (true, std::memory_order_release); };
        const auto out = ctx.sessionDir() / ("freeze-gate-" + std::to_string (pass) + ".wav");
        if (! run->bounce->startFreeze (0, sessionFile (out), length, kRate, blockSize))
        { ctx.complete (ScenarioResult::fail ("the gated freeze did not start")); return; }
        auto self = weakNext.lock();
        ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering(); },
            kRenderTimeoutMs, [&ctx, &engine, &session, &params, &strip, reference, self, run, taps, out, pass]
            {
                strip.setStemCapture (nullptr, nullptr);
                engine.setBusStemCapture (1, nullptr, nullptr);
                engine.setAuxStemCapture (0, nullptr, nullptr);
                const auto label = std::string (pass == 0 ? "reference" : pass == 1 ? "muted" : "bus-solo excluded");
                const auto audio = readBack (out);
                if (! ctx.expect (run->ok && audio.has_value(), label + ": freeze failed: " + run->error))
                { ctx.complete (ctx.verdict()); return; }
                ctx.expect (audio->info.numFrames == length && audio->peak() > 0.01f,
                            label + ": the freeze tap did not capture the complete source");
                ctx.expect (params.mute.load() == (pass == 1) && session.bus (0).strip.solo.load() == (pass == 2),
                            label + ": the render changed the mix gate");
                if (pass == 0)
                {
                    *reference = *audio;
                    ctx.expect (taps->peaks[0] > 0.01f && taps->peaks[1] > 0.01f && taps->peaks[2] > 0.01f,
                                "the reference did not exercise track, bus and aux accumulation");
                }
                else
                {
                    float difference = 0.0f;
                    const auto& expected = **reference;
                    for (std::size_t n = 0; n < std::min (audio->left.size(), expected.left.size()); ++n)
                        difference = std::max ({ difference, std::abs (audio->left[n] - expected.left[n]),
                                                              std::abs (audio->right[n] - expected.right[n]) });
                    ctx.expect (difference < 1.0e-5f, label + ": the mix gate changed pre-fader freeze samples");
                    ctx.expect (*std::max_element (taps->peaks.begin(), taps->peaks.end()) < 1.0e-8f,
                                label + ": the freeze tap leaked through the mix/bus/aux gate");
                }
                if (self) (*self) (pass + 1);
            }, "the gated freeze did not finish");
    };
    (*next) (0);
    return std::nullopt;
}

std::optional<ScenarioResult> defaultRenderRateWithoutDevice (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    const auto savedRate = session.sessionSampleRate;
    const auto lastDeviceRate = engine.getLastDeviceSampleRate();
    const bool registered = engine.isAudioCallbackRegistered();
    ctx.cleanup ([&engine, &session, savedRate, lastDeviceRate, registered]
    {
        engine.observeAudioRegistrationForScenario ({});
        session.sessionSampleRate = savedRate;
        engine.setLastDeviceSampleRateForScenario (lastDeviceRate);
        if (registered) engine.reattachAudioCallback();
        else engine.detachAudioCallback();
    });
    ctx.keep (session.oversamplingFactor);
    session.oversamplingFactor.store (2);
    auto next = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakNext = next;
    *next = [&ctx, &engine, &session, weakNext] (int pass)
    {
        if (pass == 12) { ctx.complete (ctx.verdict()); return; }
        const int setting = pass / 3, kind = pass % 3;
        const double expectedRate = setting == 0 || setting == 3 ? 44100.0 : 96000.0;
        const double requestedRate = setting == 3 ? expectedRate : 0.0;
        const double savedRate = setting == 1 || setting == 3 ? 96000.0 : 44100.0;
        const double cachedRate = setting >= 2 ? 96000.0 : 0.0;
        const auto length = (std::int64_t) expectedRate;
        const auto label = std::string (kind == 0 ? "mix" : kind == 1 ? "stems" : "freeze")
                         + ", rate setting " + std::to_string (setting);
        const auto source = ctx.tempDir() / ("rate-source-" + std::to_string (pass) + ".wav");
        std::vector<float> samples ((std::size_t) length);
        for (std::int64_t n = 0; n < length; ++n)
            samples[(std::size_t) n] = 0.1f * std::sin (2.0f * kPi * 1000.0f * (float) n / (float) expectedRate);
        dusk::audio::WriteSpec spec;
        spec.sampleRate = expectedRate; spec.numChannels = 1; spec.bitsPerSample = 32;
        auto writer = dusk::audio::FileWriter::create (source, spec);
        const float* channels[] { samples.data() };
        if (writer == nullptr || ! writer->write (channels, 1, length) || ! writer->flush())
        { ctx.complete (ScenarioResult::fail ("could not write the rate fixture")); return; }
        writer.reset();
        session.track (0).regions.clear();
        placeRegion (ctx, 0, source, 0, length);
        session.sessionSampleRate = savedRate;
        engine.prepareForSelfTest (expectedRate, 1024);
        const int liveLatency = engine.getChannelStrip (0).getOversamplingLatencySamples();
        engine.setLastDeviceSampleRateForScenario (cachedRate);
        engine.reattachAudioCallback();
        engine.detachAudioCallback();
        engine.audioDeviceStopped();
        engine.reattachAudioCallback();
        if (! ctx.expect (engine.getCurrentSampleRate() <= 0.0 && engine.isAudioCallbackRegistered(),
                          label + ": the fixture still has a running device rate"))
        { ctx.complete (ctx.verdict()); return; }
        auto registration = std::make_shared<std::vector<bool>>();
        engine.observeAudioRegistrationForScenario ([registration] (bool attached) { registration->push_back (attached); });
        struct Observed
        {
            bool stayedDetached = true;
            int renderLatency = -1;
        };
        auto observed = std::make_shared<Observed>();
        auto run = std::make_shared<BounceRun>();
        const auto quality = setting == 3 ? OfflineRenderQuality::High : OfflineRenderQuality::MatchPlayback;
        run->bounce = std::make_unique<BounceEngine> (engine, session, quality);
        ctx.cleanup ([run] { run->bounce.reset(); });
        run->bounce->onProgressUpdated = [&engine, observed] (float)
        {
            observed->stayedDetached &= ! engine.isAudioCallbackRegistered();
            observed->renderLatency = engine.getChannelStrip (0).getOversamplingLatencySamples();
        };
        run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
        { raw->ok = ok; raw->error = std::move (error); raw->finished.store (true, std::memory_order_release); };
        const auto out = ctx.sessionDir() / ("device-less-rate-" + std::to_string (pass) + ".wav");
        auto output = out;
        const auto mode = kind == 0 ? BounceEngine::Mode::MasterMix : BounceEngine::Mode::Stems;
        if (kind == 1)
        {
            const auto targets = BounceEngine::collectStemTargets (session, sessionFile (out));
            if (! ctx.expect (targets.size() == 1, "the rate fixture did not name one stem"))
            { ctx.complete (ctx.verdict()); return; }
            output = pathOf (targets.front().file);
        }
        const bool started = kind == 2 ? run->bounce->startFreeze (0, sessionFile (out), length, requestedRate, 1024)
            : run->bounce->start (sessionFile (out), requestedRate, 1024, 0.0, mode);
        if (! ctx.expect (started, label + ": render did not start")) { ctx.complete (ctx.verdict()); return; }
        auto self = weakNext.lock();
        ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering(); },
            kRenderTimeoutMs, [&ctx, &engine, &session, registration, observed, run, self, output, pass, setting,
                               expectedRate, savedRate, cachedRate, length, liveLatency, label]
            {
                engine.observeAudioRegistrationForScenario ({});
                ctx.expect (run->ok, label + ": render failed: " + run->error);
                ctx.expect (observed->stayedDetached && engine.isAudioCallbackRegistered()
                            && *registration == std::vector<bool> { false, true },
                            label + ": callback registration was not restored");
                ctx.expect (session.sessionSampleRate == savedRate && engine.getLastDeviceSampleRate() == cachedRate,
                            label + ": the render changed the session/device rate fallback");
                ctx.expect (setting == 3 ? observed->renderLatency > liveLatency : observed->renderLatency == liveLatency,
                            label + ": the render used the wrong quality");
                engine.prepareForSelfTest (expectedRate, ScenarioContext::kBlockSize);
                ctx.expect (session.oversamplingFactor.load() == 2
                            && engine.getChannelStrip (0).getOversamplingLatencySamples() == liveLatency,
                            label + ": render quality was not restored");
                const auto audio = readBack (output);
                if (ctx.expect (audio.has_value(), label + ": output is unreadable"))
                {
                    ctx.expect (std::abs (audio->info.sampleRate - expectedRate) < 0.5,
                                label + ": default render ignored the timeline rate");
                    ctx.expect (audio->info.numFrames == length, label + ": timeline frame length changed");
                    const auto begin = (std::size_t) length / 4, end = 3 * (std::size_t) length / 4;
                    int crossings = 0;
                    for (auto n = begin + 1; n < end && n < audio->left.size(); ++n)
                        if (audio->left[n - 1] <= 0.0f && audio->left[n] > 0.0f) ++crossings;
                    const double frequency = (double) crossings * expectedRate / (double) (end - begin);
                    ctx.note (label + ": tone " + std::to_string (frequency) + " Hz, header "
                              + std::to_string (audio->info.sampleRate));
                    ctx.expect (audio->peak() > 0.01f && std::abs (frequency - 1000.0) < 5.0,
                                label + ": the rendered tone/timeline duration changed");
                }
                if (self) (*self) (pass + 1);
            }, "the device-less rate render did not finish");
    };
    (*next) (0);
    return std::nullopt;
}

// ------------------------------------------------------ a refused target

std::optional<std::string> fileBytes (const std::filesystem::path& path)
{
    std::FILE* file = std::fopen (path.u8string().c_str(), "rb");
    if (file == nullptr) return std::nullopt;
    std::string bytes;
    char chunk[4096];
    for (std::size_t got; (got = std::fread (chunk, 1, sizeof (chunk), file)) > 0;)
        bytes.append (chunk, got);
    std::fclose (file);
    return bytes;
}

bool writeBytes (const std::filesystem::path& path, const std::string& bytes)
{
    std::FILE* file = std::fopen (path.u8string().c_str(), "wb");
    if (file == nullptr) return false;
    const bool wrote = std::fwrite (bytes.data(), 1, bytes.size(), file) == bytes.size();
    return std::fclose (file) == 0 && wrote;
}

// An existing file in a writable folder that this user cannot open for
// writing. False when the file modes are not enforced (root), so there is
// nothing to refuse.
bool makeRefusedTarget (ScenarioContext& ctx, const std::filesystem::path& path, const std::string& bytes)
{
    if (! writeBytes (path, bytes)) return false;
    namespace fs = std::filesystem;
    std::error_code fsError;
    fs::permissions (path, fs::perms::owner_read | fs::perms::group_read | fs::perms::others_read,
                     fs::perm_options::replace, fsError);
    ctx.cleanup ([path]
    {
        std::error_code ignored;
        fs::permissions (path, fs::perms::owner_write, fs::perm_options::add, ignored);
    });
    std::FILE* probe = std::fopen (path.u8string().c_str(), "r+b");
    if (probe == nullptr) return true;
    std::fclose (probe);
    return false;
}

// The reason a refused open reports, as the platform words it.
bool namesRefusal (const std::string& error, const std::filesystem::path& target)
{
    const auto prefix = "Could not write " + target.filename().u8string() + ": ";
   #ifdef _WIN32
    return error.rfind (prefix, 0) == 0 && error.size() > prefix.size();
   #else
    return error == prefix + std::generic_category().message (EACCES);
   #endif
}

// A render whose target exists but cannot be opened leaves it byte for byte,
// says why, and deletes only the files it created or truncated itself: a
// Mixdown, an MP3 bounce, a master export, a stem set refused at its third
// stem, and a freeze.
std::optional<ScenarioResult> refusedTargetLeftAsItWas (ScenarioContext& ctx)
{
    static constexpr int kLength = 4800;
    const auto source = ctx.tempDir() / "tone.wav";
    if (! writeMono (source, sine (440.0f, 0.25f, kLength)))
        return ScenarioResult::fail ("could not write the source take");
    for (int t = 0; t < 4; ++t)
        placeRegion (ctx, t, source, 0, kLength);

    const std::string previous = "the previous render, kept byte for byte";
    const auto mixdown = ctx.sessionDir() / "mixdown.wav";
    if (! makeRefusedTarget (ctx, mixdown, previous))
        return ScenarioResult::skip ("file modes are not enforced for this user, so no open is refused");

    const auto stemDir = ctx.sessionDir() / "stems";
    std::error_code fsError;
    std::filesystem::create_directories (stemDir, fsError);
    const auto base = stemDir / "song.wav";
    std::vector<std::filesystem::path> stems;
    for (const auto& target : BounceEngine::collectStemTargets (ctx.session(), sessionFile (base)))
        stems.push_back (pathOf (target.file));
    if (stems.size() != 4)
        return ScenarioResult::fail ("expected four track stems, got " + std::to_string (stems.size()));
    // Stem 1 is new, stem 2 is truncated, stem 3 refuses, stem 4 is never reached.
    if (! writeBytes (stems[1], previous) || ! makeRefusedTarget (ctx, stems[2], previous)
        || ! writeBytes (stems[3], previous))
        return ScenarioResult::fail ("could not lay out the previous stems");

    const auto freezeTarget = ctx.tempDir() / "freeze_track01.wav";
    if (! makeRefusedTarget (ctx, freezeTarget, previous))
        return ScenarioResult::fail ("could not lay out the previous freeze file");

    auto keptAsItWas = [&ctx, previous] (const std::filesystem::path& path, const std::string& what)
    {
        const auto bytes = fileBytes (path);
        ctx.expect (bytes.has_value(), what + " was deleted");
        ctx.expect (! bytes.has_value() || *bytes == previous, what + " was changed");
    };

    auto freezeLeg = [&ctx, freezeTarget, keptAsItWas]
    {
        auto run = std::make_shared<BounceRun>();
        run->bounce = std::make_unique<BounceEngine> (ctx.engine(), ctx.session());
        ctx.cleanup ([run] { run->bounce.reset(); });
        run->bounce->onFinished = [raw = run.get()] (bool ok, std::string error)
        {
            raw->ok = ok;
            raw->error = std::move (error);
            raw->finished.store (true, std::memory_order_release);
        };
        if (! ctx.expect (run->bounce->startFreeze (0, sessionFile (freezeTarget), kLength, kRate),
                          "the freeze render did not start"))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire)
                                      && ! run->bounce->isRendering(); },
                       kRenderTimeoutMs,
                       [&ctx, run, freezeTarget, keptAsItWas]
                       {
                           ctx.note ("freeze: " + run->error);
                           ctx.expect (! run->ok, "a freeze onto a refused file reported success");
                           ctx.expect (namesRefusal (run->error, freezeTarget),
                                       "the freeze error does not say why: " + run->error);
                           keptAsItWas (freezeTarget, "the refused freeze file");
                           ctx.complete (ctx.verdict());
                       },
                       "the freeze render never finished");
    };

    Render stemRender;
    stemRender.mode = BounceEngine::Mode::Stems;
    stemRender.tailSeconds = 0.1;
    auto stemsLeg = [&ctx, base, stems, stemRender, keptAsItWas, freezeLeg]
    {
        renderThen (ctx, base, stemRender, [&ctx, stems, keptAsItWas, freezeLeg] (bool ok, const std::string& error)
        {
            ctx.note ("stems: " + error);
            ctx.expect (! ok, "a stem bounce with a refused stem reported success");
            ctx.expect (namesRefusal (error, stems[2]), "the stem error does not say why: " + error);
            std::error_code existsError;
            ctx.expect (! std::filesystem::exists (stems[0], existsError),
                        "the stem the bounce created was left behind");
            ctx.expect (! std::filesystem::exists (stems[1], existsError),
                        "the stem the bounce truncated was left behind");
            keptAsItWas (stems[2], "the refused stem");
            keptAsItWas (stems[3], "the stem after the refused one");
            freezeLeg();
        });
    };

    auto masterLeg = [&ctx, source, previous, keptAsItWas, stemsLeg]
    {
        auto& player = ctx.engine().getMasteringPlayer();
        const auto master = ctx.sessionDir() / "master.wav";
        if (! player.loadFile (sessionFile (source)) || ! makeRefusedTarget (ctx, master, previous))
        {
            ctx.complete (ScenarioResult::fail ("could not load the mix or lay out the previous master"));
            return;
        }
        ctx.cleanup ([&player] { player.unloadFile(); });
        Render render;
        render.mode = BounceEngine::Mode::MasteringChain;
        render.tailSeconds = 0.1;
        renderThen (ctx, master, render, [&ctx, &player, master, keptAsItWas, stemsLeg] (bool ok, const std::string& error)
        {
            ctx.note ("master export: " + error);
            ctx.expect (! ok, "a master export onto a refused file reported success");
            ctx.expect (namesRefusal (error, master), "the master export error does not say why: " + error);
            keptAsItWas (master, "the refused master");
            player.unloadFile();
            stemsLeg();
        });
    };

    auto mp3Leg = [&ctx, previous, keptAsItWas, masterLeg]
    {
       #if DUSKSTUDIO_HAS_LAME
        const auto mp3 = ctx.sessionDir() / "mix.mp3";
        if (! makeRefusedTarget (ctx, mp3, previous))
        {
            ctx.complete (ScenarioResult::fail ("could not lay out the previous MP3"));
            return;
        }
        Render render;
        render.format = BounceEngine::Format::Mp3;
        render.tailSeconds = 0.1;
        renderThen (ctx, mp3, render, [&ctx, mp3, keptAsItWas, masterLeg] (bool ok, const std::string& error)
        {
            ctx.note ("mp3: " + error);
            ctx.expect (! ok, "an MP3 bounce onto a refused file reported success");
            ctx.expect (namesRefusal (error, mp3), "the MP3 error does not say why: " + error);
            keptAsItWas (mp3, "the refused MP3");
            masterLeg();
        });
       #else
        (void) previous;
        (void) keptAsItWas;
        ctx.note ("mp3: built without the MP3 encoder, leg not run");
        masterLeg();
       #endif
    };

    Render mixRender;
    mixRender.tailSeconds = 0.1;
    renderThen (ctx, mixdown, mixRender, [&ctx, mixdown, keptAsItWas, mp3Leg] (bool ok, const std::string& error)
    {
        ctx.note ("mixdown: " + error);
        ctx.expect (! ok, "a Mixdown onto a refused file reported success");
        ctx.expect (namesRefusal (error, mixdown), "the Mixdown error does not say why: " + error);
        keptAsItWas (mixdown, "the refused mixdown.wav");
        mp3Leg();
    });
    return std::nullopt;
}

// --------------------------------------------------------------- alignment

// Track 1 plays to the master and sends to aux 1, track 2 plays through bus 1,
// and track 3 is muted. The stems come out aligned with each other and with
// the mix, and together rebuild it; the muted track's stem is silent.
std::optional<ScenarioResult> stemsRebuildTheMix (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    static constexpr int kLength = 24000;
    const auto a = ctx.tempDir() / "a.wav";
    const auto b = ctx.tempDir() / "b.wav";
    if (! writeMono (a, sine (220.0f, 0.2f, kLength)) || ! writeMono (b, sine (330.0f, 0.2f, kLength)))
        return ScenarioResult::fail ("could not write the source takes");
    placeRegion (ctx, 0, a, 4800, kLength);
    placeRegion (ctx, 1, b, 9600, kLength);
    placeRegion (ctx, 2, a, 0, kLength);

    auto& sendDb = session.track (0).strip.auxSendDb[0];
    const float savedSend = sendDb.load (std::memory_order_relaxed);
    ctx.cleanup ([&sendDb, savedSend] { sendDb.store (savedSend, std::memory_order_relaxed); });
    sendDb.store (-6.0f, std::memory_order_relaxed);
    session.track (1).strip.busAssign[0].store (true, std::memory_order_relaxed);
    session.track (2).strip.mute.store (true, std::memory_order_relaxed);
    session.recomputeRtCounters();

    const auto stemDir = ctx.sessionDir() / "stems";
    std::error_code fsError;
    std::filesystem::create_directories (stemDir, fsError);
    const auto base = stemDir / "mix.wav";
    const auto targets = BounceEngine::collectStemTargets (session, sessionFile (base));
    const auto mixPath = ctx.sessionDir() / "mix.wav";

    Render stemRender;
    stemRender.mode = BounceEngine::Mode::Stems;
    stemRender.tailSeconds = 0.5;
    Render mixRender;
    mixRender.tailSeconds = 0.5;

    renderThen (ctx, base, stemRender, [&ctx, targets, mixPath, mixRender] (bool ok, const std::string& error)
    {
        if (! ctx.expect (ok, "the stem bounce failed: " + error))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        renderThen (ctx, mixPath, mixRender, [&ctx, targets, mixPath] (bool mixOk, const std::string& mixError)
        {
            const auto mix = readBack (mixPath);
            if (! ctx.expect (mixOk && mix.has_value(), "the mix bounce failed: " + mixError))
            {
                ctx.complete (ctx.verdict());
                return;
            }

            int trackStems = 0, busStems = 0, auxStems = 0;
            std::vector<float> rebuiltL (mix->left.size(), 0.0f), rebuiltR (mix->right.size(), 0.0f);
            for (const auto& target : targets)
            {
                const auto name = pathOf (target.file).filename().u8string();
                const auto stem = readBack (pathOf (target.file));
                if (! ctx.expect (stem.has_value(), "a stem is missing: " + name))
                    continue;
                ctx.expect (stem->info.numFrames == mix->info.numFrames,
                            "a stem is not the same length as the mix: " + name);
                switch (target.kind)
                {
                    case BounceEngine::StemTarget::Kind::Track: ++trackStems; break;
                    case BounceEngine::StemTarget::Kind::Bus:   ++busStems;   break;
                    case BounceEngine::StemTarget::Kind::Aux:   ++auxStems;   break;
                    case BounceEngine::StemTarget::Kind::Mix:   break;
                }
                if (target.kind == BounceEngine::StemTarget::Kind::Track && target.index == 2)
                    ctx.expect (stem->peak() <= 0.0f, "the muted track's stem is not silent");
                // A bus-routed track is inside its bus stem, so the rebuild
                // takes the direct track stems plus every bus and aux stem.
                if (target.kind == BounceEngine::StemTarget::Kind::Track && target.index == 1)
                    continue;
                for (std::size_t s = 0; s < rebuiltL.size() && s < stem->left.size(); ++s)
                {
                    rebuiltL[s] += stem->left[s];
                    rebuiltR[s] += stem->right[s];
                }
            }
            ctx.expect (trackStems == 3 && busStems == 1 && auxStems == 1,
                        "expected three track stems, one bus stem and one aux stem, got "
                            + std::to_string (trackStems) + "/" + std::to_string (busStems)
                            + "/" + std::to_string (auxStems));

            float worst = 0.0f;
            std::size_t worstAt = 0;
            for (std::size_t s = 0; s < rebuiltL.size(); ++s)
            {
                const float d = std::max (std::abs (rebuiltL[s] - mix->left[s]),
                                          std::abs (rebuiltR[s] - mix->right[s]));
                if (d > worst) { worst = d; worstAt = s; }
            }
            ctx.note ("largest stem-sum error " + std::to_string (worst) + " at sample "
                      + std::to_string (worstAt) + "; mix peak " + std::to_string (mix->peak()));
            ctx.expect (mix->peak() > 0.05f, "the mix is silent");
            ctx.expect (worst < 1.0e-4f, "the stems do not add back up to the mix");
            ctx.complete (ctx.verdict());
        });
    });
    return std::nullopt;
}

// A stems bounce renders the stems it named when it started. Its worker used
// to name them again on its own thread, reading regions, names and automation
// lanes the message thread replaces as it edits. The worker is held at its
// start while track 6 gains a region; the render still writes the one stem
// start() counted.
std::optional<ScenarioResult> stemsRenderWhatStartNamed (ScenarioContext& ctx)
{
    static constexpr int kLength = 4800;
    static constexpr int kLateTrack = 5;
    const auto source = ctx.tempDir() / "take.wav";
    if (! writeMono (source, sine (220.0f, 0.2f, kLength)))
        return ScenarioResult::fail ("could not write the source take");
    placeRegion (ctx, 0, source, 0, kLength);

    const auto stemDir = ctx.sessionDir() / "stems-named";
    std::error_code fsError;
    std::filesystem::create_directories (stemDir, fsError);
    const auto base = stemDir / "mix.wav";
    Render stemRender;
    stemRender.mode = BounceEngine::Mode::Stems;
    stemRender.tailSeconds = 0.1;

    auto held = std::make_shared<dusk::AutoResetEvent>();
    auto run = startRender (ctx, base, stemRender, [held] { held->wait(); });
    if (run == nullptr) return std::nullopt;
    // Runs ahead of the join startRender queued, so a failed wait still lets
    // the worker go.
    ctx.cleanup ([held] { held->signal(); });

    const int named = run->bounce->getTotalStemsToRender();
    ctx.expect (named == 1, "start() named " + std::to_string (named) + " stems, not one");
    placeRegion (ctx, kLateTrack, source, 0, kLength);
    const auto lateStem = pathOf (BounceEngine::stemOutputFile (
        sessionFile (base), kLateTrack, ctx.session().track (kLateTrack).name.toStdString()));
    held->signal();

    ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire)
                                  && ! run->bounce->isRendering(); },
                   kRenderTimeoutMs,
                   [&ctx, run, named, lateStem]
                   {
                       ctx.expect (run->ok, "the stem bounce failed: " + run->error);
                       const int rendered = run->bounce->getTotalStemsToRender();
                       ctx.note ("named at start " + std::to_string (named) + ", rendered "
                                 + std::to_string (rendered));
                       ctx.expect (rendered == named, "the render counted stems start() never named");
                       std::error_code existsError;
                       ctx.expect (! std::filesystem::exists (lateStem, existsError),
                                   "the render wrote a stem for a track that had no content when it started");
                       ctx.complete (ctx.verdict());
                   },
                   "the bounce never finished");
    return std::nullopt;
}

// An impulse on a direct track and one on a bus-routed track, rendered as a
// mix and as stems at both render qualities, all land where they sit on the timeline.
std::optional<ScenarioResult> rendersLandOnTheTimeline (ScenarioContext& ctx)
{
    static constexpr std::int64_t kAt = 24000;
    const auto source = ctx.tempDir() / "impulse.wav";
    if (! writeMono (source, impulse (4800, 0)))
        return ScenarioResult::fail ("could not write the impulse");
    placeRegion (ctx, 0, source, kAt, 4800);
    placeRegion (ctx, 1, source, kAt + 12000, 4800);
    ctx.session().track (1).strip.busAssign[0].store (true, std::memory_order_relaxed);
    ctx.session().recomputeRtCounters();

    const auto stemDir = ctx.sessionDir() / "stems";
    std::error_code fsError;
    std::filesystem::create_directories (stemDir, fsError);
    const auto base = stemDir / "at.wav";
    const auto targets = BounceEngine::collectStemTargets (ctx.session(), sessionFile (base));
    const auto mixPath = ctx.sessionDir() / "at-mix.wav";

    const auto landed = [&ctx] (const std::vector<float>& samples, std::int64_t from, std::int64_t to,
                                std::int64_t want, const std::string& label)
    {
        from = std::clamp (from, std::int64_t { 0 }, (std::int64_t) samples.size());
        to = std::clamp (to, from, (std::int64_t) samples.size());
        if (! ctx.expect (from < to, label + ": the impulse search range is empty"))
            return;
        const std::vector<float> window (samples.begin() + from, samples.begin() + to);
        const auto at = from + argmaxAbs (window);
        ctx.note (label + ": impulse at " + std::to_string (at));
        ctx.expect (std::abs (at - want) <= kFractionalSlack,
                    label + ": the impulse moved by " + std::to_string (at - want) + " samples");
    };

    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const int savedFactor = session.oversamplingFactor.load (std::memory_order_relaxed);
    ctx.cleanup ([&session, &engine, savedFactor]
    {
        session.oversamplingFactor.store (savedFactor, std::memory_order_relaxed);
        engine.prepareForSelfTest (kRate, ScenarioContext::kBlockSize);
    });
    session.oversamplingFactor.store (4, std::memory_order_relaxed);
    engine.prepareForSelfTest (kRate, ScenarioContext::kBlockSize);
    const int highLatency = engine.getChannelStrip (0).getOversamplingLatencySamples();
    session.oversamplingFactor.store (1, std::memory_order_relaxed);
    engine.prepareForSelfTest (kRate, ScenarioContext::kBlockSize);
    const int liveLatency = engine.getChannelStrip (0).getOversamplingLatencySamples();

    auto next = std::make_shared<std::function<void (int)>>();
    std::weak_ptr<std::function<void (int)>> weakNext = next;
    *next = [&ctx, &session, &engine, weakNext, mixPath, base, targets, landed, highLatency, liveLatency] (int pass)
    {
        if (pass >= 4)
        {
            ctx.complete (ctx.verdict());
            return;
        }
        const bool stems = pass % 2 != 0;
        Render render;
        render.tailSeconds = 0.2;
        render.mode = stems ? BounceEngine::Mode::Stems : BounceEngine::Mode::MasterMix;
        render.quality = pass < 2 ? OfflineRenderQuality::High : OfflineRenderQuality::MatchPlayback;
        const auto label = std::string (pass < 2 ? "high quality, " : "match playback, ") + (stems ? "stems" : "mix");
        const int expectedLatency = pass < 2 ? highLatency : liveLatency;
        auto renderLatency = std::make_shared<std::atomic<int>> (-1);
        auto run = startRender (ctx, stems ? base : mixPath, render, {}, [&engine, renderLatency] (float)
        { renderLatency->store (engine.getChannelStrip (0).getOversamplingLatencySamples(), std::memory_order_relaxed); });
        if (run == nullptr) return;
        auto self = weakNext.lock();
        ctx.waitUntil ([run] { return run->finished.load (std::memory_order_acquire) && ! run->bounce->isRendering(); },
            kRenderTimeoutMs,
            [&ctx, &session, &engine, run, self, pass, stems, label, expectedLatency, renderLatency,
             mixPath, targets, landed, liveLatency]
            {
                if (! ctx.expect (run->ok, label + ": bounce failed: " + run->error))
                { ctx.complete (ctx.verdict()); return; }
                ctx.expect (renderLatency->load (std::memory_order_relaxed) == expectedLatency,
                            label + ": the render did not use the selected oversampling quality");
                ctx.expect (session.oversamplingFactor.load (std::memory_order_relaxed) == 1,
                            label + ": the render changed the live oversampling setting");
                engine.prepareForSelfTest (kRate, ScenarioContext::kBlockSize);
                ctx.expect (engine.getChannelStrip (0).getOversamplingLatencySamples() == liveLatency,
                            label + ": the render's oversampling override was not cleared");
                if (! stems)
                {
                    const auto mix = readBack (mixPath);
                    if (ctx.expect (mix.has_value(), label + ": the mix bounce wrote no readable file"))
                    {
                        landed (mix->left, 0, kAt + 6000, kAt, label + ", direct track");
                        landed (mix->left, kAt + 6000, (std::int64_t) mix->left.size(), kAt + 12000, label + ", bus-routed track");
                    }
                }
                else for (const auto& target : targets)
                {
                    const auto stem = readBack (pathOf (target.file));
                    if (ctx.expect (stem.has_value(), label + ": a stem file was not readable"))
                    {
                        const bool routed = target.kind == BounceEngine::StemTarget::Kind::Bus
                                         || (target.kind == BounceEngine::StemTarget::Kind::Track
                                             && target.index == 1);
                        landed (stem->left, 0, (std::int64_t) stem->left.size(),
                                routed ? kAt + 12000 : kAt,
                                label + ", " + pathOf (target.file).filename().u8string());
                    }
                }
                (*self) (pass + 1);
            }, "the alignment bounce never finished");
    };
    (*next) (0);
    return std::nullopt;
}

#if DUSKSTUDIO_HAS_NATIVE_VST3
// An insert's latency delays every other track to match; the render drops
// that compensation too, so an impulse on a plug-in-free track stays put.
std::optional<ScenarioResult> pdcTrimmedFromRender (ScenarioContext& ctx)
{
    static constexpr std::int64_t kAt = 24000;
    const auto source = ctx.tempDir() / "impulse.wav";
    if (! writeMono (source, impulse (4800, 0)))
        return ScenarioResult::fail ("could not write the impulse");
    placeRegion (ctx, 0, source, kAt, 4800);

    auto& engine = ctx.engine();
    auto& slot = engine.getChannelStrip (3).getNativeVst3Slot();
    std::string error;
    if (! slot.load (*ctx.fixture ("relayout.vst3"), kRate, ScenarioContext::kBlockSize, error))
        return ScenarioResult::fail ("the latency fixture did not load: " + error);
    for (int i = 0; i < slot.paramCount(); ++i)
        if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == "Latency Mode")
            slot.setParamValue (info->id, 1.0);
    if (! slot.reactivate (kRate, ScenarioContext::kBlockSize, error))
        return ScenarioResult::fail ("the latency fixture did not re-activate: " + error);
    engine.recomputePdc();
    ctx.note ("aggregate PDC " + std::to_string (engine.getAggregatePdcLatencySamples()));
    if (! ctx.expect (engine.getAggregatePdcLatencySamples() > 0,
                      "the fixture added no delay compensation to trim"))
        return ctx.verdict();

    const auto out = ctx.sessionDir() / "pdc.wav";
    Render render;
    render.tailSeconds = 0.2;
    renderThen (ctx, out, render, [&ctx, out] (bool ok, const std::string& renderError)
    {
        const auto mix = readBack (out);
        if (ctx.expect (ok && mix.has_value(), "the bounce failed: " + renderError))
        {
            const auto at = argmaxAbs (mix->left);
            ctx.note ("impulse at sample " + std::to_string (at));
            ctx.expect (std::abs (at - kAt) <= kFractionalSlack,
                        "the impulse moved by " + std::to_string (at - kAt)
                            + " samples, so the compensation delay is in the file");
        }
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}
#endif

// Export master drops the mastering chain's own latency from the file, with the
// multiband comp in or out. The chain's EQ and limiter each oversample 4x, so it
// can land two samples out.
std::optional<ScenarioResult> exportMasterInPlace (ScenarioContext& ctx, bool compIn)
{
    static constexpr int kAt = 24000;
    auto& player = ctx.engine().getMasteringPlayer();
    auto& mastering = ctx.session().mastering();
    const auto source = ctx.tempDir() / "impulse-mix.wav";
    if (! writeMono (source, impulse (48000, kAt)))
        return ScenarioResult::fail ("could not write the source mix");
    if (! player.loadFile (sessionFile (source)))
        return ScenarioResult::fail ("the mastering player would not load the mix");
    const bool compWas = mastering.compEnabled.load (std::memory_order_relaxed);
    ctx.cleanup ([&player, &mastering, compWas]
    {
        player.unloadFile();
        mastering.compEnabled.store (compWas, std::memory_order_relaxed);
    });
    mastering.compEnabled.store (compIn, std::memory_order_relaxed);

    const auto out = ctx.sessionDir() / "master.wav";
    Render render;
    render.mode = BounceEngine::Mode::MasteringChain;
    render.tailSeconds = 0.2;
    renderThen (ctx, out, render, [&ctx, out] (bool ok, const std::string& error)
    {
        const auto master = readBack (out);
        if (ctx.expect (ok && master.has_value(), "the export failed: " + error))
        {
            const auto at = argmaxAbs (master->left);
            ctx.note ("impulse at sample " + std::to_string (at));
            ctx.expect (std::abs (at - kAt) <= 2 * kFractionalSlack,
                        "the export moved the mix by " + std::to_string (at - kAt) + " samples");
        }
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}

// Live, with Effect Oversampling at 2x and 4x, a bus-routed track plays at the
// same moment as a direct one.
ScenarioResult busRoutedLandsWithDirect (ScenarioContext& ctx)
{
    static constexpr std::int64_t kAt = 24000;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    const auto source = ctx.tempDir() / "impulse.wav";
    if (! writeMono (source, impulse (4800, 0)))
        return ScenarioResult::fail ("could not write the impulse");
    placeRegion (ctx, 0, source, kAt, 4800);
    placeRegion (ctx, 1, source, kAt, 4800);
    session.track (1).strip.busAssign[0].store (true, std::memory_order_relaxed);

    const int savedFactor = session.oversamplingFactor.load (std::memory_order_relaxed);
    ctx.cleanup ([&session, &engine, savedFactor]
    {
        session.oversamplingFactor.store (savedFactor, std::memory_order_relaxed);
        engine.prepareForSelfTest (kRate, ScenarioContext::kBlockSize);
    });

    const auto landsAt = [&] (int audibleTrack)
    {
        for (int t = 0; t < 2; ++t)
            session.track (t).strip.mute.store (t != audibleTrack, std::memory_order_relaxed);
        session.recomputeRtCounters();
        engine.getTransport().setPlayhead (0);
        engine.play();
        std::int64_t at = -1;
        float best = 0.0f;
        for (int block = 0; block < (int) (kAt / ScenarioContext::kBlockSize) + 8; ++block)
        {
            ctx.pump (1);
            const auto& out = ctx.lastBlock (0);
            for (std::size_t i = 0; i < out.size(); ++i)
                if (std::abs (out[i]) > best)
                {
                    best = std::abs (out[i]);
                    at = (std::int64_t) block * ScenarioContext::kBlockSize + (std::int64_t) i;
                }
        }
        engine.stop();
        return at;
    };

    for (const int factor : { 2, 4 })
    {
        session.oversamplingFactor.store (factor, std::memory_order_relaxed);
        engine.prepareForSelfTest (kRate, ScenarioContext::kBlockSize);
        const auto direct = landsAt (0);
        const auto routed = landsAt (1);
        ctx.note (std::to_string (factor) + "x: direct at " + std::to_string (direct)
                  + ", via bus at " + std::to_string (routed));
        ctx.expect (std::abs (direct - routed) <= kFractionalSlack,
                    std::to_string (factor) + "x: the bus-routed track played "
                        + std::to_string (routed - direct) + " samples after the direct one");
    }
    return ctx.verdict();
}

// The lag, within +/-maxLag, at which `signal` best matches `reference` over
// `length` samples from `from`; positive when `signal` plays later.
std::int64_t bestLag (const std::vector<float>& reference, const std::vector<float>& signal,
                      std::int64_t from, int length, int maxLag)
{
    std::int64_t best = 0;
    double bestSum = 0.0;
    for (int lag = -maxLag; lag <= maxLag; ++lag)
    {
        double sum = 0.0;
        for (int i = 0; i < length; ++i)
        {
            const auto r = from + i, s = r + lag;
            if (r >= 0 && s >= 0 && r < (std::int64_t) reference.size()
                && s < (std::int64_t) signal.size())
                sum += (double) reference[(std::size_t) r] * (double) signal[(std::size_t) s];
        }
        if (lag == -maxLag || sum > bestSum)
        {
            bestSum = sum;
            best = lag;
        }
    }
    return best;
}

// Where Sunset's Oversampling switch sits among the slot's parameters, or -1.
int oversamplingParam (const builtin::NativeBuiltinSlot& slot)
{
    for (int i = 0; i < slot.paramCount(); ++i)
        if (const auto* info = slot.paramInfo (i);
            info != nullptr && std::string (info->id) == "oversampling")
            return i;
    return -1;
}

// Sunset on tracks 0, 1, ..., one for each Oversampling setting given, each
// playing `notes` - a key and the timeline sample it starts on, an eighth of a
// beat long in a region twice that. Each track's MIDI out carries exactly what
// its Sunset is handed, into a recorder port of its own. Null, with the reason
// recorded, when any of that cannot be set up.
RecordingMidiBackend* sunsetsIntoRecorders (ScenarioContext& ctx, const std::vector<float>& oversampling,
                                            const std::vector<std::pair<int, std::int64_t>>& notes)
{
    static constexpr const char* kSunset = "dusk.builtin.synth";
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const int numTracks = (int) oversampling.size();

    // The engine remembers each track's live MIDI route from the last block it
    // ran, which may have been the previous scenario's. One block with these
    // tracks still audio settles that, so a reset it owes is not recorded here.
    ctx.pump (1);

    auto owned = std::make_unique<RecordingMidiBackend> (numTracks);
    auto* const recorder = owned.get();
    engine.installMidiOutputBackend (std::move (owned));
    bool opened = engine.getMidiOutputDevices().size() >= (std::size_t) numTracks;
    for (int t = 0; t < numTracks && opened; ++t)
        opened = engine.ensureMidiOutputOpen (t);
    if (! ctx.expect (opened, "the recording MIDI outputs would not open"))
        return nullptr;

    ctx.cleanup ([&engine, &session, &transport, numTracks]
    {
        engine.stop();
        transport.setLoopEnabled (false);
        for (int t = 0; t < numTracks; ++t)
        {
            session.track (t).midiOutputIndex.store (-1, std::memory_order_relaxed);
            engine.getChannelStrip (t).unloadBuiltin();
        }
    });

    const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
    static constexpr std::int64_t kNoteTicks = kMidiTicksPerQuarter / 8;
    for (int t = 0; t < numTracks; ++t)
    {
        auto& track = session.track (t);
        track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        auto& strip = engine.getChannelStrip (t);
        std::string error;
        if (! ctx.expect (strip.loadBuiltin (kSunset, error), "could not load Sunset: " + error))
            return nullptr;
        auto& slot = strip.getBuiltinSlot();
        const int param = oversamplingParam (slot);
        if (! ctx.expect (param >= 0, "Sunset has no Oversampling parameter"))
            return nullptr;
        slot.setParamValue (param, oversampling[(std::size_t) t]);
        auto regions = std::make_unique<std::vector<MidiRegion>>();
        for (const auto& [pitch, onAt] : notes)
        {
            MidiRegion region;
            region.timelineStart = onAt;
            region.lengthInTicks = 2 * kNoteTicks;
            region.lengthInSamples = ticksToSamples (region.lengthInTicks, kRate, bpm);
            region.notes.push_back ({ 1, pitch, 100, 0, kNoteTicks });
            regions->push_back (region);
        }
        track.midiRegions.publish (std::move (regions));
        track.midiOutputIndex.store (t, std::memory_order_relaxed);
    }
    ctx.pump (1);
    return recorder;
}

// The bank hands blocks to its pump thread, so a recorder's count settles a
// little after the last block rather than inside it. Once it has, `check` is
// handed everything recorded and the scenario completes with the verdict.
void whenRecorderSettles (ScenarioContext& ctx, const RecordingMidiBackend* recorder,
                          std::function<void (const std::vector<RecordingMidiBackend::Message>&)> check)
{
    struct Drain
    {
        std::size_t lastCount = 0;
        int stablePolls = 0;
    };
    auto drain = std::make_shared<Drain>();
    ctx.waitUntil ([drain, recorder]
                   {
                       const auto count = recorder->count();
                       if (count == drain->lastCount) ++drain->stablePolls;
                       else { drain->lastCount = count; drain->stablePolls = 0; }
                       return drain->stablePolls >= 3;
                   },
                   5000,
                   [&ctx, recorder, check = std::move (check)]
                   {
                       check (recorder->captured());
                       ctx.expect (! recorder->hasDropped(), "the MIDI recorder ran out of room");
                       ctx.complete (ctx.verdict());
                   },
                   "the MIDI output recorder never stopped receiving events");
}

// What one recorder port was handed: its resets, each counted by the All Sound
// Off on channel 1 and listed at its block offset, and its note-ons by key.
struct PortTally
{
    int resets = 0;
    std::string resetOffsets;
    std::array<int, 128> noteOns {};
};

PortTally tallyPort (const std::vector<RecordingMidiBackend::Message>& messages, int port)
{
    PortTally tally;
    for (const auto& m : messages)
    {
        if (m.port != port || m.numBytes != 3)
            continue;
        if ((m.bytes[0] & 0xF0) == 0x90 && m.bytes[2] > 0)
            ++tally.noteOns[(std::size_t) (m.bytes[1] & 0x7F)];
        else if (m.bytes[0] == 0xB0 && m.bytes[1] == 120)
        {
            ++tally.resets;
            tally.resetOffsets += " " + std::to_string (m.sampleOffset);
        }
    }
    return tally;
}

// How many times a note on a recorder port should have been attacked.
struct Attacks
{
    const char* note;
    int key;
    int expected;
};

void expectHanded (ScenarioContext& ctx, const std::string& track, const PortTally& tally,
                   int expectedResets, std::initializer_list<Attacks> attacks)
{
    std::string line = track + ": " + std::to_string (tally.resets) + " resets, at block offsets"
                     + tally.resetOffsets;
    for (const auto& a : attacks)
        line += "; " + std::string (a.note) + " attacked "
              + std::to_string (tally.noteOns[(std::size_t) a.key]) + " times";
    ctx.note (line);
    ctx.expect (tally.resets == expectedResets,
                track + ", Sunset was reset " + std::to_string (tally.resets) + " times, not "
                    + std::to_string (expectedResets));
    for (const auto& a : attacks)
    {
        const int attacked = tally.noteOns[(std::size_t) a.key];
        ctx.expect (attacked == a.expected,
                    track + ", " + a.note + " was attacked " + std::to_string (attacked) + " times, not "
                        + std::to_string (a.expected));
    }
}

// Live, a Sunset note plays where it sits on the timeline, level with an
// impulse an audio track holds at the same sample, at every setting of
// Sunset's own Oversampling switch - straight through, and on the pass after
// a loop wraps, where the note's scheduling wraps that latency ahead of the
// audio.
ScenarioResult sunsetLandsWithAudio (ScenarioContext& ctx)
{
    static constexpr const char* kSunset = "dusk.builtin.synth";
    if (builtin::findUnit (kSunset) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    static constexpr std::int64_t kAt = 24000;
    // The loop ends 16 samples into a block, so the note, clear of the audio
    // seam's declick 128 samples past the loop start, is scheduled in the span
    // after the seam of the very block that wraps.
    static constexpr std::int64_t kLoopStart = kAt - 128;
    static constexpr std::int64_t kLoopEnd = kLoopStart + 93 * ScenarioContext::kBlockSize + 16;
    static constexpr int kAudioTrack = 0, kSynthTrack = 1;
    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    const auto source = ctx.tempDir() / "impulse.wav";
    if (! writeMono (source, impulse (4800, 0)))
        return ScenarioResult::fail ("could not write the impulse");
    placeRegion (ctx, kAudioTrack, source, kAt, 4800);

    auto& strip = engine.getChannelStrip (kSynthTrack);
    ctx.cleanup ([&strip, &transport]
    {
        transport.setLoopEnabled (false);
        strip.unloadBuiltin();
    });
    session.track (kSynthTrack).mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    std::string error;
    if (! ctx.expect (strip.loadBuiltin (kSunset, error), "could not load Sunset: " + error))
        return ctx.verdict();
    auto& slot = strip.getBuiltinSlot();
    const int oversampling = oversamplingParam (slot);
    if (! ctx.expect (oversampling >= 0, "Sunset has no Oversampling parameter"))
        return ctx.verdict();

    // Short enough that its release has died away before the loop wraps.
    MidiRegion region;
    region.timelineStart = kAt;
    region.lengthInTicks = kMidiTicksPerQuarter;
    region.lengthInSamples = ticksToSamples (region.lengthInTicks, kRate,
                                             session.tempoBpm.load (std::memory_order_relaxed));
    region.notes.push_back ({ 1, 60, 127, 0, kMidiTicksPerQuarter / 4 });
    session.track (kSynthTrack).midiRegions.publish (
        std::make_unique<std::vector<MidiRegion>> (1, region));

    // Plays with one of the two tracks audible and returns the left master
    // output, then lets the note's release die away. Straight through it plays
    // from the top; looped, from the loop start through one wrap.
    const auto play = [&] (int audibleTrack, bool looped)
    {
        for (const int t : { kAudioTrack, kSynthTrack })
            session.track (t).strip.mute.store (t != audibleTrack, std::memory_order_relaxed);
        session.recomputeRtCounters();
        transport.setLoopRange (kLoopStart, kLoopEnd);
        transport.setLoopEnabled (looped);
        transport.setPlayhead (looped ? kLoopStart : 0);
        engine.play();
        std::vector<float> out;
        const auto frames = looped ? kLoopEnd - kLoopStart : kAt;
        for (int block = 0; block < (int) (frames / ScenarioContext::kBlockSize) + 24; ++block)
        {
            ctx.pump (1);
            const auto& o = ctx.lastBlock (0);
            out.insert (out.end(), o.begin(), o.end());
        }
        engine.stop();
        transport.setLoopEnabled (false);
        ctx.pump (400);
        return out;
    };

    // At 1x Sunset has no latency and its attack starts on the impulse's
    // sample. That render is the reference the other two must line up with.
    for (const bool looped : { false, true })
    {
        const std::string leg = looped ? "after the loop wraps" : "straight through";
        // Where the note plays in the render; looped, on the second pass.
        const std::int64_t noteAt = looped ? (kLoopEnd - kLoopStart) + (kAt - kLoopStart) : kAt;
        std::vector<float> atUnity;
        std::int64_t audioAtUnity = 0;
        for (const int setting : { 0, 1, 2 })
        {
            slot.setParamValue (oversampling, (float) setting);
            ctx.pump (1);
            const int latency = slot.getLatencySamples();
            const auto audio = play (kAudioTrack, looped);
            const auto synth = play (kSynthTrack, looped);
            const auto audioAt = noteAt - 512
                + argmaxAbs (std::vector<float> (audio.begin() + (noteAt - 512), audio.end()));
            const std::string factor = std::to_string (1 << setting) + "x";
            if (setting == 0)
            {
                // The first sample to rise clear of the floor ahead of the note.
                // How hard the attack starts depends on the voice's history, so
                // the floor sets the bar, not the note.
                float noiseFloor = 0.0f;
                for (std::int64_t i = noteAt - 512; i < noteAt - 64; ++i)
                    noiseFloor = std::max (noiseFloor, std::abs (synth[(std::size_t) i]));
                std::int64_t onset = -1;
                for (std::int64_t i = noteAt - 64; i < (std::int64_t) synth.size() && onset < 0; ++i)
                    if (std::abs (synth[(std::size_t) i]) > 3.0f * noiseFloor)
                        onset = i;
                ctx.note (leg + ", 1x: impulse at " + std::to_string (audioAt)
                          + ", note starts at " + std::to_string (onset));
                ctx.expect (latency == 0, "at 1x Sunset reports " + std::to_string (latency)
                                              + " samples of latency");
                ctx.expect (onset == audioAt, leg + ", at 1x the note started "
                                                  + std::to_string (onset - audioAt)
                                                  + " samples from the impulse");
                atUnity = synth;
                audioAtUnity = audioAt;
                continue;
            }
            const auto lag = bestLag (atUnity, synth, audioAtUnity - 64, 2048, 32)
                           - (audioAt - audioAtUnity);
            ctx.note (leg + ", " + factor + ": Sunset reports " + std::to_string (latency)
                      + " samples, the note plays " + std::to_string (lag) + " from the 1x note");
            ctx.expect (latency > 0, "at " + factor + " Sunset reports no latency to compensate");
            ctx.expect (std::abs (lag) <= kFractionalSlack,
                        leg + ", at " + factor + " the note played " + std::to_string (lag)
                            + " samples after where it sits on the timeline");
        }
    }
    return ctx.verdict();
}

// Sunset's Oversampling switch moves the latency a MIDI track's notes are
// scheduled ahead by, and flipping it while the transport rolls must not move
// the schedule over any event. Up from 1x the window would jump 12 samples past
// a note-off due in that gap and leave the note hanging; back down it would go
// over 12 samples already sent and play a note-on twice. The track's MIDI out
// carries exactly what the instrument is handed, so a recorder there counts
// every note-on and note-off, and each note has to die away once it ends.
std::optional<ScenarioResult> sunsetOversamplingFlipsMidRoll (ScenarioContext& ctx)
{
    static constexpr const char* kSunset = "dusk.builtin.synth";
    if (builtin::findUnit (kSunset) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    static constexpr int kTrack = 0;
    static constexpr int kBlock = ScenarioContext::kBlockSize;
    // A flip lands inside the block it is pumped with, so the schedule moves
    // from the next one: note A ends 5 samples into kUpBlock, the first block
    // scheduled at 2x, and note B starts 5 samples into kDownBlock, the first
    // back at 1x.
    static constexpr std::int64_t kUpBlock = 40;
    static constexpr std::int64_t kDownBlock = 1000;
    static constexpr int kIntoBlock = 5;
    static constexpr int kNoteA = 60, kNoteB = 64;
    static constexpr float kQuietDb = -60.0f;

    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto& track = session.track (kTrack);

    auto owned = std::make_unique<RecordingMidiBackend>();
    auto* const recorder = owned.get();
    engine.installMidiOutputBackend (std::move (owned));
    if (engine.getMidiOutputDevices().empty() || ! engine.ensureMidiOutputOpen (0))
        return ScenarioResult::fail ("the recording MIDI output would not open");

    auto& strip = engine.getChannelStrip (kTrack);
    ctx.cleanup ([&engine, &strip, &track]
    {
        engine.stop();
        track.midiOutputIndex.store (-1, std::memory_order_relaxed);
        strip.unloadBuiltin();
    });
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    std::string error;
    if (! ctx.expect (strip.loadBuiltin (kSunset, error), "could not load Sunset: " + error))
        return ctx.verdict();
    auto& slot = strip.getBuiltinSlot();
    const int oversampling = oversamplingParam (slot);
    if (! ctx.expect (oversampling >= 0, "Sunset has no Oversampling parameter"))
        return ctx.verdict();
    slot.setParamValue (oversampling, 0.0f);
    ctx.pump (1);
    if (! ctx.expect (slot.getLatencySamples() == 0, "at 1x Sunset reports latency"))
        return ctx.verdict();

    const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
    static constexpr std::int64_t kNoteTicks = kMidiTicksPerQuarter / 4;
    const auto noteSamples = ticksToSamples (kNoteTicks, kRate, bpm);
    const auto noteRegion = [&] (int pitch, std::int64_t onAt)
    {
        MidiRegion region;
        region.timelineStart = onAt;
        region.lengthInTicks = 2 * kNoteTicks;
        region.lengthInSamples = ticksToSamples (region.lengthInTicks, kRate, bpm);
        region.notes.push_back ({ 1, pitch, 100, 0, kNoteTicks });
        return region;
    };
    const std::int64_t offA = kUpBlock * kBlock + kIntoBlock;
    const std::int64_t onB = kDownBlock * kBlock + kIntoBlock;
    auto regions = std::make_unique<std::vector<MidiRegion>>();
    regions->push_back (noteRegion (kNoteA, offA - noteSamples));
    regions->push_back (noteRegion (kNoteB, onB));
    track.midiRegions.publish (std::move (regions));
    track.midiOutputIndex.store (0, std::memory_order_relaxed);

    std::int64_t block = 0;
    float loudestDb = -100.0f;
    const auto pumpTo = [&] (std::int64_t end)
    {
        for (; block < end; ++block)
        {
            ctx.pump (1);
            loudestDb = std::max (loudestDb, strip.getOutLDb());
        }
    };
    // The block the track's output first reads quiet, or -1 if it is still
    // sounding at `limit`.
    const auto quietBy = [&] (std::int64_t limit)
    {
        for (; block < limit; pumpTo (block + 1))
            if (strip.getOutLDb() <= kQuietDb)
                return block;
        return std::int64_t { -1 };
    };

    transport.setLoopEnabled (false);
    transport.setPlayhead (0);
    engine.play();

    pumpTo (kUpBlock - 1);
    slot.setParamValue (oversampling, 1.0f);
    pumpTo (kUpBlock);
    const int raised = slot.getLatencySamples();
    ctx.note ("at 2x Sunset reports " + std::to_string (raised) + " samples");
    ctx.expect (raised > kIntoBlock, "the switch to 2x opened no gap over note A's note-off");
    pumpTo (kUpBlock + 1);
    ctx.note ("note A peaked at " + std::to_string (loudestDb) + " dB");
    ctx.expect (loudestDb > kQuietDb + 20.0f, "note A never sounded");
    const auto quietA = quietBy (kDownBlock - 8);
    ctx.note ("note A was quiet by block " + std::to_string (quietA));
    ctx.expect (quietA >= 0, "note A is still sounding: its note-off, due in the gap the "
                             "switch to 2x opened, never reached Sunset");

    pumpTo (kDownBlock - 1);
    slot.setParamValue (oversampling, 0.0f);
    pumpTo (kDownBlock);
    ctx.expect (slot.getLatencySamples() == 0, "the switch back to 1x left latency behind");
    pumpTo ((onB + noteSamples) / kBlock + 1);
    const auto quietB = quietBy (kDownBlock + 600);
    ctx.note ("note B was quiet by block " + std::to_string (quietB));
    ctx.expect (quietB >= 0, "note B is still sounding after its note-off");
    engine.stop();
    ctx.pump (4);

    whenRecorderSettles (ctx, recorder, [&ctx] (const std::vector<RecordingMidiBackend::Message>& messages)
    {
        std::array<int, 128> ons {}, offs {};
        for (const auto& m : messages)
        {
            const int kind = m.bytes[0] & 0xF0;
            if (m.numBytes != 3 || (kind != 0x80 && kind != 0x90))
                continue;
            auto& tally = (kind == 0x90 && m.bytes[2] > 0) ? ons : offs;
            ++tally[(std::size_t) (m.bytes[1] & 0x7F)];
        }
        for (const int pitch : { kNoteA, kNoteB })
        {
            const auto name = std::string (pitch == kNoteA ? "note A" : "note B");
            const int on = ons[(std::size_t) pitch], off = offs[(std::size_t) pitch];
            ctx.note (name + ": " + std::to_string (on) + " note-on, "
                      + std::to_string (off) + " note-off");
            ctx.expect (on == 1, name + " was handed " + std::to_string (on) + " note-ons, not one");
            ctx.expect (off == 1, name + " was handed " + std::to_string (off) + " note-offs, not one");
        }
    });
    return std::nullopt;
}

#if DUSKSTUDIO_HAS_NATIVE_CLAP
// A latency rise while the transport rolls is bridged: the next block's window
// starts where the last one ended and stretches to the new lead, and what it
// holds before the block's first sample lands on that sample. A note that
// starts and ends inside that stretch went out note-off first, then note-on,
// and hung. The latency fixture's look-ahead goes from 0 to 512 between two
// blocks, and a 9-tick note sits in the 512 samples the next block bridges.
std::optional<ScenarioResult> latencyRiseDropsBridgedNote (ScenarioContext& ctx)
{
    static constexpr int kTrack = 0;
    static constexpr int kLookAhead = 512;
    static constexpr int kKey = 61;
    static constexpr std::int64_t kNoteTicks = 9;
    static constexpr std::int64_t kIntoBridge = 32;

    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto& track = session.track (kTrack);
    auto& strip = engine.getChannelStrip (kTrack);
    auto& slot = strip.getNativeClapSlot();

    ctx.pump (1);
    auto owned = std::make_unique<RecordingMidiBackend>();
    auto* const recorder = owned.get();
    engine.installMidiOutputBackend (std::move (owned));
    if (engine.getMidiOutputDevices().empty() || ! engine.ensureMidiOutputOpen (0))
        return ScenarioResult::fail ("the recording MIDI output would not open");

    ctx.keep (track.mode);
    ctx.cleanup ([&engine, &track, &slot]
    {
        engine.stop();
        track.midiOutputIndex.store (-1, std::memory_order_relaxed);
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>>());
        slot.unload();
    });
    track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
    const auto fixture = ctx.fixture ("latency.clap");
    std::string error;
    if (! ctx.expect (fixture.has_value()
                          && slot.load (*fixture, kRate, ScenarioContext::kBlockSize, error,
                                        "studio.dusk.test.latency"),
                      "the latency fixture did not load: " + error))
        return ctx.verdict();
    const clap::ClapInstance::ParamInfo* lookAhead = nullptr;
    for (int i = 0; i < slot.paramCount() && lookAhead == nullptr; ++i)
        if (const auto* info = slot.paramInfo (i); info != nullptr && info->name == "Look-ahead")
            lookAhead = info;
    if (! ctx.expect (lookAhead != nullptr, "the latency fixture has no Look-ahead"))
        return ctx.verdict();
    track.midiOutputIndex.store (0, std::memory_order_relaxed);

    transport.setLoopEnabled (false);
    transport.setPlayhead (0);
    engine.play();
    ctx.pump (8);
    slot.setParamValue (lookAhead->id, kLookAhead);
    ctx.pump (1);   // the fixture asks for its restart from process()

    ctx.waitUntil ([&strip] { return strip.getInsertPluginLatencySamples() == kLookAhead; }, 2000,
                   [&ctx, &engine, &transport, &session, &track, recorder]
    {
        // The next block plays from here, and so does its window.
        const auto from = transport.getPlayhead();
        const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
        const auto noteSamples = ticksToSamples (kNoteTicks, kRate, bpm);
        ctx.expect (kIntoBridge + noteSamples < kLookAhead, "the note does not fit the bridged stretch");
        auto regions = std::make_unique<std::vector<MidiRegion>>();
        MidiRegion region;
        region.timelineStart = from + kIntoBridge;
        region.lengthInTicks = 4 * kNoteTicks;
        region.lengthInSamples = ticksToSamples (region.lengthInTicks, kRate, bpm);
        region.notes.push_back ({ 1, kKey, 100, 0, kNoteTicks });
        regions->push_back (region);
        track.midiRegions.publish (std::move (regions));

        ctx.pump (16);
        engine.stop();
        ctx.pump (4);
        whenRecorderSettles (ctx, recorder, [&ctx] (const std::vector<RecordingMidiBackend::Message>& messages)
        {
            int ons = 0, offs = 0;
            bool sounding = false;
            for (const auto& m : messages)
            {
                const int kind = m.bytes[0] & 0xF0;
                if (m.numBytes != 3 || m.bytes[1] != kKey || (kind != 0x80 && kind != 0x90))
                    continue;
                sounding = kind == 0x90 && m.bytes[2] > 0;
                ++(sounding ? ons : offs);
            }
            ctx.note ("the bridged note: " + std::to_string (ons) + " note-on, "
                      + std::to_string (offs) + " note-off");
            ctx.expect (! sounding, "the note inside the bridged stretch was left sounding: its "
                                    "note-off went out ahead of its note-on");
            // Dropped, not missed: the window read the note, held its note-on
            // back and let its note-off go.
            ctx.expect (ons == 0 && offs == 1, "the note inside the bridged stretch was not dropped: "
                                               "expected its note-off alone, got " + std::to_string (ons)
                                               + " note-on, " + std::to_string (offs) + " note-off");
        });
    }, "the fixture's look-ahead never became the track's latency");
    return std::nullopt;
}
#endif

// A MIDI track's window runs its instrument's latency ahead of the block, so
// with Sunset at 2x it crosses the loop end a block before the transport does
// whenever the end falls just past a block boundary. The next window starts
// past the end but carries on from the last one, so the seam's reset, and the
// chase that re-attacks what the loop start holds, must not go out again. Each
// track's MIDI out carries exactly what its instrument is handed, into a
// recorder port of its own; a second track at 1x, with no latency, is the
// control. Every pass hands each instrument one reset and one note-on for each
// of two notes, one held into the loop start and one on it. The loop coming
// on, and its end moving in, while the 2x window is already past the end are
// jumps that do need their reset; Sunset's latency also rises and falls across
// a seam.
std::optional<ScenarioResult> sunsetLoopSeamResetsOnce (ScenarioContext& ctx)
{
    static constexpr const char* kSunset = "dusk.builtin.synth";
    if (builtin::findUnit (kSunset) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    static constexpr int kBlock = ScenarioContext::kBlockSize;
    static constexpr int kLatentTrack = 0, kPlainTrack = 1, kNumTracks = 2;
    // Whole blocks long and rolled from a block boundary, so the end lands
    // this far into a block on every pass.
    static constexpr int kSeamIntoBlock = 5;
    static constexpr std::int64_t kLoopEnd = 40 * kBlock + kSeamIntoBlock;
    static constexpr std::int64_t kLoopStart = kLoopEnd - 32 * kBlock;
    // The block whose window at 2x starts past the loop end, and the one
    // before it, whose window crosses the end.
    static constexpr std::int64_t kSeamBlock = kLoopEnd - kSeamIntoBlock;
    static constexpr std::int64_t kCrossingBlock = kSeamBlock - kBlock;
    // Mid-pass the loop end moves in to just past the block about to roll,
    // which keeps the loop whole blocks long.
    static constexpr std::int64_t kEditBlock = kLoopStart + (kBlock - kSeamIntoBlock) + 24 * kBlock;
    static constexpr int kHeld = 60, kOnStart = 64;

    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto* const recorder = sunsetsIntoRecorders (ctx, { 1.0f, 0.0f },
                                                 { { kHeld, kLoopStart - 1000 }, { kOnStart, kLoopStart } });
    if (recorder == nullptr)
        return ctx.verdict();
    auto& latentSlot = engine.getChannelStrip (kLatentTrack).getBuiltinSlot();
    const int latentOversampling = oversamplingParam (latentSlot);
    const int latency = latentSlot.getLatencySamples();
    ctx.note ("at 2x Sunset reports " + std::to_string (latency) + " samples");
    ctx.expect (latency > kSeamIntoBlock, "at 2x Sunset's window does not reach the loop end a block early");
    ctx.expect (engine.getChannelStrip (kPlainTrack).getBuiltinSlot().getLatencySamples() == 0,
                "at 1x Sunset reports latency");

    int wraps = 0;
    bool onPlan = true;
    const auto pumpBlock = [&]
    {
        const auto at = transport.getPlayhead();
        ctx.pump (1);
        if (transport.getPlayhead() < at)
            ++wraps;
    };
    // Rolls one block, then on until the next block starts at `at`.
    const auto rollTo = [&] (std::int64_t at)
    {
        pumpBlock();
        for (int guard = 0; transport.getPlayhead() != at && guard < 64; ++guard)
            pumpBlock();
        onPlan = onPlan && transport.getPlayhead() == at;
    };

    transport.setLoopRange (kLoopStart, kLoopEnd);
    transport.setLoopEnabled (false);
    transport.setPlayhead (0);
    engine.play();

    rollTo (kSeamBlock);
    transport.setLoopEnabled (true);
    rollTo (kSeamBlock);
    rollTo (kCrossingBlock);
    // The switch lands in the block it is pumped with, so the window that
    // starts past the end is the first at 4x, stretched; a pass on, the first
    // back at 2x, shortened.
    latentSlot.setParamValue (latentOversampling, 2.0f);
    rollTo (kCrossingBlock);
    const int raised = latentSlot.getLatencySamples();
    latentSlot.setParamValue (latentOversampling, 1.0f);
    rollTo (kSeamBlock);
    rollTo (kSeamBlock);
    rollTo (kEditBlock);
    transport.setLoopRange (kLoopStart, kEditBlock + kSeamIntoBlock);
    rollTo (kEditBlock);
    for (int i = 0; i < 8; ++i)
        pumpBlock();
    engine.stop();
    ctx.pump (4);

    ctx.note ("at 4x Sunset reports " + std::to_string (raised) + " samples");
    ctx.expect (raised > latency, "the switch to 4x raised no latency");
    ctx.expect (onPlan, "a block did not start where the passes were planned");

    // The roll's start and Stop reset Sunset too.
    whenRecorderSettles (ctx, recorder, [&ctx, wraps] (const std::vector<RecordingMidiBackend::Message>& messages)
    {
        for (int t = 0; t < kNumTracks; ++t)
            expectHanded (ctx, t == kLatentTrack ? "at 2x" : "at 1x", tallyPort (messages, t), wraps + 2,
                          { { "the note held into the loop start", kHeld, wraps + 1 },
                            { "the note on the loop start", kOnStart, wraps + 1 } });
    });
    return std::nullopt;
}

// At no latency a MIDI track's window is its block, so a loop end on a block
// boundary never falls inside one: the block before the seam ends on it, and
// the transport has wrapped before the next. The seam still resets the
// instrument and chases what the loop start holds, once, at the head of the
// block after it, so a note sounding into the loop end stops there instead of
// hanging until Stop. Sunset at 2x, whose window crosses the same seam inside
// a block, is the control. On every pass, once the notes in the loop's first
// half have died away and before the one over the loop end comes round again,
// both tracks are silent.
std::optional<ScenarioResult> sunsetLoopSeamOnBlockBoundary (ScenarioContext& ctx)
{
    static constexpr const char* kSunset = "dusk.builtin.synth";
    if (builtin::findUnit (kSunset) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    static constexpr int kBlock = ScenarioContext::kBlockSize;
    static constexpr int kPlainTrack = 0, kLatentTrack = 1, kNumTracks = 2;
    static constexpr std::int64_t kLoopStart = 16 * kBlock;
    static constexpr std::int64_t kLoopEnd = kLoopStart + 128 * kBlock;
    static constexpr int kSeams = 3;
    static constexpr int kHeld = 60, kOnStart = 64, kOverEnd = 67;
    // Every note but the one over the loop end, which is still to come, ended
    // over half a second before, longer than Sunset's release takes to die.
    static constexpr std::int64_t kQuietAt = kLoopEnd - 8 * kBlock;
    static constexpr float kQuietDb = -60.0f;

    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto* const recorder = sunsetsIntoRecorders (
        ctx, { 0.0f, 1.0f },
        { { kHeld, kLoopStart - 1000 }, { kOnStart, kLoopStart }, { kOverEnd, kLoopEnd - 2000 } });
    if (recorder == nullptr)
        return ctx.verdict();
    ctx.expect (engine.getChannelStrip (kPlainTrack).getBuiltinSlot().getLatencySamples() == 0,
                "at 1x Sunset reports latency");
    ctx.expect (engine.getChannelStrip (kLatentTrack).getBuiltinSlot().getLatencySamples() > 0,
                "at 2x Sunset reports no latency");

    transport.setLoopRange (kLoopStart, kLoopEnd);
    transport.setLoopEnabled (true);
    transport.setPlayhead (kLoopStart);
    engine.play();

    const auto name = [] (int t) { return std::string (t == kLatentTrack ? "at 2x" : "at 1x"); };
    int wraps = 0, passes = 0;
    std::array<float, kNumTracks> loudestDb { -100.0f, -100.0f };
    for (int guard = 0; passes <= kSeams && guard < (kSeams + 2) * 128; ++guard)
    {
        const auto at = transport.getPlayhead();
        ctx.pump (1);
        if (transport.getPlayhead() < at)
            ++wraps;
        for (int t = 0; t < kNumTracks; ++t)
            loudestDb[(std::size_t) t] = std::max (loudestDb[(std::size_t) t],
                                                   engine.getChannelStrip (t).getOutLDb());
        if (transport.getPlayhead() != kQuietAt)
            continue;
        ++passes;
        for (int t = 0; t < kNumTracks; ++t)
        {
            const float level = engine.getChannelStrip (t).getOutLDb();
            const auto where = name (t) + ", pass " + std::to_string (passes);
            ctx.note (where + ": " + std::to_string (level) + " dB before the note over the loop end");
            ctx.expect (level <= kQuietDb, where + ": Sunset still sounds at " + std::to_string (level)
                                               + " dB after every note in the pass has ended");
        }
    }
    engine.stop();
    ctx.pump (4);
    ctx.expect (wraps == kSeams, "the transport wrapped " + std::to_string (wraps) + " times, not "
                                     + std::to_string (kSeams));
    for (int t = 0; t < kNumTracks; ++t)
        ctx.expect (loudestDb[(std::size_t) t] > kQuietDb + 20.0f, name (t) + ", Sunset never sounded");

    // The roll's start and Stop reset Sunset too, and the run stops short of
    // the note over the loop end on the last pass.
    whenRecorderSettles (ctx, recorder, [&ctx, name, wraps] (const std::vector<RecordingMidiBackend::Message>& messages)
    {
        for (int t = 0; t < kNumTracks; ++t)
            expectHanded (ctx, name (t), tallyPort (messages, t), wraps + 2,
                          { { "the note held into the loop start", kHeld, wraps + 1 },
                            { "the note on the loop start", kOnStart, wraps + 1 },
                            { "the note over the loop end", kOverEnd, wraps } });
    });
    return std::nullopt;
}

// Sunset at 2x is scheduled its latency ahead, so with the loop end just past
// a block boundary its window crosses the seam a block before the transport
// does: it resets Sunset there and starts what the loop start holds. Switched
// off before that next block, the loop never wraps the transport, which runs
// straight on through the loop end. What Sunset was started from the loop
// start must not hang there, and the note sounding over the loop end must come
// back: the next block resets Sunset again and chases that note, which then
// ends on its own. At 1x the window had not reached the seam, and Sunset plays
// straight through.
std::optional<ScenarioResult> sunsetLoopOffPastTheSeam (ScenarioContext& ctx)
{
    static constexpr const char* kSunset = "dusk.builtin.synth";
    if (builtin::findUnit (kSunset) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    static constexpr int kBlock = ScenarioContext::kBlockSize;
    static constexpr int kPlainTrack = 0, kLatentTrack = 1, kNumTracks = 2;
    static constexpr int kSeamIntoBlock = 5;
    static constexpr std::int64_t kLoopStart = 16 * kBlock;
    static constexpr std::int64_t kLoopEnd = kLoopStart + 32 * kBlock + kSeamIntoBlock;
    // The block whose window at 2x starts past the loop end.
    static constexpr std::int64_t kSeamBlock = kLoopEnd - kSeamIntoBlock;
    // A third of a second after the note over the loop end has ended.
    static constexpr std::int64_t kQuietAt = kSeamBlock + 64 * kBlock;
    static constexpr int kHeld = 60, kOnStart = 64, kOverEnd = 67;
    static constexpr float kQuietDb = -60.0f;

    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();
    auto* const recorder = sunsetsIntoRecorders (
        ctx, { 0.0f, 1.0f },
        { { kHeld, kLoopStart - 1000 }, { kOnStart, kLoopStart }, { kOverEnd, kLoopEnd - 2000 } });
    if (recorder == nullptr)
        return ctx.verdict();
    ctx.expect (engine.getChannelStrip (kPlainTrack).getBuiltinSlot().getLatencySamples() == 0,
                "at 1x Sunset reports latency");
    ctx.expect (engine.getChannelStrip (kLatentTrack).getBuiltinSlot().getLatencySamples() > kSeamIntoBlock,
                "at 2x Sunset's window does not reach the loop end a block early");

    const auto name = [] (int t) { return std::string (t == kLatentTrack ? "at 2x" : "at 1x"); };
    std::array<float, kNumTracks> loudestDb { -100.0f, -100.0f };
    bool onPlan = true;
    const auto rollTo = [&] (std::int64_t at)
    {
        for (int guard = 0; transport.getPlayhead() != at && guard < 128; ++guard)
        {
            ctx.pump (1);
            for (int t = 0; t < kNumTracks; ++t)
                loudestDb[(std::size_t) t] = std::max (loudestDb[(std::size_t) t],
                                                       engine.getChannelStrip (t).getOutLDb());
        }
        onPlan = onPlan && transport.getPlayhead() == at;
    };

    transport.setLoopRange (kLoopStart, kLoopEnd);
    transport.setLoopEnabled (true);
    transport.setPlayhead (kLoopStart);
    engine.play();
    rollTo (kSeamBlock);
    transport.setLoopEnabled (false);
    rollTo (kQuietAt);
    for (int t = 0; t < kNumTracks; ++t)
    {
        const float level = engine.getChannelStrip (t).getOutLDb();
        ctx.note (name (t) + ": " + std::to_string (level) + " dB after the last note");
        ctx.expect (level <= kQuietDb, name (t) + ": Sunset still sounds at " + std::to_string (level)
                                           + " dB a third of a second after the last note ended");
        ctx.expect (loudestDb[(std::size_t) t] > kQuietDb + 20.0f, name (t) + ", Sunset never sounded");
    }
    engine.stop();
    ctx.pump (4);
    ctx.expect (onPlan, "a block did not start where the roll was planned");

    // At 2x the roll's start, the seam, the return to the timeline past the
    // loop end, and Stop each reset Sunset; at 1x only the start and Stop do.
    whenRecorderSettles (ctx, recorder, [&ctx] (const std::vector<RecordingMidiBackend::Message>& messages)
    {
        expectHanded (ctx, "at 2x", tallyPort (messages, kLatentTrack), 4,
                      { { "the note held into the loop start", kHeld, 2 },
                        { "the note on the loop start", kOnStart, 2 },
                        { "the note over the loop end", kOverEnd, 2 } });
        expectHanded (ctx, "at 1x", tallyPort (messages, kPlainTrack), 2,
                      { { "the note held into the loop start", kHeld, 1 },
                        { "the note on the loop start", kOnStart, 1 },
                        { "the note over the loop end", kOverEnd, 1 } });
    });
    return std::nullopt;
}

// ---------------------------------------------------------- Export master

// Export master renders the mastering chain on the loaded mix: the file is
// captured after the limiter, and the CD preset is 16-bit at 44.1 kHz.
std::optional<ScenarioResult> exportMasterPostLimiter (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& mastering = ctx.session().mastering();
    const auto source = ctx.tempDir() / "loud.wav";
    if (! writeMono (source, sine (1000.0f, 0.9f, 48000)))
        return ScenarioResult::fail ("could not write the source mix");

    auto& player = engine.getMasteringPlayer();
    if (! player.loadFile (sessionFile (source)))
        return ScenarioResult::fail ("the mastering player would not load the mix");
    const bool limiterWas = mastering.limiterEnabled.load (std::memory_order_relaxed);
    const float driveWas = mastering.limiterDriveDb.load (std::memory_order_relaxed);
    ctx.cleanup ([&player, &mastering, limiterWas, driveWas]
    {
        player.unloadFile();
        mastering.limiterEnabled.store (limiterWas, std::memory_order_relaxed);
        mastering.limiterDriveDb.store (driveWas, std::memory_order_relaxed);
    });
    mastering.limiterEnabled.store (true, std::memory_order_relaxed);
    mastering.limiterDriveDb.store (6.0f, std::memory_order_relaxed);
    const float ceiling = std::pow (10.0f, mastering.limiterCeilingDb.load (std::memory_order_relaxed) / 20.0f);

    const auto out = ctx.sessionDir() / "master-cd.wav";
    Render render;
    render.mode = BounceEngine::Mode::MasteringChain;
    render.sampleRate = 44100.0;
    render.wavBitDepth = 16;
    render.tailSeconds = 0.5;
    renderThen (ctx, out, render, [&ctx, out, ceiling] (bool ok, const std::string& error)
    {
        const auto master = readBack (out);
        if (ctx.expect (ok && master.has_value(), "the export failed: " + error))
        {
            ctx.note ("rate " + std::to_string (master->info.sampleRate) + ", bits "
                      + std::to_string (master->info.bitsPerSample) + ", peak "
                      + std::to_string (master->peak()) + ", ceiling " + std::to_string (ceiling));
            ctx.expect (std::abs (master->info.sampleRate - 44100.0) < 0.5, "the CD preset is not at 44.1 kHz");
            ctx.expect (master->info.bitsPerSample == 16, "the CD preset is not 16-bit");
            ctx.expect (master->peak() > 0.5f, "the export is nearly silent");
            // A 16-bit sample and its dither add a few LSB on top of the ceiling.
            ctx.expect (master->peak() <= ceiling + 4.0f / 32768.0f,
                        "the export went over the limiter ceiling, so it was not captured after the limiter");
        }
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}

std::optional<ScenarioResult> masteringMonoMonitor (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& player = engine.getMasteringPlayer();
    auto& feed = engine.getMasteringChain().getPhaseFeed();
    const auto oldStage = engine.getStage();
    const bool oldMono = engine.getMasteringMonoMonitor();
    ctx.cleanup ([&engine, &player, &feed, oldStage, oldMono]
    {
        player.stop();
        player.unloadFile();
        feed.discardPending();
        engine.setMasteringMonoMonitor (oldMono);
        engine.setStage (oldStage);
    });
    auto& params = ctx.session().mastering();
    ctx.keep (params.eqEnabled);
    ctx.keep (params.compEnabled);
    ctx.keep (params.limiterEnabled);
    params.eqEnabled.store (false);
    params.compEnabled.store (false);
    params.limiterEnabled.store (false);
    const auto left = sine (440.0f, 0.25f, 96000);
    const auto right = sine (997.0f, 0.08f, 96000);
    const auto source = ctx.tempDir() / "stereo-monitor.wav";
    dusk::audio::WriteSpec spec;
    spec.sampleRate = kRate;
    spec.numChannels = 2;
    spec.bitsPerSample = 32;
    {
        auto writer = dusk::audio::FileWriter::create (source, spec);
        const float* channels[] = { left.data(), right.data() };
        if (writer == nullptr || ! writer->write (channels, 2, (std::int64_t) left.size())
            || ! writer->flush())
            return ScenarioResult::fail ("could not write the asymmetric stereo mix");
    }
    engine.setStage (AudioEngine::Stage::Mastering);
    engine.setMasteringMonoMonitor (false);
    if (! player.loadFile (sessionFile (source)))
        return ScenarioResult::fail ("could not load the asymmetric stereo mix");
    player.play();
    feed.discardPending();
    std::array<float, ScenarioContext::kBlockSize> feedL {}, feedR {};
    for (const bool mono : { false, true, false })
    {
        engine.setMasteringMonoMonitor (mono);
        float maxError = 0.0f, stereoDifference = 0.0f, outputPeak = 0.0f;
        for (int block = 0; block < 24; ++block)
        {
            if (! player.primeOfflineBlock (ScenarioContext::kBlockSize))
                return ScenarioResult::fail ("could not prime the live monitor block");
            ctx.pump (1);
            const auto chunk = feed.read (feedL.data(), feedR.data(), ScenarioContext::kBlockSize);
            if (! ctx.expect (chunk.samples == ScenarioContext::kBlockSize, "missing live stereo analysis block"))
                return ctx.verdict();
            if (block < 12) continue;
            for (std::size_t i = 0; i < feedL.size(); ++i)
            {
                ctx.expect (std::isfinite (feedL[i]) && std::isfinite (feedR[i])
                            && std::isfinite (ctx.lastBlock (0)[i]) && std::isfinite (ctx.lastBlock (1)[i]),
                            "the monitor or phase feed contains a non-finite sample");
                const float mid = 0.5f * feedL[i] + 0.5f * feedR[i];
                maxError = std::max ({ maxError,
                    std::abs (ctx.lastBlock (0)[i] - (mono ? mid : feedL[i])),
                    std::abs (ctx.lastBlock (1)[i] - (mono ? mid : feedR[i])) });
                stereoDifference = std::max (stereoDifference, std::abs (feedL[i] - feedR[i]));
                outputPeak = std::max (outputPeak, std::abs (ctx.lastBlock (0)[i]));
            }
        }
        ctx.expect (maxError < 1.0e-6f, mono ? "Mono did not fold only the live monitor"
                                             : "stereo monitoring differs from the analysis tap");
        ctx.expect (stereoDifference > 0.01f, "the analysis feed lost its stereo difference");
        ctx.expect (outputPeak > 0.01f, "the live monitor is silent");
    }
    player.stop();
    const auto baselinePath = ctx.sessionDir() / "master-stereo.wav";
    const auto monoPath = ctx.sessionDir() / "master-with-monitor-mono.wav";
    Render render;
    render.mode = BounceEngine::Mode::MasteringChain;
    render.sampleRate = 0.0;
    render.tailSeconds = 0.2;
    renderThen (ctx, baselinePath, render, [&ctx, baselinePath, monoPath, render] (bool ok, const std::string& error)
    {
        const auto baseline = readBack (baselinePath);
        if (! ctx.expect (ok && baseline.has_value(), "the stereo export failed: " + error))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        ctx.expect (ctx.engine().getMasteringChain().getPhaseFeed().pendingSamples() == 0,
                    "offline export entered the live analysis feed");
        ctx.engine().setMasteringMonoMonitor (true);
        renderThen (ctx, monoPath, render, [&ctx, baseline, monoPath] (bool monoOk, const std::string& monoError)
        {
            const auto monitored = readBack (monoPath);
            if (ctx.expect (monoOk && monitored.has_value(), "the Mono-monitor export failed: " + monoError)
                && ctx.expect (monitored->info.numChannels == 2 && baseline->info.numChannels == 2
                               && monitored->left.size() == baseline->left.size(), "export channel or frame count changed"))
            {
                float maxError = 0.0f, stereoDifference = 0.0f;
                for (std::size_t i = 0; i < baseline->left.size(); ++i)
                {
                    ctx.expect (std::isfinite (baseline->left[i]) && std::isfinite (baseline->right[i])
                                && std::isfinite (monitored->left[i]) && std::isfinite (monitored->right[i]),
                                "the export contains a non-finite sample");
                    maxError = std::max ({ maxError, std::abs (baseline->left[i] - monitored->left[i]),
                                          std::abs (baseline->right[i] - monitored->right[i]) });
                    stereoDifference = std::max (stereoDifference, std::abs (baseline->left[i] - baseline->right[i]));
                }
                ctx.expect (baseline->peak() > 0.01f && stereoDifference > 0.01f, "the reference export is silent or mono");
                ctx.expect (maxError <= 1.0e-6f, "Mono monitoring changed the exported stereo samples");
                ctx.expect (ctx.engine().getMasteringMonoMonitor(), "the export changed the monitor switch");
                ctx.expect (ctx.engine().getMasteringChain().getPhaseFeed().pendingSamples() == 0,
                            "Mono export entered the live analysis feed");
            }
            ctx.complete (ctx.verdict());
        });
    });
    return std::nullopt;
}

std::optional<ScenarioResult> masteringPhaseContinuity (ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& player = engine.getMasteringPlayer();
    auto& feed = engine.getMasteringChain().getPhaseFeed();
    const auto oldStage = engine.getStage();
    ctx.cleanup ([&engine, &player, &feed, oldStage]
    {
        player.stop();
        player.unloadFile();
        feed.discardPending();
        engine.setStage (oldStage);
    });
    constexpr int kFrames = 3 * dusk::audio::BufferedFileReader::kDefaultWindowFrames;
    const auto source = ctx.tempDir() / "phase-continuity.wav";
    if (! writeMono (source, sine (440.0f, 0.1f, kFrames)))
        return ScenarioResult::fail ("could not write the continuity mix");
    engine.setStage (AudioEngine::Stage::Mastering);
    if (! player.loadFile (sessionFile (source)))
        return ScenarioResult::fail ("could not load the continuity mix");
    feed.discardPending();
    std::array<float, ScenarioContext::kBlockSize> left {}, right {};
    const auto block = [&] (bool prime = true)
    {
        if (prime) ctx.expect (player.primeOfflineBlock (ScenarioContext::kBlockSize), "could not prime a continuity block");
        ctx.pump (1);
        const auto chunk = feed.read (left.data(), right.data(), ScenarioContext::kBlockSize);
        if (prime) ctx.expect (chunk.samples == ScenarioContext::kBlockSize, "missing primed continuity block");
        return chunk;
    };
    player.play();
    const auto first = block();
    const auto adjacent = block();
    ctx.expect (first.samples == ScenarioContext::kBlockSize && adjacent.samples == ScenarioContext::kBlockSize,
                "normal playback did not enter the phase feed");
    ctx.expect (first.generation == adjacent.generation
                && adjacent.sequence == first.sequence + first.samples, "adjacent playback lost continuity");
    player.setPlayhead (player.getPlayhead());
    const auto seek = block();
    ctx.expect (seek.generation != adjacent.generation, "a same-position seek retained the FFT window");
    player.stop();
    const auto stopped = block();
    const auto tail = block();
    ctx.expect (stopped.samples == ScenarioContext::kBlockSize && tail.samples == ScenarioContext::kBlockSize,
                "stopped-chain tails were excluded from analysis");
    ctx.expect (stopped.generation != seek.generation && tail.generation == stopped.generation,
                "Stop did not start one continuous tail generation");

    player.play();
    const auto resumed = block();
    ctx.expect (resumed.generation != tail.generation, "Play did not reset continuity");
    engine.suspendProcessing();
    ctx.pump (1);
    engine.resumeProcessing();
    const auto gated = block();
    ctx.expect (gated.generation != resumed.generation, "a process-gate interval retained old analysis");

    player.setPlayhead (kFrames - 4096);
    const auto missing = block (false);
    ctx.expect (missing.samples == 0, "an unavailable reader block entered phase analysis");
    const auto recovered = block();
    ctx.expect (recovered.samples == ScenarioContext::kBlockSize && recovered.generation != gated.generation,
                "a recovered reader block reused the pre-gap generation");
    player.setPlayhead (kFrames - 128);
    const auto final = block();
    const auto eofTail = block();
    ctx.expect (! player.isPlaying(), "the player did not reach EOF");
    ctx.expect (final.samples == ScenarioContext::kBlockSize && eofTail.samples == ScenarioContext::kBlockSize
                && final.generation == eofTail.generation, "EOF split the limiter tail from its final block");
    engine.prepareForSelfTest (96000.0, ScenarioContext::kBlockSize);
    ctx.cleanup ([&engine] { engine.prepareForSelfTest (kRate, ScenarioContext::kBlockSize); });
    const auto prepared = block();
    ctx.expect (prepared.generation != eofTail.generation && std::abs (prepared.sampleRate - 96000.0) < 0.5,
                "a device-rate change retained old phase metadata");
    engine.setStage (AudioEngine::Stage::Mixing);
    ctx.pump (1);
    engine.setStage (AudioEngine::Stage::Mastering);
    const auto returned = block();
    ctx.expect (returned.generation != prepared.generation, "leaving and returning to Mastering retained old analysis");
    return ctx.verdict();
}

// The export presets that do not leave the session rate. Every export plays the
// loaded mix through the mastering chain; `check` inspects the file.
std::optional<ScenarioResult> exportMaster (ScenarioContext& ctx, const std::vector<float>& mix,
                                            const Render& render, const char* name,
                                            std::function<void (const std::filesystem::path&)> check)
{
    auto& player = ctx.engine().getMasteringPlayer();
    const auto source = ctx.tempDir() / "mix.wav";
    if (! writeMono (source, mix))
        return ScenarioResult::fail ("could not write the source mix");
    if (! player.loadFile (sessionFile (source)))
        return ScenarioResult::fail ("the mastering player would not load the mix");
    ctx.cleanup ([&player] { player.unloadFile(); });

    const auto out = ctx.sessionDir() / name;
    renderThen (ctx, out, render, [&ctx, out, check] (bool ok, const std::string& error)
    {
        if (ctx.expect (ok, "the export failed: " + error))
            check (out);
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
}

// WAV 24-bit keeps the session rate: the preset passes no rate of its own.
std::optional<ScenarioResult> exportMasterWav24 (ScenarioContext& ctx)
{
    Render render;
    render.mode = BounceEngine::Mode::MasteringChain;
    render.sampleRate = 0.0;
    render.tailSeconds = 0.5;
    return exportMaster (ctx, sine (1000.0f, 0.25f, 48000), render, "master.wav",
                         [&ctx] (const std::filesystem::path& out)
    {
        const auto master = readBack (out);
        if (! ctx.expect (master.has_value(), "the export wrote no readable file"))
            return;
        const auto& info = master->info;
        ctx.note ("rate " + std::to_string (info.sampleRate) + ", bits " + std::to_string (info.bitsPerSample)
                  + ", channels " + std::to_string (info.numChannels) + ", peak " + std::to_string (master->peak()));
        ctx.expect (std::abs (info.sampleRate - kRate) < 0.5, "the 24-bit preset is not at the session rate");
        ctx.expect (info.bitsPerSample == 24 && ! info.isFloat, "the 24-bit preset is not 24-bit PCM");
        ctx.expect (info.numChannels == 2, "the export is not stereo");
        ctx.expect (master->peak() > 0.1f, "the export is nearly silent");
    });
}

// MP3 320 kbps: every frame header carries MPEG-1 Layer III at 320 kbps and the
// session's 48 kHz.
std::optional<ScenarioResult> exportMasterMp3 (ScenarioContext& ctx)
{
   #if DUSKSTUDIO_HAS_LAME
    Render render;
    render.mode = BounceEngine::Mode::MasteringChain;
    render.format = BounceEngine::Format::Mp3;
    render.sampleRate = 0.0;
    render.tailSeconds = 0.5;
    const auto mix = sine (1000.0f, 0.25f, (int) kRate);
    // An MPEG-1 Layer III frame holds 1152 samples, so the mix and its tail
    // need at least this many.
    const auto minFrames = (int) std::ceil (((double) mix.size() + render.tailSeconds * kRate) / 1152.0);
    return exportMaster (ctx, mix, render, "master.mp3",
                         [&ctx, minFrames] (const std::filesystem::path& out)
    {
        std::vector<unsigned char> bytes;
        if (std::FILE* file = std::fopen (out.u8string().c_str(), "rb"))
        {
            unsigned char chunk[4096];
            std::size_t got = 0;
            while ((got = std::fread (chunk, 1, sizeof (chunk), file)) > 0)
                bytes.insert (bytes.end(), chunk, chunk + got);
            std::fclose (file);
        }
        std::size_t at = 0;
        if (bytes.size() >= 10 && bytes[0] == 'I' && bytes[1] == 'D' && bytes[2] == '3')
            at = 10 + (((std::size_t) bytes[6] & 0x7F) << 21 | ((std::size_t) bytes[7] & 0x7F) << 14
                       | ((std::size_t) bytes[8] & 0x7F) << 7 | ((std::size_t) bytes[9] & 0x7F));
        // At 320 kbps and 48 kHz an MPEG-1 Layer III frame is 144 * 320000 / 48000
        // = 960 bytes, plus one when the padding bit is set.
        int frames = 0, wrong = 0;
        while (at + 4 <= bytes.size() && bytes[at] == 0xFF && (bytes[at + 1] & 0xE0) == 0xE0)
        {
            const int version = (bytes[at + 1] >> 3) & 3;
            const int layer   = (bytes[at + 1] >> 1) & 3;
            const int rate    = (bytes[at + 2] >> 4) & 0xF;
            const int srIndex = (bytes[at + 2] >> 2) & 3;
            const int padding = (bytes[at + 2] >> 1) & 1;
            const auto length = (std::size_t) (960 + padding);
            if (length > bytes.size() - at)
                break;
            if (version != 3 || layer != 1 || rate != 14 || srIndex != 1)
                ++wrong;
            ++frames;
            at += length;
        }
        ctx.note (std::to_string (frames) + " frames, " + std::to_string (wrong) + " not 320 kbps MPEG-1 Layer III at 48 kHz, "
                  + std::to_string (bytes.size() - std::min (at, bytes.size())) + " trailing bytes");
        ctx.expect (frames >= minFrames, "the MP3 export has " + std::to_string (frames) + " frames, short of the "
                                             + std::to_string (minFrames) + " the mix and its tail need");
        ctx.expect (wrong == 0, "an MP3 frame is not 320 kbps MPEG-1 Layer III at 48 kHz");
    });
   #else
    (void) ctx;
    return noMp3Encoder();
   #endif
}

// The CD preset's dither is TPDF at +/-1 LSB and not noise-shaped. On a silent
// mix the file holds the dither alone: a triangular +/-1 LSB draw rounds to +1
// or -1 LSB one time in eight each and to 0 the rest, and an unshaped (white)
// dither leaves neighbouring samples uncorrelated, where a shaped one would
// correlate them.
std::optional<ScenarioResult> exportMasterDither (ScenarioContext& ctx)
{
    Render render;
    render.mode = BounceEngine::Mode::MasteringChain;
    render.sampleRate = 44100.0;
    render.wavBitDepth = 16;
    render.tailSeconds = 0.5;
    return exportMaster (ctx, std::vector<float> (48000, 0.0f), render, "master-cd.wav",
                         [&ctx] (const std::filesystem::path& out)
    {
        const auto master = readBack (out);
        if (! ctx.expect (master.has_value(), "the export wrote no readable file"))
            return;
        ctx.expect (master->info.bitsPerSample == 16, "the CD preset is not 16-bit");
        std::int64_t total = 0, up = 0, down = 0, wide = 0;
        double lag0 = 0.0, lag1 = 0.0;
        for (const auto* channel : { &master->left, &master->right })
        {
            long previous = 0;
            for (const float s : *channel)
            {
                const long lsb = std::lround (s * 32768.0f);
                if (lsb > 1 || lsb < -1) ++wide;
                if (lsb == 1) ++up;
                if (lsb == -1) ++down;
                lag0 += (double) (lsb * lsb);
                lag1 += (double) (lsb * previous);
                previous = lsb;
                ++total;
            }
        }
        const double pUp = (double) up / (double) std::max<std::int64_t> (1, total);
        const double pDown = (double) down / (double) std::max<std::int64_t> (1, total);
        const double r1 = lag0 > 0.0 ? lag1 / lag0 : 1.0;
        ctx.note ("samples " + std::to_string (total) + ", +1 LSB " + std::to_string (pUp) + ", -1 LSB "
                  + std::to_string (pDown) + ", wider " + std::to_string (wide) + ", lag-1 correlation "
                  + std::to_string (r1));
        ctx.expect (total > 44100, "the export is too short to measure the dither");
        ctx.expect (wide == 0, "the dither on silence reaches past 1 LSB");
        ctx.expect (pUp > 0.1 && pUp < 0.15 && pDown > 0.1 && pDown < 0.15,
                    "the dither does not round like a +/-1 LSB triangular draw");
        ctx.expect (std::abs (r1) < 0.05, "neighbouring dither samples correlate, so the dither is shaped");
    });
}

std::optional<ScenarioResult> pendingInstrumentRefusesRender (ScenarioContext& ctx, bool restoring)
{
   #if DUSKSTUDIO_HAS_MULTISAMPLE
    auto& engine = ctx.engine();
    auto& track = ctx.session().track (0);
    const auto path = ctx.tempDir() / "pending.sfz";
    if (! dusk::fs::writeStringToFile (path, "<region> sample=*sine key=60"))
        return ScenarioResult::fail ("could not write the pending instrument");
    ctx.keep (engine.getChannelStrip (0).insertMode);
    ctx.keep (track.mode);
    ctx.keep (track.frozen);
    ctx.cleanup ([&engine, &track, regions = track.midiRegions.current(),
                  savedPath = track.nativeMultisamplePath, locator = track.nativeMultisampleLocator,
                  state = track.nativeMultisampleStateBase64]
    {
        engine.cancelNativeMultisampleLoad (0);
        engine.suspendProcessing();
        engine.getChannelStrip (0).unloadNativeMultisample();
        engine.resumeProcessing();
        track.nativeMultisamplePath = savedPath;
        track.nativeMultisampleLocator = locator;
        track.nativeMultisampleStateBase64 = state;
        track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (regions));
    });
    track.mode.store ((int) Track::Mode::Midi);
    track.frozen.store (false);
    MidiRegion region;
    region.lengthInSamples = 48000;
    region.lengthInTicks = 1920;
    region.notes.push_back ({ 1, 60, 100, 0, 960 });
    track.midiRegions.publish (std::make_unique<std::vector<MidiRegion>> (1, region));
    auto held = std::make_shared<bool> (true);
    engine.setAssetRenderActive (true);
    ctx.cleanup ([&engine, held] { if (*held) engine.setAssetRenderActive (false); });
    if (! engine.requestNativeMultisampleLoad (0, path, {}, {}, -1, restoring))
        return ScenarioResult::fail ("could not queue the pending instrument");
    ctx.expect (engine.hasPendingMultisampleLoads(), "the instrument was not pending");
    auto bounce = std::make_shared<BounceEngine> (engine, ctx.session());
    ctx.cleanup ([bounce] { bounce->cancel(); });
    auto accepted = std::make_shared<int> (0);
    bounce->onAccepted = [accepted] { ++*accepted; };
    const auto output = ctx.tempDir() / "render.wav";
    const auto file = sessionFile (output);
    const std::string sentinel = "existing output must survive refusal";
    if (! dusk::fs::writeStringToFile (output, sentinel))
        return ScenarioResult::fail ("could not seed the existing output");
    for (int attempt = 0; attempt < 6; ++attempt)
    {
        const bool started = attempt == 5
            ? bounce->startFreeze (0, file, 48000, kRate, 256)
            : bounce->start (file, kRate, 256, 0.0,
                attempt == 2 ? BounceEngine::Mode::MasteringChain
                    : attempt == 1 || attempt == 4 ? BounceEngine::Mode::Stems : BounceEngine::Mode::MasterMix,
                BounceEngine::Format::Wav, 320, 24, attempt == 3 || attempt == 4);
        ctx.expect (! started, "render accepted a pending instrument at attempt " + std::to_string (attempt));
        if (started)
        {
            bounce->cancel();
            ctx.waitUntil ([bounce] { return ! bounce->isRendering(); }, kRenderTimeoutMs,
                [&ctx] { ctx.complete (ctx.verdict()); }, "the wrongly accepted render did not stop");
            return std::nullopt;
        }
        ctx.expect (bounce->getLastError() == "Wait for soundfont loading to finish, then render again.",
                    "the refusal did not explain how to retry");
        ctx.expect (! bounce->isRendering() && *accepted == 0, "refusal changed render state or accepted the target");
        ctx.expect (dusk::fs::loadFileAsString (output) == sentinel, "refusal changed the existing output");
    }
    engine.setAssetRenderActive (false);
    *held = false;
    ctx.waitUntil ([&engine] { engine.serviceMultisampleLoads(); return ! engine.hasPendingMultisampleLoads(); },
        10000, [&ctx, &engine]
        {
            if (! ctx.expect (engine.getChannelStrip (0).getNativeMultisampleSlot().getInstance() != nullptr,
                              "the instrument did not load after refusal"))
            { ctx.complete (ctx.verdict()); return; }
            const auto rendered = ctx.tempDir() / "loaded.wav";
            Render render;
            render.tailSeconds = 0.0;
            renderThen (ctx, rendered, render, [&ctx, rendered] (bool ok, const std::string& error)
            {
                ctx.expect (ok, "the loaded instrument could not render: " + error);
                const auto audio = readBack (rendered);
                ctx.expect (audio && audio->peak() > 0.001f, "the loaded instrument rendered silently");
                ctx.complete (ctx.verdict());
            });
        }, "the pending instrument did not settle");
    return std::nullopt;
   #else
    (void) ctx; (void) restoring;
    return ScenarioResult::skip ("built without native multisample instruments");
   #endif
}

const ScenarioRegistrar mixRegistrar { Scenario {
    "bounce.master_mix_file", { "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return masterMixFile (ctx); }, 120000 } };
const ScenarioRegistrar wholeSongLoopRegistrar { Scenario {
    "bounce.whole_song_ignores_playback_loop", { "bounce", "stems", "freeze", "loop" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return wholeSongIgnoresPlaybackLoop (ctx); }, 180000 } };
const ScenarioRegistrar offlineMidiRegistrar { Scenario {
    "bounce.offline_midi_isolation", { "bounce", "stems", "freeze", "midi", "sync" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return offlineMidiIsolation (ctx); }, 180000 } };
const ScenarioRegistrar offlineHardwareRegistrar { Scenario {
    "bounce.offline_hardware_is_dry", { "bounce", "stems", "freeze", "hardware" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return offlineHardwareIsDry (ctx); }, 180000 } };
const ScenarioRegistrar freezeGateRegistrar { Scenario {
    "bounce.freeze_ignores_mix_gate", { "bounce", "freeze", "mute", "solo" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return freezeIgnoresMixGate (ctx); }, 180000 } };
const ScenarioRegistrar offlineCueRegistrar { Scenario {
    "bounce.offline_cue_does_not_print", { "bounce", "aux", "cue" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return offlineCueDoesNotPrint (ctx); }, 180000 } };
const ScenarioRegistrar defaultRateRegistrar { Scenario {
    "bounce.default_rate_without_device", { "bounce", "stems", "freeze", "rate" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return defaultRenderRateWithoutDevice (ctx); }, 180000 } };
const ScenarioRegistrar mp3Registrar { Scenario {
    "bounce.mp3_by_format", { "bounce", "mp3" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return mp3Bounce (ctx); }, 120000 } };
const ScenarioRegistrar clickRegistrar { Scenario {
    "bounce.metronome_never_prints", { "bounce", "metronome" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return metronomeNeverPrints (ctx); }, 120000 } };
const ScenarioRegistrar faderRegistrar { Scenario {
    "bounce.captures_post_master_fader", { "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return capturesPostMasterFader (ctx); }, 120000 } };
const ScenarioRegistrar stemsRegistrar { Scenario {
    "bounce.stems_rebuild_the_mix", { "bounce", "stems" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return stemsRebuildTheMix (ctx); }, 180000 } };
const ScenarioRegistrar stemsNamedRegistrar { Scenario {
    "bounce.stems_render_what_start_named", { "bounce", "stems" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return stemsRenderWhatStartNamed (ctx); }, 120000 } };
const ScenarioRegistrar timelineRegistrar { Scenario {
    "bounce.renders_land_on_the_timeline", { "bounce", "stems", "pdc" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return rendersLandOnTheTimeline (ctx); }, 180000 } };
const ScenarioRegistrar pdcRegistrar { Scenario {
    "bounce.pdc_trimmed_from_render", { "bounce", "pdc", "vst3" }, Needs::Engine, { "relayout.vst3" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_VST3
        return pdcTrimmedFromRender (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native VST3 host");
       #endif
    }, 120000 } };
const ScenarioRegistrar exportInPlaceRegistrar { Scenario {
    "bounce.export_master_in_place", { "bounce", "mastering", "pdc" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterInPlace (ctx, false); }, 120000 } };
const ScenarioRegistrar exportInPlaceCompRegistrar { Scenario {
    "bounce.export_master_in_place_comp_in", { "bounce", "mastering", "pdc" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterInPlace (ctx, true); }, 120000 } };
const ScenarioRegistrar busAlignRegistrar { Scenario {
    "mix.bus_routed_lands_with_direct", { "mix", "pdc" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return busRoutedLandsWithDirect (ctx); } } };
const ScenarioRegistrar sunsetAlignRegistrar { Scenario {
    "mix.sunset_lands_with_audio", { "mix", "pdc", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return sunsetLandsWithAudio (ctx); } } };
const ScenarioRegistrar sunsetOversamplingRegistrar { Scenario {
    "mix.sunset_oversampling_mid_roll", { "mix", "midi", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return sunsetOversamplingFlipsMidRoll (ctx); }, 120000 } };
const ScenarioRegistrar latencyBridgeRegistrar { Scenario {
    "mix.latency_rise_drops_bridged_note", { "mix", "midi", "clap" }, Needs::Engine, { "latency.clap" },
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult>
    {
       #if DUSKSTUDIO_HAS_NATIVE_CLAP
        return latencyRiseDropsBridgedNote (ctx);
       #else
        (void) ctx;
        return ScenarioResult::skip ("built without the native CLAP host");
       #endif
    }, 120000 } };
const ScenarioRegistrar sunsetLoopSeamRegistrar { Scenario {
    "mix.sunset_loop_seam_resets_once", { "mix", "midi", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return sunsetLoopSeamResetsOnce (ctx); }, 120000 } };
const ScenarioRegistrar sunsetBlockBoundarySeamRegistrar { Scenario {
    "mix.sunset_loop_seam_on_block_boundary", { "mix", "midi", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return sunsetLoopSeamOnBlockBoundary (ctx); }, 120000 } };
const ScenarioRegistrar sunsetLoopOffRegistrar { Scenario {
    "mix.sunset_loop_off_past_the_seam", { "mix", "midi", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return sunsetLoopOffPastTheSeam (ctx); }, 120000 } };
const ScenarioRegistrar cancelRegistrar { Scenario {
    "bounce.cancel_leaves_no_file", { "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return cancelLeavesNoFile (ctx); }, 120000 } };
const ScenarioRegistrar refusedRegistrar { Scenario {
    "bounce.refused_target_left_as_it_was", { "bounce", "stems", "freeze" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return refusedTargetLeftAsItWas (ctx); }, 120000 } };
const ScenarioRegistrar exportRegistrar { Scenario {
    "bounce.export_master_post_limiter", { "bounce", "mastering" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterPostLimiter (ctx); }, 120000 } };
const ScenarioRegistrar monoMonitorRegistrar { Scenario {
    "mastering.mono_monitor_keeps_export_stereo", { "mastering", "phase", "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return masteringMonoMonitor (ctx); }, 240000 } };
const ScenarioRegistrar pendingRestoreRegistrar { Scenario {
    "bounce.pending_sfz_restore_refuses_render", { "bounce", "freeze", "sfz", "issue-900" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return pendingInstrumentRefusesRender (ctx, true); }, 120000 } };
const ScenarioRegistrar pendingReplacementRegistrar { Scenario {
    "bounce.pending_sfz_replacement_refuses_render", { "bounce", "freeze", "sfz", "issue-900" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return pendingInstrumentRefusesRender (ctx, false); }, 120000 } };
const ScenarioRegistrar phaseContinuityRegistrar { Scenario {
    "mastering.phase_tracks_playback_continuity", { "mastering", "phase" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return masteringPhaseContinuity (ctx); } } };
const ScenarioRegistrar exportWav24Registrar { Scenario {
    "bounce.export_master_wav24_session_rate", { "bounce", "mastering" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterWav24 (ctx); }, 120000 } };
const ScenarioRegistrar exportMp3Registrar { Scenario {
    "bounce.export_master_mp3_320", { "bounce", "mastering", "mp3" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterMp3 (ctx); }, 120000 } };
const ScenarioRegistrar exportDitherRegistrar { Scenario {
    "bounce.export_master_cd_dither_is_tpdf", { "bounce", "mastering" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterDither (ctx); }, 120000 } };
const ScenarioRegistrar finalizationRegistrar { Scenario {
    "bounce.native_finalization_failures", { "bounce", "stems", "freeze", "realtime" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return finalizationFailures (ctx); }, 120000 } };
} // namespace
} // namespace duskstudio::scenario
