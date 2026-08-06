#include "NotepadChords.h"

#include <algorithm>
#include <utility>
#include <array>
#include <cctype>
#include <vector>

namespace duskstudio::notepad::chords
{
namespace
{
constexpr int kInvalidPitch = -1;

constexpr std::array<const char*, 12> kSharpNames {
    "C", "C#", "D", "D#", "E", "F", "F#", "G", "G#", "A", "A#", "B"
};
constexpr std::array<const char*, 12> kFlatNames {
    "C", "Db", "D", "Eb", "E", "F", "Gb", "G", "Ab", "A", "Bb", "B"
};

int naturalPitch (char note) noexcept
{
    switch (note)
    {
        case 'C': return 0;
        case 'D': return 2;
        case 'E': return 4;
        case 'F': return 5;
        case 'G': return 7;
        case 'A': return 9;
        case 'B': return 11;
        default:  return kInvalidPitch;
    }
}

// Consumes a root note at position i, returning its pitch class. i advances
// past the accidental. A trailing 'b' is the flat, not the start of a "b5"
// alteration - "Bb5" is B flat, the reading every chord chart uses.
int readRoot (std::string_view token, std::size_t& i) noexcept
{
    if (i >= token.size())
        return kInvalidPitch;

    const auto pitch = naturalPitch (token[i]);
    if (pitch == kInvalidPitch)
        return kInvalidPitch;
    ++i;

    if (i < token.size() && (token[i] == '#' || token[i] == 'b'))
    {
        const auto shifted = token[i] == '#' ? pitch + 1 : pitch + 11;
        ++i;
        return shifted % 12;
    }
    return pitch;
}

bool matchesWord (std::string_view token, std::size_t i, std::string_view word) noexcept
{
    if (token.size() - i < word.size())
        return false;
    for (std::size_t n = 0; n < word.size(); ++n)
        if (std::tolower (static_cast<unsigned char> (token[i + n])) != word[n])
            return false;
    return true;
}

// The suffix is whatever qualifies the root: a run of quality words, single
// modifier characters and figure digits. Anything else - a stray letter, a
// space, punctuation that never appears in a chord name - fails the parse.
bool readSuffix (std::string_view token, std::size_t& i) noexcept
{
    constexpr std::array<std::string_view, 8> words {
        "maj", "min", "sus", "add", "aug", "dim", "alt", "no"
    };
    constexpr std::string_view modifiers = "mM+-#b()^*,";

    while (i < token.size())
    {
        if (std::isdigit (static_cast<unsigned char> (token[i])) != 0)
        {
            while (i < token.size() && std::isdigit (static_cast<unsigned char> (token[i])) != 0)
                ++i;
            continue;
        }

        bool matchedWord = false;
        for (const auto word : words)
        {
            if (matchesWord (token, i, word))
            {
                i += word.size();
                matchedWord = true;
                break;
            }
        }
        if (matchedWord)
            continue;

        if (modifiers.find (token[i]) != std::string_view::npos)
        {
            ++i;
            continue;
        }
        return false;
    }
    return true;
}

// A suspension is often infixed - "7sus4", "13sus4" - so the whole suffix is
// searched rather than only its head.
bool hasSuspension (std::string_view suffix) noexcept
{
    for (std::size_t i = 0; i + 3 <= suffix.size(); ++i)
        if (matchesWord (suffix, i, "sus"))
            return true;
    return false;
}

struct ParsedChord
{
    int  root = kInvalidPitch;
    std::string suffix;
    int  bass = kInvalidPitch;
};

bool parse (std::string_view token, ParsedChord& out)
{
    if (token.empty() || token.size() > 16)
        return false;
    for (const auto ch : token)
        if (std::isspace (static_cast<unsigned char> (ch)) != 0)
            return false;

    const auto slash = token.find ('/');
    const auto head = token.substr (0, slash);

    std::size_t i = 0;
    out.root = readRoot (head, i);
    if (out.root == kInvalidPitch)
        return false;
    const auto suffixStart = i;
    if (! readSuffix (head, i))
        return false;
    out.suffix = std::string (head.substr (suffixStart));

    if (slash == std::string_view::npos)
        return true;

    const auto bass = token.substr (slash + 1);
    std::size_t j = 0;
    out.bass = readRoot (bass, j);
    if (out.bass == kInvalidPitch || j != bass.size())
        return false;
    return true;
}

int shiftPitch (int pitch, int semitones) noexcept
{
    const auto wrapped = (pitch + semitones) % 12;
    return wrapped < 0 ? wrapped + 12 : wrapped;
}
} // namespace

bool isChord (std::string_view token) noexcept
{
    ParsedChord parsed;
    return parse (token, parsed);
}

std::string transpose (std::string_view token, int semitones, bool preferFlats)
{
    ParsedChord parsed;
    if (! parse (token, parsed))
        return std::string (token);

    const auto& names = preferFlats ? kFlatNames : kSharpNames;
    std::string result = names[static_cast<std::size_t> (shiftPitch (parsed.root, semitones))];
    result += parsed.suffix;
    if (parsed.bass != kInvalidPitch)
    {
        result += '/';
        result += names[static_cast<std::size_t> (shiftPitch (parsed.bass, semitones))];
    }
    return result;
}

namespace
{
enum class Triad { major, minor, diminished, other };

Triad triadFor (const ParsedChord& chord) noexcept
{
    // A suspension or an omitted third settles the question before any quality
    // letter does: neither names a major or a minor triad.
    if (hasSuspension (chord.suffix)
        || chord.suffix.compare (0, 3, "aug") == 0
        || (! chord.suffix.empty() && chord.suffix[0] == '+')
        || chord.suffix == "5")
        return Triad::other;
    if (chord.suffix.compare (0, 3, "dim") == 0)
        return Triad::diminished;
    if (! chord.suffix.empty() && chord.suffix[0] == 'm'
        && chord.suffix.compare (0, 3, "maj") != 0)
        return Triad::minor;
    return Triad::major;
}

int diatonicRank (const ParsedChord& candidate, int tonic, bool minor) noexcept
{
    constexpr std::array<int, 7> kMajorDegrees { 0, 2, 4, 5, 7, 9, 11 };
    constexpr std::array<Triad, 7> kMajorTriads {
        Triad::major, Triad::minor, Triad::minor, Triad::major,
        Triad::major, Triad::minor, Triad::diminished
    };
    constexpr std::array<int, 7> kMinorDegrees { 0, 2, 3, 5, 7, 8, 10 };
    constexpr std::array<Triad, 7> kMinorTriads {
        Triad::minor, Triad::diminished, Triad::major, Triad::minor,
        Triad::minor, Triad::major, Triad::major
    };

    const auto& degrees = minor ? kMinorDegrees : kMajorDegrees;
    const auto& triads = minor ? kMinorTriads : kMajorTriads;
    for (std::size_t i = 0; i < degrees.size(); ++i)
    {
        if ((tonic + degrees[i]) % 12 != candidate.root)
            continue;
        return triadFor (candidate) == triads[i] ? 0 : 1;
    }
    return 2;
}

char lower (char c) noexcept
{
    return static_cast<char> (std::tolower (static_cast<unsigned char> (c)));
}

bool alphabeticallyBefore (const std::string& left, const std::string& right) noexcept
{
    return std::lexicographical_compare (
        left.begin(), left.end(), right.begin(), right.end(),
        [] (char a, char b) { return lower (a) < lower (b); });
}
} // namespace

bool completes (std::string_view candidate, std::string_view draft) noexcept
{
    if (draft.empty() || draft.size() > candidate.size())
        return false;
    for (std::size_t i = 0; i < draft.size(); ++i)
        if (lower (draft[i]) != lower (candidate[i]))
            return false;
    return true;
}

std::vector<std::string> rankCandidates (std::vector<std::string> candidates,
                                         std::string_view detectedKey)
{
    candidates.erase (std::remove_if (candidates.begin(), candidates.end(),
                                      [] (const std::string& name) { return ! isChord (name); }),
                      candidates.end());
    std::sort (candidates.begin(), candidates.end(), alphabeticallyBefore);
    candidates.erase (std::unique (candidates.begin(), candidates.end()), candidates.end());

    ParsedChord key;
    if (! parse (detectedKey, key) || key.bass != kInvalidPitch
        || (! key.suffix.empty() && key.suffix != "m"))
        return candidates;
    const bool minor = key.suffix == "m";
    std::stable_sort (candidates.begin(), candidates.end(),
                      [&] (const std::string& left, const std::string& right)
                      {
                          ParsedChord parsedLeft;
                          ParsedChord parsedRight;
                          (void) parse (left, parsedLeft);
                          (void) parse (right, parsedRight);
                          return diatonicRank (parsedLeft, key.root, minor)
                               < diatonicRank (parsedRight, key.root, minor);
                      });
    return candidates;
}

// Key signatures as they are actually written: minor keys take sharps at C#,
// F# and G#, majors take flats at Db, Eb, Ab and Bb, and the enharmonic pair
// at 6 accidentals resolves to F# either way.
namespace
{
const char* keyName (int tonic, bool minor) noexcept
{
    constexpr std::array<const char*, 12> kMajorKeys {
        "C", "Db", "D", "Eb", "E", "F", "F#", "G", "Ab", "A", "Bb", "B"
    };
    constexpr std::array<const char*, 12> kMinorKeys {
        "C", "C#", "D", "Eb", "E", "F", "F#", "G", "G#", "A", "Bb", "B"
    };
    const auto index = static_cast<std::size_t> (tonic);
    return minor ? kMinorKeys[index] : kMajorKeys[index];
}

// A suspension replaces the third and a power chord omits it, so neither says
// anything about mode.
bool carriesThird (const std::string& suffix) noexcept
{
    if (hasSuspension (suffix))
        return false;
    for (const auto ch : suffix)
        if (ch != '5' && ch != '(' && ch != ')')
            return true;
    return suffix.find ('5') == std::string::npos;
}
} // namespace

std::string detectKey (const std::vector<std::string>& names)
{
    struct Root { int pitch; bool minor; bool third; };
    std::vector<Root> roots;
    for (const auto& name : names)
    {
        ParsedChord parsed;
        if (! parse (name, parsed))
            continue;
        const bool third = carriesThird (parsed.suffix);
        const bool minor = third && ! parsed.suffix.empty() && parsed.suffix[0] == 'm'
                        && parsed.suffix.compare (0, 3, "maj") != 0;
        roots.push_back ({ parsed.root, minor, third });
    }

    std::vector<int> thirdRoots;
    for (const auto& root : roots)
        if (root.third && std::find (thirdRoots.begin(), thirdRoots.end(), root.pitch)
                              == thirdRoots.end())
            thirdRoots.push_back (root.pitch);
    if (thirdRoots.size() < 3)
        return {};

    constexpr std::array<int, 7> kMajorDegrees { 0, 2, 4, 5, 7, 9, 11 };
    constexpr std::array<int, 7> kMinorDegrees { 0, 2, 3, 5, 7, 8, 10 };

    std::vector<std::pair<int, bool>> best;
    int bestScore = 0;
    for (int tonic = 0; tonic < 12; ++tonic)
    {
        for (const bool minor : { false, true })
        {
            const auto& degrees = minor ? kMinorDegrees : kMajorDegrees;
            int score = 0;
            for (const auto& root : roots)
            {
                const bool diatonic = std::find_if (degrees.begin(), degrees.end(),
                                                    [&] (int degree)
                                                    {
                                                        return (tonic + degree) % 12 == root.pitch;
                                                    }) != degrees.end();
                if (diatonic)
                    ++score;
            }
            if (score > bestScore)
            {
                bestScore = score;
                best.clear();
            }
            if (score == bestScore)
                best.emplace_back (tonic, minor);
        }
    }

    // A key and its relative always share a pitch set, so diatonic fit alone
    // cannot separate them. The opening triad is what states the tonic; when it
    // is not one of the tied candidates, the evidence is genuinely ambiguous
    // and the answer is blank.
    if (bestScore == 0 || best.empty())
        return {};
    if (best.size() > 1)
    {
        const auto opening = std::find_if (roots.begin(), roots.end(),
                                           [] (const Root& root) { return root.third; });
        if (opening == roots.end())
            return {};
        const std::pair<int, bool> stated { opening->pitch, opening->minor };
        if (std::find (best.begin(), best.end(), stated) == best.end())
            return {};
        best.assign (1, stated);
    }

    const auto [bestTonic, bestMinor] = best.front();
    return std::string (keyName (bestTonic, bestMinor)) + (bestMinor ? "m" : "");
}
} // namespace duskstudio::notepad::chords
