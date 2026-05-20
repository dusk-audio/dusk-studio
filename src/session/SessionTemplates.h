#pragma once

#include "Session.h"

namespace duskstudio
{
// Opinionated starting points for "New session". Each template stamps
// track names / colours / modes onto a fresh Session so the user is one
// arm-click away from recording. Templates only mutate the per-track
// surface (name, colour, mode); they intentionally leave fader, EQ, comp,
// sends, automation, regions, and plugins at session defaults so no
// template hides surprises in the signal chain.
//
// Adding a template: append to the enum, add a name to nameForTemplate,
// add a stamp routine to applyTemplate. The menu surface in MainComponent
// auto-iterates the enum so a new template appears without further wiring.
enum class SessionTemplate
{
    Blank = 0,        // numeric track names + hue-rotated palette (default)
    Band,             // Drums / Bass / Guitars / Keys / Vocals
    Beats,            // Kick / Snare / Hat / Perc / 808 / Pad / Lead / Vox
    SingerSongwriter, // Vocal / Acoustic gtr / Bass / Synth / Drums
    kCount
};

// Display label for a template - shown in the File → New menu.
const char* nameForTemplate (SessionTemplate t) noexcept;

// Stamp the given template onto an existing Session. Resets per-track
// names / colours / modes for the first N tracks the template covers;
// remaining tracks fall back to numeric / hue-rotated defaults so the
// 16-channel grid is always fully populated.
void applyTemplate (Session& session, SessionTemplate t);
} // namespace duskstudio
