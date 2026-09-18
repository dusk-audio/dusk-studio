#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../BounceEngine.h"
#include "../../MasteringPlayer.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../../dsp/ChannelStrip.h"
#include "../../../session/Session.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <type_traits>
#include <vector>

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
                                        const Render& render)
{
    auto run = std::make_shared<BounceRun>();
    run->bounce = std::make_unique<BounceEngine> (ctx.engine(), ctx.session());
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
        ctx.expect (std::filesystem::file_size (out, fsError) > 4096, "the MP3 bounce is nearly empty");
        ctx.complete (ctx.verdict());
    });
    return std::nullopt;
   #else
    (void) ctx;
    return ScenarioResult::skip ("built without the MP3 encoder");
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

const ScenarioRegistrar mixRegistrar { Scenario {
    "bounce.master_mix_file", { "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return masterMixFile (ctx); }, 120000 } };
const ScenarioRegistrar mp3Registrar { Scenario {
    "bounce.mp3_by_format", { "bounce", "mp3" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return mp3Bounce (ctx); }, 120000 } };
const ScenarioRegistrar clickRegistrar { Scenario {
    "bounce.metronome_never_prints", { "bounce", "metronome" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return metronomeNeverPrints (ctx); }, 120000 } };
const ScenarioRegistrar faderRegistrar { Scenario {
    "bounce.captures_post_master_fader", { "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return capturesPostMasterFader (ctx); }, 120000 } };
const ScenarioRegistrar cancelRegistrar { Scenario {
    "bounce.cancel_leaves_no_file", { "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return cancelLeavesNoFile (ctx); }, 120000 } };
const ScenarioRegistrar exportRegistrar { Scenario {
    "bounce.export_master_post_limiter", { "bounce", "mastering" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterPostLimiter (ctx); }, 120000 } };
} // namespace
} // namespace duskstudio::scenario
