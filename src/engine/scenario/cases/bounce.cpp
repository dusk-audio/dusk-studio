#include "../RecordingMidiBackend.h"
#include "../Scenario.h"
#include "../ScenarioContext.h"
#include "../../AudioEngine.h"
#include "../../BounceEngine.h"
#include "../../MasteringPlayer.h"
#include "../../builtin/BuiltinRegistry.h"
#include "../../audiofile/FileReader.h"
#include "../../audiofile/FileWriter.h"
#include "../../../dsp/ChannelStrip.h"
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

// Live, a Sunset note plays where it sits on the timeline, level with an
// impulse an audio track holds at the same sample, at every setting of
// Sunset's own Oversampling switch - straight through, and on the pass after
// a loop wraps, where the note's scheduling wraps that latency ahead of the
// audio.
ScenarioResult sunsetLandsWithAudio (ScenarioContext& ctx)
{
    constexpr const char* kSunset = "dusk.builtin.synth";
    if (builtin::findUnit (kSunset) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    static constexpr std::int64_t kAt = 24000;
    // The loop ends 16 samples into a block, so the note, clear of the audio
    // seam's declick 128 samples past the loop start, is scheduled in the span
    // after the seam of the very block that wraps.
    static constexpr std::int64_t kLoopStart = kAt - 128;
    static constexpr std::int64_t kLoopEnd = kLoopStart + 93 * ScenarioContext::kBlockSize + 16;
    constexpr int kAudioTrack = 0, kSynthTrack = 1;
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
    constexpr const char* kSunset = "dusk.builtin.synth";
    if (builtin::findUnit (kSunset) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    constexpr int kTrack = 0;
    constexpr int kBlock = ScenarioContext::kBlockSize;
    // A flip lands inside the block it is pumped with, so the schedule moves
    // from the next one: note A ends 5 samples into kUpBlock, the first block
    // scheduled at 2x, and note B starts 5 samples into kDownBlock, the first
    // back at 1x.
    constexpr std::int64_t kUpBlock = 40;
    constexpr std::int64_t kDownBlock = 1000;
    constexpr int kIntoBlock = 5;
    constexpr int kNoteA = 60, kNoteB = 64;
    constexpr float kQuietDb = -60.0f;

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
    constexpr std::int64_t kNoteTicks = kMidiTicksPerQuarter / 4;
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

    // The bank hands blocks to its pump thread, so the count settles a little
    // after the last block rather than inside it.
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
                   [&ctx, recorder]
                   {
                       std::array<int, 128> ons {}, offs {};
                       for (const auto& m : recorder->captured())
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
                           ctx.expect (on == 1, name + " was handed " + std::to_string (on)
                                                    + " note-ons, not one");
                           ctx.expect (off == 1, name + " was handed " + std::to_string (off)
                                                     + " note-offs, not one");
                       }
                       ctx.expect (! recorder->hasDropped(), "the MIDI recorder ran out of room");
                       ctx.complete (ctx.verdict());
                   },
                   "the MIDI output recorder never stopped receiving events");
    return std::nullopt;
}

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
    constexpr const char* kSunset = "dusk.builtin.synth";
    if (builtin::findUnit (kSunset) == nullptr)
        return ScenarioResult::skip ("this build has no Sunset");

    constexpr int kBlock = ScenarioContext::kBlockSize;
    constexpr int kLatentTrack = 0, kPlainTrack = 1, kNumTracks = 2;
    // Whole blocks long and rolled from a block boundary, so the end lands
    // this far into a block on every pass.
    constexpr int kSeamIntoBlock = 5;
    constexpr std::int64_t kLoopEnd = 40 * kBlock + kSeamIntoBlock;
    constexpr std::int64_t kLoopStart = kLoopEnd - 32 * kBlock;
    // The block whose window at 2x starts past the loop end, and the one
    // before it, whose window crosses the end.
    constexpr std::int64_t kSeamBlock = kLoopEnd - kSeamIntoBlock;
    constexpr std::int64_t kCrossingBlock = kSeamBlock - kBlock;
    // Mid-pass the loop end moves in to just past the block about to roll,
    // which keeps the loop whole blocks long.
    constexpr std::int64_t kEditBlock = kLoopStart + (kBlock - kSeamIntoBlock) + 24 * kBlock;
    constexpr int kHeld = 60, kOnStart = 64;

    auto& session = ctx.session();
    auto& engine = ctx.engine();
    auto& transport = engine.getTransport();

    auto owned = std::make_unique<RecordingMidiBackend> (kNumTracks);
    auto* const recorder = owned.get();
    engine.installMidiOutputBackend (std::move (owned));
    if (engine.getMidiOutputDevices().size() < (std::size_t) kNumTracks
        || ! engine.ensureMidiOutputOpen (kLatentTrack) || ! engine.ensureMidiOutputOpen (kPlainTrack))
        return ScenarioResult::fail ("the recording MIDI outputs would not open");

    ctx.cleanup ([&engine, &session, &transport]
    {
        engine.stop();
        transport.setLoopEnabled (false);
        for (int t = 0; t < kNumTracks; ++t)
        {
            session.track (t).midiOutputIndex.store (-1, std::memory_order_relaxed);
            engine.getChannelStrip (t).unloadBuiltin();
        }
    });

    const float bpm = session.tempoBpm.load (std::memory_order_relaxed);
    constexpr std::int64_t kNoteTicks = kMidiTicksPerQuarter / 8;
    const auto noteRegion = [&] (int pitch, std::int64_t onAt)
    {
        MidiRegion region;
        region.timelineStart = onAt;
        region.lengthInTicks = 2 * kNoteTicks;
        region.lengthInSamples = ticksToSamples (region.lengthInTicks, kRate, bpm);
        region.notes.push_back ({ 1, pitch, 100, 0, kNoteTicks });
        return region;
    };
    for (int t = 0; t < kNumTracks; ++t)
    {
        auto& track = session.track (t);
        track.mode.store ((int) Track::Mode::Midi, std::memory_order_relaxed);
        auto& strip = engine.getChannelStrip (t);
        std::string error;
        if (! ctx.expect (strip.loadBuiltin (kSunset, error), "could not load Sunset: " + error))
            return ctx.verdict();
        auto& slot = strip.getBuiltinSlot();
        const int oversampling = oversamplingParam (slot);
        if (! ctx.expect (oversampling >= 0, "Sunset has no Oversampling parameter"))
            return ctx.verdict();
        slot.setParamValue (oversampling, t == kLatentTrack ? 1.0f : 0.0f);
        auto regions = std::make_unique<std::vector<MidiRegion>>();
        regions->push_back (noteRegion (kHeld, kLoopStart - 1000));
        regions->push_back (noteRegion (kOnStart, kLoopStart));
        track.midiRegions.publish (std::move (regions));
        track.midiOutputIndex.store (t, std::memory_order_relaxed);
    }
    ctx.pump (1);
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

    // The bank hands blocks to its pump thread, so the count settles a little
    // after the last block rather than inside it.
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
                   [&ctx, recorder, wraps]
                   {
                       const auto messages = recorder->captured();
                       const auto isNoteOn = [] (const RecordingMidiBackend::Message& m)
                       {
                           return m.numBytes == 3 && (m.bytes[0] & 0xF0) == 0x90 && m.bytes[2] > 0;
                       };
                       for (int t = 0; t < kNumTracks; ++t)
                       {
                           // The roll starts and Stop ends with a reset of their
                           // own, outside the first and last note-on.
                           std::size_t firstOn = messages.size(), lastOn = 0;
                           for (std::size_t i = 0; i < messages.size(); ++i)
                               if (messages[i].port == t && isNoteOn (messages[i]))
                               {
                                   firstOn = std::min (firstOn, i);
                                   lastOn = i;
                               }
                           int resets = 0;
                           std::string offsets;
                           std::array<int, 128> ons {};
                           for (std::size_t i = 0; i < messages.size(); ++i)
                           {
                               const auto& m = messages[i];
                               if (m.port != t)
                                   continue;
                               if (isNoteOn (m))
                                   ++ons[(std::size_t) (m.bytes[1] & 0x7F)];
                               if (m.numBytes == 3 && m.bytes[0] == 0xB0 && m.bytes[1] == 120
                                   && i > firstOn && i < lastOn)
                               {
                                   ++resets;
                                   offsets += " " + std::to_string (m.sampleOffset);
                               }
                           }
                           const std::string name = t == kLatentTrack ? "at 2x" : "at 1x";
                           const int held = ons[(std::size_t) kHeld], onStart = ons[(std::size_t) kOnStart];
                           ctx.note (name + ": " + std::to_string (resets) + " resets over "
                                     + std::to_string (wraps) + " seams, at block offsets" + offsets
                                     + "; the held note attacked " + std::to_string (held)
                                     + " times, the note on the loop start " + std::to_string (onStart));
                           ctx.expect (resets == wraps,
                                       name + ", " + std::to_string (wraps) + " loop seams reset Sunset "
                                           + std::to_string (resets) + " times");
                           ctx.expect (held == wraps + 1,
                                       name + ", the note held into the loop start was attacked "
                                           + std::to_string (held) + " times, not once before the loop and once a pass");
                           ctx.expect (onStart == wraps + 1,
                                       name + ", the note on the loop start was attacked "
                                           + std::to_string (onStart) + " times, not once before the loop and once a pass");
                       }
                       ctx.expect (! recorder->hasDropped(), "the MIDI recorder ran out of room");
                       ctx.complete (ctx.verdict());
                   },
                   "the MIDI output recorder never stopped receiving events");
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
const ScenarioRegistrar sunsetAlignRegistrar { Scenario {
    "mix.sunset_lands_with_audio", { "mix", "pdc", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) -> std::optional<ScenarioResult> { return sunsetLandsWithAudio (ctx); } } };
const ScenarioRegistrar sunsetOversamplingRegistrar { Scenario {
    "mix.sunset_oversampling_mid_roll", { "mix", "midi", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return sunsetOversamplingFlipsMidRoll (ctx); }, 120000 } };
const ScenarioRegistrar sunsetLoopSeamRegistrar { Scenario {
    "mix.sunset_loop_seam_resets_once", { "mix", "midi", "builtin" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return sunsetLoopSeamResetsOnce (ctx); }, 120000 } };
const ScenarioRegistrar cancelRegistrar { Scenario {
    "bounce.cancel_leaves_no_file", { "bounce" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return cancelLeavesNoFile (ctx); }, 120000 } };
const ScenarioRegistrar refusedRegistrar { Scenario {
    "bounce.refused_target_left_as_it_was", { "bounce", "stems", "freeze" }, Needs::Engine, {},
    [] (ScenarioContext& ctx) { return refusedTargetLeftAsItWas (ctx); }, 120000 } };
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
