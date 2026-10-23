#include "GuiScenarioSupport.h"
#include "../WheelScroll.h"
#include "../../foundation/Fs.h"
#include "../../engine/audiofile/FileWriter.h"
#include "../../session/SessionSerializer.h"
#include "../../session/TakeComp.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <vector>

namespace duskstudio::scenario::gui
{
float notch (float n) { return n * wheel::profile().lineNotchDelta; }

void runSteps (ScenarioContext& ctx, std::shared_ptr<std::vector<Step>> steps,
               std::function<void()> onDone)
{
    // The pending timer holds the runner and the runner holds itself weakly, so
    // it lives exactly as long as it has a step left.
    auto runFrom = std::make_shared<std::function<void (std::size_t)>>();
    std::weak_ptr<std::function<void (std::size_t)>> weakRunner = runFrom;

    *runFrom = [&ctx, steps, weakRunner, onDone] (std::size_t index)
    {
        if (ctx.isComplete()) return;

        if (index >= steps->size())
        {
            onDone();
            return;
        }

        auto runner = weakRunner.lock();
        using Clock = std::chrono::steady_clock;
        const auto runThenNext = [&ctx, steps, runner, index]
        {
            (*steps)[index].run();

            const auto next = index + 1;
            const int delay = next < steps->size() ? (*steps)[next].delayMs : 0;
            const auto deadline = Clock::now() + std::chrono::milliseconds (kSettleBoundMs);
            auto settle = std::make_shared<std::function<void()>>();
            std::weak_ptr<std::function<void()>> weakSettle = settle;
            *settle = [&ctx, runner, index, next, delay, deadline, weakSettle]
            {
                if (panelInputSeen())
                {
                    if (runner != nullptr)
                        ctx.later (delay, [runner, next] { (*runner) (next); });
                    return;
                }
                if (Clock::now() >= deadline)
                {
                    ctx.expect (false, "a panel had not drawn the input step " + std::to_string (index)
                                           + " sent it after " + std::to_string (kSettleBoundMs) + " ms");
                    ctx.complete (ctx.verdict());
                    return;
                }
                if (auto again = weakSettle.lock())
                    ctx.later (kUntilPollMs, [again] { (*again)(); });
            };
            (*settle)();
        };

        const auto& step = (*steps)[index];
        if (! step.until)
        {
            runThenNext();
            return;
        }

        // Wall-clock bounds, as in ScenarioContext::waitUntil. A timeout goes
        // through expect so an earlier broken expectation stays the verdict.
        const auto started = Clock::now();
        const auto deadline = started + std::chrono::milliseconds (step.boundMs);
        const auto retryAt = started + std::chrono::milliseconds (kRetryAfterMs);
        auto retried = std::make_shared<bool> (false);
        auto poll = std::make_shared<std::function<void()>>();
        std::weak_ptr<std::function<void()>> weakPoll = poll;
        *poll = [&ctx, steps, index, deadline, retryAt, retried, runThenNext, weakPoll]
        {
            const auto& waiting = (*steps)[index];
            if (waiting.until())
            {
                runThenNext();
                return;
            }
            const auto now = Clock::now();
            if (now >= deadline)
            {
                ctx.expect (false, waiting.timeout.empty() ? "step " + std::to_string (index) + " timed out"
                                                           : waiting.timeout);
                ctx.complete (ctx.verdict());
                return;
            }
            if (waiting.retry && ! *retried && now >= retryAt)
            {
                *retried = true;
                ctx.note ("step " + std::to_string (index) + " was still waiting after "
                          + std::to_string (kRetryAfterMs) + " ms and ran its retry");
                waiting.retry();
            }
            if (auto next = weakPoll.lock())
                ctx.later (kUntilPollMs, [next] { (*next)(); });
        };
        (*poll)();
    };

    ctx.later (steps->empty() ? 0 : steps->front().delayMs, [runFrom] { (*runFrom) (0); });
}

void dismissAlert (GuiHost& host)
{
    if (! host.modalStackEmpty()) host.closeTopModal();
}

GuiHost::Stage guiStage (AudioEngine::Stage stage)
{
    switch (stage)
    {
        case AudioEngine::Stage::Mixing:    return GuiHost::Stage::Mixing;
        case AudioEngine::Stage::Aux:       return GuiHost::Stage::Aux;
        case AudioEngine::Stage::Mastering: return GuiHost::Stage::Mastering;
        case AudioEngine::Stage::Recording: break;
    }
    return GuiHost::Stage::Recording;
}

void keepStage (GuiHost& host, ScenarioContext& ctx)
{
    ctx.cleanup ([&host, stage = guiStage (ctx.engine().getStage())] { host.switchToStage (stage); });
}

void drainModals (GuiHost& host)
{
    for (int guard = 0; guard < 32 && ! host.modalStackEmpty(); ++guard) host.closeTopModal();
}

void reopenSavedSession (GuiHost& host, const std::filesystem::path& sessionJson)
{
    host.openSession (sessionJson);
    host.answerRecovery (GuiHost::Recovery::LoadSaved);
}

std::string fileBytes (const std::filesystem::path& path)
{
    return dusk::fs::loadFileAsString (path);
}

std::string commandChord (char letter)
{
   #if defined (__APPLE__)
    return std::string ("command + ") + letter;
   #else
    return std::string ("ctrl + ") + letter;
   #endif
}

void typeReplacing (GuiHost& host, const std::string& text)
{
    host.pressPeerKey (commandChord ('A'), 'a');
    for (const char ch : text)
        host.pressPeerKey (ch == ' ' ? "Space" : std::string (1, ch), ch);
}

void pushFileMenuSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps,
                        const std::string& item)
{
    steps.push_back ({ 100, [&host, &ctx] { ctx.expect (host.clickFileMenu(), "the File menu is unavailable"); },
                       [&host] { return host.modalStackEmpty(); },
                       "a modal was still up when the File menu was wanted" });
    // The second click goes out only while no menu is up, so it lands on the
    // menu bar exactly where the first one did.
    steps.push_back ({ 0, [&host, &ctx, item]
                       { ctx.expect (host.clickContextMenuItem (item), "the File menu has no " + item); },
                       [&host] { return ! host.contextMenuItems().empty(); },
                       "the File menu did not open",
                       [&host] { host.clickFileMenu(); } });
}

void pushSaveAsSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps,
                      const std::filesystem::path& target, std::function<void()> beforeSave)
{
    const auto browserUp = [&host] { return host.modalText().rfind ("Save session as...", 0) == 0; };
    pushFileMenuSteps (host, ctx, steps, "Save as...");
    steps.push_back ({ 0, [&host, &ctx, target, beforeSave]
    {
        if (! ctx.expect (host.focusFileName(), "the Save As browser has no name field")) return;
        typeReplacing (host, target.string());
        if (beforeSave) beforeSave();
        ctx.expect (host.clickModalButton ("Save"), "the Save As browser has no Save button");
    },
    browserUp, "File > Save as... did not bring up the Save As browser",
    [&host]
    {
        if (! host.contextMenuItems().empty()) host.clickContextMenuItem ("Save as...");
    } });
    // No second press: the Save button acts in the press itself, and one that
    // went out after a failed name entry would save under the default name.
    steps.push_back ({ 0, [] {}, [browserUp] { return ! browserUp(); },
                       "the Save As browser did not take the name it was given" });
}

void pushCancelledMixdownCloseSteps (GuiHost& host, std::vector<Step>& steps, bool closeOptional)
{
    steps.push_back ({ 0, [] {}, [&host] { return ! host.mixdownRunning(); }, "the mixdown did not cancel" });
    steps.push_back ({ 0, [] {},
                       [&host, closeOptional]
                       { return (closeOptional && host.modalStackEmpty()) || host.clickModalButton ("Close"); },
                       "the cancelled mixdown did not offer Close" });
    steps.push_back ({ 0, [] {}, [&host] { return host.modalStackEmpty(); },
                       "the cancelled mixdown left its modal open" });
}

std::string keyCodeDescription (char glyph)
{
    constexpr char digits[] = "0123456789abcdef";
    return std::string ("#") + digits[(glyph >> 4) & 0xf] + digits[glyph & 0xf];
}

char controlCharacter (char letter) { return (char) (letter - 'a' + 1); }

StripHandle* readyStrip (GuiHost& host, ScenarioContext& ctx)
{
    keepStage (host, ctx);
    host.switchToStage (GuiHost::Stage::Mixing);
    auto* strip = host.strip (kStripIndex);
    if (strip == nullptr) return nullptr;
    strip->closeEditor();
    strip->unloadNativePlugins();
    strip->refreshInsertButton();
    return strip;
}

std::optional<ScenarioResult> beginTakeCase (GuiHost& host, ScenarioContext& ctx)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    if (! engine.getTransport().isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save session");
    ctx.cleanup ([&host, &engine, &session, originalDir, restore]
    {
        drainModals (host);
        host.closeAudioEditor();
        engine.clearTakeAudition();
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
    });
    auto& track = session.track (0);
    track.frozen.store (false);
    track.regions.clear();
    track.takes.clear();
    engine.getUndoManager().clearUndoHistory();
    return std::nullopt;
}

std::optional<TakeId> addLevelTake (ScenarioContext& ctx, Track& track, const std::string& name,
                                    std::int64_t start, std::int64_t length, float level)
{
    AudioTake take;
    take.id = ctx.session().allocateTakeId();
    const auto path = ctx.tempDir() / ("take-" + std::to_string (take.id) + ".wav");
    dusk::audio::WriteSpec spec;
    spec.sampleRate = 48000;
    spec.numChannels = 1;
    auto writer = dusk::audio::FileWriter::create (path, spec);
    const std::vector<float> signal ((std::size_t) length, level);
    const float* channels[] = { signal.data() };
    if (! writer || ! writer->write (channels, 1, length) || ! writer->flush())
        return std::nullopt;
    take.name = name;
    take.file = decltype (take.file) (path.string());
    take.timelineStart = start;
    take.lengthInSamples = length;
    track.takes.push_back (take);
    return take.id;
}

std::optional<std::vector<TakeId>> addLevelTakes (ScenarioContext& ctx, Track& track, int count)
{
    std::vector<TakeId> ids;
    for (int pass = 0; pass < count; ++pass)
    {
        const auto id = addLevelTake (ctx, track, "Take " + std::to_string (pass + 1), 0, kTakeCaseLength,
                                      0.2f + 0.2f * (float) pass);
        if (! id) return std::nullopt;
        ids.push_back (*id);
    }
    return ids;
}

std::string takeControl (const char* kind, TakeId take)
{
    return std::string ("take-") + kind + ":" + std::to_string (take);
}

bool sameRegions (const std::vector<AudioRegion>& a, const std::vector<AudioRegion>& b)
{
    return std::equal (a.begin(), a.end(), b.begin(), b.end(), [] (const AudioRegion& x, const AudioRegion& y)
    {
        return x.takeId == y.takeId && x.timelineStart == y.timelineStart
            && x.lengthInSamples == y.lengthInSamples && x.sourceOffset == y.sourceOffset;
    });
}

std::string undoDescription (AudioEngine& engine)
{
    auto& undo = engine.getUndoManager();
    return undo.canUndo() ? undo.getUndoDescription().toStdString() : std::string();
}

std::optional<TakeId> landTake (ScenarioContext& ctx, Track& track, std::int64_t start, std::int64_t length,
                                bool announced)
{
    auto& engine = ctx.engine();
    if (announced && engine.onBeforeRecordCommit)
        engine.onBeforeRecordCommit (1u);
    const auto id = addLevelTake (ctx, track, "Take 2", start, length, kShortTakeLevel);
    if (id)
        promoteTakeRange (ctx.session(), track, track.takes.back(), start, start + length);
    return id;
}

bool nearly (float a, float b) { return std::abs (a - b) < 1.0e-4f; }

float savedFaderOf (const std::filesystem::path& sessionJson)
{
    Session probe;
    if (! SessionSerializer::load (probe, sessionJson)) return 1000.0f;
    return probe.track (0).strip.faderDb.load (std::memory_order_relaxed);
}

bool expectBrowserInHome (GuiHost& host, ScenarioContext& ctx, const std::string& what)
{
    const auto isInside = [] (const std::filesystem::path& folder, const std::filesystem::path& root)
    {
        const auto relative = folder.lexically_relative (root);
        return ! root.empty() && ! relative.empty() && *relative.begin() != "..";
    };
   #if defined (_WIN32)
    const wchar_t* variable = ::_wgetenv (L"USERPROFILE");
    const auto home = std::filesystem::path (variable != nullptr ? variable : L"").lexically_normal();
   #else
    const char* variable = std::getenv ("HOME");
    const auto home = std::filesystem::u8path (variable != nullptr ? variable : "").lexically_normal();
   #endif
    const auto folder = host.fileBrowserFolder().lexically_normal();
    // A redirected Documents folder (another drive on Windows) is the user's
    // real one, and the browsers that start there are right to.
    const auto documents = dusk::fs::userDocumentsDir().lexically_normal();
    const bool inside = ! folder.empty()
                        && (isInside (folder, home)
                            || (! isInside (documents, home) && isInside (folder, documents)));
    return ctx.expect (inside, what + " opened at '" + folder.u8string() + "', outside HOME " + home.u8string());
}

bool writeRampFixture (const std::filesystem::path& path, double sampleRate, std::int64_t frames)
{
    auto writer = dusk::audio::FileWriter::create (path, { sampleRate, 1, 24 });
    if (writer == nullptr) return false;
    std::vector<float> ramp (static_cast<std::size_t> (frames));
    for (std::size_t i = 0; i < ramp.size(); ++i)
        ramp[i] = -0.5f + static_cast<float> (i) / static_cast<float> (ramp.size());
    const float* channels[] { ramp.data() };
    return writer->write (channels, 1, frames) && writer->flush();
}

std::optional<ScenarioResult> seedTapeRegion (GuiHost& host, ScenarioContext& ctx, AudioRegion& region)
{
    auto& engine = ctx.engine();
    auto& session = ctx.session();
    auto& transport = engine.getTransport();
    if (! transport.isStopped() || ! host.modalStackEmpty())
        return ScenarioResult::skip ("requires stopped transport and no modal");
    const auto originalDir = currentSessionDirectory (session);
    const auto restore = ctx.tempDir() / "restore.json";
    if (! SessionSerializer::save (session, restore)) return ScenarioResult::fail ("could not save the initial session");
    const bool expanded = host.timelineViewMatches (true);
    ctx.cleanup (host.preserveKeyboardFocus());
    ctx.cleanup ([&host, &ctx, &session, &transport, originalDir, restore, expanded, view = host.tapeView(),
                  loopStart = transport.getLoopStart(), loopEnd = transport.getLoopEnd(),
                  looping = transport.isLoopEnabled(), playhead = transport.getPlayhead()]
    {
        drainModals (host);
        host.openSession (restore);
        applySessionDirectory (session, originalDir);
        transport.setLoopRange (loopStart, loopEnd);
        transport.setLoopEnabled (looping);
        transport.locate (playhead);
        ctx.engine().getUndoManager().clearUndoHistory();
        if (! host.timelineViewMatches (expanded)) host.pressKey ("T", 't');
        host.restoreTapeView (view);
    });
    // Its own folder: autosave writes there, and an autosave newer than
    // restore.json would turn the cleanup's reload into a recovery prompt.
    const auto folder = ctx.tempDir() / "session";
    std::error_code error;
    if (! std::filesystem::create_directories (folder, error) && error)
        return ScenarioResult::fail ("could not create the session folder");
    applySessionDirectory (session, folder);
    session.snapToGrid = false;
    session.getMarkers().clear();
    auto& track = session.track (0);
    track.mode.store ((int) Track::Mode::Mono);
    track.frozen.store (false);
    const double rate = engine.getCurrentSampleRate();
    const auto beat = static_cast<std::int64_t> (std::llround (rate / 2.0));
    region = {};
    region.timelineStart = beat * 4;
    region.sourceOffset = beat;
    region.lengthInSamples = beat * 4;
    const auto source = folder / "Ramp.wav";
    if (! writeRampFixture (source, rate, region.lengthInSamples * 2))
        return ScenarioResult::fail ("could not write the region fixture");
    region.file = decltype (region.file) (source.u8string().c_str());
    track.regions = { region };
    engine.getUndoManager().clearUndoHistory();
    if (! expanded) host.pressKey ("T", 't');
    host.pressKey ("0", '0');
    return std::nullopt;
}

bool writeBytes (const std::filesystem::path& path, std::size_t size, char fill)
{
    std::ofstream out (path, std::ios::binary);
    const std::string bytes (size, fill);
    out.write (bytes.data(), (std::streamsize) bytes.size());
    return static_cast<bool> (out);
}

std::string joinedLines (const std::vector<std::string>& lines)
{
    std::string text;
    for (const auto& line : lines) text += (text.empty() ? "" : " | ") + line;
    return text;
}
} // namespace duskstudio::scenario::gui
