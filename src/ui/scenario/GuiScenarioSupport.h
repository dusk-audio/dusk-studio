#pragma once

#include "../GuiHost.h"
#include "../../engine/AudioEngine.h"
#include "../../engine/scenario/Scenario.h"
#include "../../engine/scenario/ScenarioContext.h"
#include "../../session/Session.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

// The scenario suite's window-driven cases: what the release checklist used to
// check by opening editors by hand. Each drives the live window through GuiHost
// and puts back what it changed, so the next case starts on a clean strip.
namespace duskstudio::scenario::gui
{
constexpr int kStripIndex = 0;
constexpr int kAuxLane    = 0;
constexpr int kAuxSlot    = 0;

// A delay before each step, run from the message loop - the shape the capture
// harness uses, and the only one that lets a plug-in editor actually come up
// between two steps.
//
// A step that depends on UI state arriving asynchronously names it in `until`:
// after the delay the step polls it and runs once it holds, so a loaded host
// only makes the case slower. The bound catches a hang, not a slow machine, and
// fails the case with `timeout`. `retry`, where set, is sent once if the state
// is still missing after kRetryAfterMs; only for a click the window can lose
// and a second press cannot change the verdict. `boundMs` shortens the bound for
// a case whose budget cannot hold the default.
//
// The delay of the step after one that sent a native panel input starts once the
// panel has drawn that input, so a slow renderer cannot leave a drag half
// delivered when the next step reads its result.
constexpr int kUntilBoundMs = 15000;

struct Step
{
    int delayMs;
    std::function<void()> run;
    std::function<bool()> until {};
    std::string timeout {};
    std::function<void()> retry {};
    int boundMs = kUntilBoundMs;
};

// A wheel of n notches as this platform's window peer reports it.
float notch (float n);

// Half the shortest window case's budget, so a panel that stops drawing is what the
// case reports rather than its watchdog.
constexpr int kSettleBoundMs = 5000;
constexpr int kUntilPollMs  = 10;
constexpr int kRetryAfterMs = 3000;

void runSteps (ScenarioContext& ctx, std::shared_ptr<std::vector<Step>> steps,
               std::function<void()> onDone);

// A failed open is a legitimate outcome for a fixture that ships no editor, and
// it leaves the alert that says so on screen. Take it down so the next step sees
// the window the user would.
void dismissAlert (GuiHost& host);

GuiHost::Stage guiStage (AudioEngine::Stage stage);

// Puts the stage back to whatever the case found, however the case ends. A
// stage left on Mixing is the window the NEXT case opens on, so every case
// that switches stages registers this before it does.
void keepStage (GuiHost& host, ScenarioContext& ctx);

void drainModals (GuiHost& host);

// Reopening a session that a case saved into the directory it also made the
// session directory: the configured autosave heartbeat writes session.json.autosave
// beside it while the case runs, and an autosave newer than the file being
// opened raises the recovery prompt. Take the saved file - it is the state the
// case wrote and means to go back to. A no-op when no prompt came up.
void reopenSavedSession (GuiHost& host, const std::filesystem::path& sessionJson);

std::string fileBytes (const std::filesystem::path& path);

std::string commandChord (char letter);

// Replaces whatever the focused text field holds, a key at a time.
void typeReplacing (GuiHost& host, const std::string& text);

// Opens the File menu once nothing else is up and picks item from it.
void pushFileMenuSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps,
                        const std::string& item);

// File > Save as... into target, typed into the browser the way a user would.
// The save runs inside the browser's Save press, so the steps end once the
// browser has gone: whatever the save did is done by then. beforeSave, if set,
// runs just before that press.
void pushSaveAsSteps (GuiHost& host, ScenarioContext& ctx, std::vector<Step>& steps,
                      const std::filesystem::path& target, std::function<void()> beforeSave = {});

// A cancelled mixdown stops its worker, and the dialog swaps Cancel for Close on
// its next timer tick. Close is pressed as soon as it shows. Only a case that
// leaves open whether the dialog stays up passes closeOptional.
void pushCancelledMixdownCloseSteps (GuiHost& host, std::vector<Step>& steps, bool closeOptional);

// A key press as the display server hands it over. X11 fills JUCE's key code
// with the XLookupString glyph, so an unmodified letter arrives lowercase and a
// Cmd/Ctrl chord arrives as the lowercase keysym with a control character for
// its text. Naming the code in hex is the only way for a case to press what a
// Linux keyboard actually sends rather than the tidier form a description
// string would build.
std::string keyCodeDescription (char glyph);

char controlCharacter (char letter);

StripHandle* readyStrip (GuiHost& host, ScenarioContext& ctx);

enum class Format { Clap, Lv2, Vst3 };

constexpr std::int64_t kTakeCaseLength = 96000;
constexpr float kShortTakeLevel = 0.75f;
constexpr float kWholeTakeLevel = 0.5f;

// Stopped with no modal, the first track holding no region or take and the undo
// history empty. The session comes back and any audition ends with the case.
std::optional<ScenarioResult> beginTakeCase (GuiHost& host, ScenarioContext& ctx);

// A mono take holding one steady level, so which take a track plays reads off its samples.
std::optional<TakeId> addLevelTake (ScenarioContext& ctx, Track& track, const std::string& name,
                                    std::int64_t start, std::int64_t length, float level);

std::optional<std::vector<TakeId>> addLevelTakes (ScenarioContext& ctx, Track& track, int count);

std::string takeControl (const char* kind, TakeId take);

bool sameRegions (const std::vector<AudioRegion>& a, const std::vector<AudioRegion>& b);

std::string undoDescription (AudioEngine& engine);

// Puts a new take on the track the way a recording's stop does: the notice the shell
// passes to the editor when `announced`, then the take carved into the regions under it.
std::optional<TakeId> landTake (ScenarioContext& ctx, Track& track, std::int64_t start, std::int64_t length,
                                bool announced);

bool nearly (float a, float b);

// Track 1's fader as a session file holds it; a large sentinel when the file
// will not load.
float savedFaderOf (const std::filesystem::path& sessionJson);

// A run's HOME is its sandbox. A browser that starts outside it lists the
// user's own folders, and on macOS a privacy prompt for them stalls the run.
// The home is read from the environment rather than from dusk::fs, so a lookup
// that ignores the environment shows up as an escape instead of moving the
// yardstick with it.
bool expectBrowserInHome (GuiHost& host, ScenarioContext& ctx, const std::string& what);

// A rising mono ramp, so a reversed copy reads back in the opposite order.
bool writeRampFixture (const std::filesystem::path& path, double sampleRate, std::int64_t frames);

// The tape-strip region cases start alike: stopped transport, the timeline
// shown and fitted, snap off, no markers, a session folder under the case's
// temp dir, and track 0 holding one mono region over a ramp twice its length.
// Cleanup reloads the session and puts the view back as they were.
std::optional<ScenarioResult> seedTapeRegion (GuiHost& host, ScenarioContext& ctx, AudioRegion& region);

bool writeBytes (const std::filesystem::path& path, std::size_t size, char fill);

std::string joinedLines (const std::vector<std::string>& lines);
} // namespace duskstudio::scenario::gui
