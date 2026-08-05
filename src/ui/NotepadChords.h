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

// Sorts chord-entry suggestions by diatonic triad fit when detectedKey names a
// supported key. With no claimed key, or an invalid one, candidates are simply
// alphabetical. Invalid and duplicate chord names are omitted.
std::vector<std::string> rankCandidates (std::vector<std::string> candidates,
                                         std::string_view detectedKey);

// Best-fitting key for a set of chord names, or empty when the evidence does
// not support a claim. The rule, which is not to be tuned against a sample
// document:
//   - Power chords and suspensions carry no third. They count towards the root
//     set and never towards major or minor.
//   - At least three distinct roots must carry a third before a key is named.
//   - If more than one key scores the maximum, the answer is blank.
// The result is spelled the way key signatures are written (C#m, not Dbm),
// independent of how the document spells its chords.
std::string detectKey (const std::vector<std::string>& names);
} // namespace duskstudio::notepad::chords
