#include "NotepadChords.h"

#include <array>
#include <cctype>

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

struct ParsedChord
{
    int  root = kInvalidPitch;
    std::string suffix;
    int  bass = kInvalidPitch;
    std::string bassSuffix;   // empty for a well-formed slash bass
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
} // namespace duskstudio::notepad::chords
