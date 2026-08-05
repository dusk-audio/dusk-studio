#pragma once

#include <string>
#include <string_view>
#include <vector>

namespace duskstudio::notepad::chords
{
// ChordPro brackets share their opening delimiter with Markdown links, and a
// lyric sheet is full of ordinary bracketed asides. A token only counts as a
// chord when it parses completely: root note, optional accidental, a suffix
// built from recognised quality tokens, and an optional /bass note. "[Bass]"
// and "[verse 2]" fail that parse and stay literal text.
bool isChord (std::string_view token) noexcept;

// Transposes a chord token by semitones, spelling accidentals as flats when
// preferFlats is set and sharps otherwise. Non-chord tokens are returned
// unchanged so a caller can run this over a whole document blindly.
std::string transpose (std::string_view token, int semitones, bool preferFlats);

// Best-fitting major or minor key for a set of chord names, scored by how many
// roots are diatonic to it, with the first chord's root breaking ties the way
// a chart usually resolves. Empty when there is too little to go on.
std::string detectKey (const std::vector<std::string>& names, bool preferFlats);
} // namespace duskstudio::notepad::chords
