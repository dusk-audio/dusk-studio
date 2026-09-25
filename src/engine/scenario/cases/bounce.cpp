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
        const auto size = std::filesystem::file_size (out, fsError);
        ctx.expect (! fsError && size > 4096, "the MP3 bounce is missing or nearly empty");
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

// --------------------------------------------------------------- alignment

// Track 1 plays to the master and sends to aux 1, track 2 plays through bus 1,
// and track 3 is muted. The stems come out aligned with each other and with
// the mix, and together rebuild it; the muted track's stem is silent.
std::optional<ScenarioResult> stemsRebuildTheMix (ScenarioContext& ctx)
{
    auto& session = ctx.session();
    constexpr int kLength = 24000;
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

// An impulse on a direct track and one on a bus-routed track, rendered as a
// mix and as stems, all land where they sit on the timeline.
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

    Render mixRender;
    mixRender.tailSeconds = 0.2;
    Render stemRender = mixRender;
    stemRender.mode = BounceEngine::Mode::Stems;
    renderThen (ctx, mixPath, mixRender, [&ctx, mixPath, base, targets, stemRender, landed] (bool ok, const std::string& error)
    {
        const auto mix = readBack (mixPath);
        if (! ctx.expect (ok && mix.has_value(), "the mix bounce failed: " + error))
        {
            ctx.complete (ctx.verdict());
            return;
        }
        landed (mix->left, 0, kAt + 6000, kAt, "mix, direct track");
        landed (mix->left, kAt + 6000, (std::int64_t) mix->left.size(), kAt + 12000, "mix, bus-routed track");

        renderThen (ctx, base, stemRender, [&ctx, targets, landed] (bool stemsOk, const std::string& stemsError)
        {
            if (ctx.expect (stemsOk, "the stem bounce failed: " + stemsError))
                for (const auto& target : targets)
                    if (const auto stem = readBack (pathOf (target.file)))
                    {
                        const bool routed = target.kind == BounceEngine::StemTarget::Kind::Bus
                                         || (target.kind == BounceEngine::StemTarget::Kind::Track
                                             && target.index == 1);
                        landed (stem->left, 0, (std::int64_t) stem->left.size(),
                                routed ? kAt + 12000 : kAt,
                                pathOf (target.file).filename().u8string());
                    }
            ctx.complete (ctx.verdict());
        });
    });
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
            if (version != 3 || layer != 1 || rate != 14 || srIndex != 1)
                ++wrong;
            ++frames;
            at += (std::size_t) (960 + padding);
        }
        ctx.note (std::to_string (frames) + " frames, " + std::to_string (wrong) + " not 320 kbps MPEG-1 Layer III at 48 kHz, "
                  + std::to_string (bytes.size() - std::min (at, bytes.size())) + " trailing bytes");
        ctx.expect (frames >= minFrames, "the MP3 export has " + std::to_string (frames) + " frames, short of the "
                                             + std::to_string (minFrames) + " the mix and its tail need");
        ctx.expect (wrong == 0, "an MP3 frame is not 320 kbps MPEG-1 Layer III at 48 kHz");
    });
   #else
    (void) ctx;
    return ScenarioResult::skip ("built without the MP3 encoder");
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
const ScenarioRegistrar stemsRegistrar { Scenario {
    "bounce.stems_rebuild_the_mix", { "bounce", "stems" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return stemsRebuildTheMix (ctx); }, 180000 } };
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
const ScenarioRegistrar cancelRegistrar { Scenario {
    "bounce.cancel_leaves_no_file", { "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return cancelLeavesNoFile (ctx); }, 120000 } };
const ScenarioRegistrar exportRegistrar { Scenario {
    "bounce.export_master_post_limiter", { "bounce", "mastering" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterPostLimiter (ctx); }, 120000 } };
const ScenarioRegistrar exportWav24Registrar { Scenario {
    "bounce.export_master_wav24_session_rate", { "bounce", "mastering" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterWav24 (ctx); }, 120000 } };
const ScenarioRegistrar exportMp3Registrar { Scenario {
    "bounce.export_master_mp3_320", { "bounce", "mastering", "mp3" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterMp3 (ctx); }, 120000 } };
const ScenarioRegistrar exportDitherRegistrar { Scenario {
    "bounce.export_master_cd_dither_is_tpdf", { "bounce", "mastering" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return exportMasterDither (ctx); }, 120000 } };
} // namespace
} // namespace duskstudio::scenario
