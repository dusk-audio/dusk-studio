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
} // namespace duskstudio::appconfig
