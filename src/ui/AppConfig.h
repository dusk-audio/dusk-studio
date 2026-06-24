#pragma once

#include <juce_data_structures/juce_data_structures.h>

namespace duskstudio::appconfig
{
// Per-machine preferences. Backed by a juce::PropertiesFile at
//   <userApplicationDataDirectory>/Dusk Studio/app-config.properties
// - separate store from window-state.txt (geometry) and recent.txt
// (recent sessions list) so each file has a single concern.
//
// All accessors are message-thread only. The PropertiesFile has its own
// internal locking, but values returned here aren't atomic on their own,
// so don't poll from the audio thread.

// User-controlled UI scale multiplier on top of JUCE's per-display DPI.
// Default 1.0 (no extra zoom). Clamped to [0.5, 2.0] on write - the
// outer rails of "still readable" on either end.
constexpr float kUiScaleMin     = 0.5f;
constexpr float kUiScaleMax     = 2.0f;
constexpr float kUiScaleDefault = 1.0f;

float getUiScaleOverride();
void  setUiScaleOverride (float scale);

// Scan installed plugin formats on every app launch (synchronous). When
// false (default) the cached KnownPluginList is used as-is and the user
// runs scans manually from the plugin picker's "Scan plugins" button.
// Persisted per-machine so the choice survives session changes.
bool getScanPluginsOnStartup();
void setScanPluginsOnStartup (bool scan);

// Expand the tape TIMELINE strip by default on app launch. When false
// (default) the strip starts collapsed so the channel strips get full
// vertical room; user toggles via TapeStrip's TIMELINE button. Persisted
// per-machine.
bool getTapeStripExpandedDefault();
void setTapeStripExpandedDefault (bool expanded);

// Follow the playhead by default on app launch: when true, the timeline
// (and the audio / MIDI editors) start with Chase engaged so the view
// scrolls to keep the playhead in sight during playback. Default false.
// Persisted per-machine.
bool getFollowPlayheadDefault();
void setFollowPlayheadDefault (bool follow);

// Tape-head behaviour on Stop. Mirrors the equivalent option in Pro Tools
// (Operation > Transport > "Audio During Fast Forward / Rewind") and
// Logic (Preferences > Recording > "Stop returns to playback start").
// PauseInPlace (default, current behaviour) leaves the playhead where
// the user stopped — pause-and-resume feels musical. ReturnToZero
// rewinds to the timeline origin on every Stop. ReturnToLastClicked
// jumps to the last position the user clicked on the tape strip ruler,
// so Stop -> Play re-cycles a region the user just auditioned without
// having to re-click. Persisted per-machine.
enum class StopBehavior : int
{
    PauseInPlace        = 0,
    ReturnToZero        = 1,
    ReturnToLastClicked = 2,
};
StopBehavior getStopBehavior();
void         setStopBehavior (StopBehavior b);

// Multicore DSP: fan the 24 channel strips' per-block DSP across a pool of
// real-time worker threads (the audio-thread callback runs one lane itself).
// Off = serial (one core, the proven path). Auto = cores-2 workers on machines
// with >=4 cores, else Off — leaves a core for the UI + OS on top of the audio
// thread. Manual = a user-pinned worker count (clamped to the same cap).
// Persisted per-machine: a session authored on a many-core box must not impose
// its worker count on a 4-core target, so this never travels in session.json.
// resolveWorkerCount() maps the stored mode to a concrete count for this host;
// the env var DUSKSTUDIO_AUDIO_WORKERS overrides it (CI / power users).
enum class MulticoreDspMode : int
{
    Off    = 0,
    Auto   = 1,
    Manual = 2,
};
MulticoreDspMode getMulticoreDspMode();
void             setMulticoreDspMode (MulticoreDspMode m);

// Worker count for Manual mode (ignored in Off/Auto). Clamped to
// [1, maxMulticoreWorkers()] on write.
int  getMulticoreManualWorkers();
void setMulticoreManualWorkers (int n);

// Largest worker count this host allows: cores-2, clamped to
// [0, AudioEngine::getMaxWorkerCount()] (the engine's kMaxDspLanes-1). 0 on
// <3-core hosts; never exceeds the engine's worker-lane cap on big machines.
int maxMulticoreWorkers();

// Concrete worker count for this host given the stored mode. 0 = serial.
int resolveWorkerCount();

// Virtual-keyboard centre note (MIDI 0..120). Default 36 (C2) — sits
// near the lower half of the bass register so the visible 2-octave
// window (centre-12..centre+12) covers C1..C3 by default, matching
// typical bassline / synth-bass authoring on a 49-key controller.
// Persisted per-machine so the user's chosen octave survives session
// changes (each time they open the VKB it lands back on the note they
// last shifted it to).
constexpr int kVkbCentreDefault = 36;
int  getVkbCentreNote();
void setVkbCentreNote (int midiNote);
} // namespace duskstudio::appconfig
