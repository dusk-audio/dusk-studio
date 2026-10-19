#pragma once

#include <array>
#include <cctype>
#include <initializer_list>
#include <string>
#include <string_view>

namespace duskstudio::sfz
{
enum class InstrumentCategory
{
    Piano, ElectricPiano, Organ, Drums, Percussion, AcousticGuitar, ElectricGuitar,
    Bass, Strings, Brass, Woodwinds, Mallets, Synth, Voice, World, Collection, Other
};

struct CategoryInfo
{
    const char* id;
    const char* name;
};

inline constexpr std::array<CategoryInfo, 17> kInstrumentCategories {{
    { "piano", "Piano" }, { "electric-piano", "Electric Piano" }, { "organ", "Organ" },
    { "drums", "Drum Kit" }, { "percussion", "Percussion" }, { "acoustic-guitar", "Acoustic Guitar" },
    { "electric-guitar", "Electric Guitar" }, { "bass", "Bass" }, { "strings", "Strings" },
    { "brass", "Brass" }, { "woodwinds", "Woodwinds" }, { "mallets", "Mallets & Bells" },
    { "synth", "Synthesizer" }, { "voice", "Voice" }, { "world", "World" },
    { "collection", "Collections" }, { "other", "Other Instruments" }
}};

inline std::string instrumentWords (std::string_view value)
{
    std::string words = " ";
    unsigned char previous = 0;
    for (const auto c : value)
    {
        const auto ch = static_cast<unsigned char> (c);
        if (ch >= 'A' && ch <= 'Z' && previous >= 'a' && previous <= 'z') words += ' ';
        if (std::isalnum (ch)) words += static_cast<char> (std::tolower (ch));
        else if (words.back() != ' ') words += ' ';
        previous = ch;
    }
    if (words.back() != ' ') words += ' ';
    return words;
}

inline bool instrumentWordsContain (const std::string& words, std::initializer_list<const char*> terms)
{
    for (const auto* term : terms)
        if (words.find (std::string (" ") + term + " ") != std::string::npos
            || words.find (std::string (" ") + term + "s ") != std::string::npos) return true;
    return false;
}

inline InstrumentCategory instrumentCategory (std::string_view name, std::string_view context = {}, std::string_view fallback = {})
{
    const auto classify = [] (std::string_view value, bool packName)
    {
        const auto words = instrumentWords (value);
        const auto has = [&] (std::initializer_list<const char*> terms)
        {
            return instrumentWordsContain (words, terms);
        };
        using C = InstrumentCategory;
        if (packName && has ({ "general midi", "soundbank" })) return C::Collection;
        if (has ({ "synth", "synthesizer", "pad", "analog", "analogue", "dx7", "minimoog", "moog", "juno", "prophet" })) return C::Synth;
        if (has ({ "electric piano", "rhodes", "wurlitzer", "wurli", "clavinet", "epiano" })) return C::ElectricPiano;
        if (has ({ "upright bass", "double bass", "electric bass", "acoustic bass", "bass guitar", "contrabass" })
            && ! has ({ "clarinet", "flute", "bassoon", "trombone" })) return C::Bass;
        if (has ({ "piano", "grand", "upright", "scarypiano", "pianoforte", "harpsichord", "clavichord", "spinet" })) return C::Piano;
        if (has ({ "mouth organ" })) return C::World;
        if (has ({ "organ", "organs", "hammond", "tonewheel" })) return C::Organ;
        if (has ({ "steel drum", "steelpan", "steel pan", "handpan", "hang drum" })) return C::Percussion;
        if (has ({ "drum", "drumkit", "drum kit", "drum set", "kick", "snare", "hihat", "cymbal", "tom" })) return C::Drums;
        if (has ({ "percussion", "conga", "bongo", "taiko", "tambourine", "shaker", "cajon", "timpani", "djembe", "tabla", "castanet", "claves", "guiro", "agogo", "woodblock" })) return C::Percussion;
        if (has ({ "electric guitar", "distorted guitar", "overdriven guitar", "stratocaster", "telecaster", "les paul" })) return C::ElectricGuitar;
        if (has ({ "guitar", "guitars", "banjo", "ganjo", "mandolin", "ukulele", "uke", "nylon", "steel string" })) return C::AcousticGuitar;
        if (has ({ "bassoon", "clarinet", "flute", "piccolo", "oboe", "english horn", "cor anglais", "sax", "saxophone", "woodwind", "woodwinds", "recorder" })) return C::Woodwinds;
        if (has ({ "violin", "viola", "cello", "violoncello", "strings", "string ensemble", "pizzicato", "harp" })) return C::Strings;
        if (has ({ "trumpet", "trombone", "horn", "tuba", "brass", "cornet", "flugelhorn", "euphonium" })) return C::Brass;
        if (has ({ "bass", "basses", "contrabass" })) return C::Bass;
        if (has ({ "marimba", "vibraphone", "vibes", "xylophone", "glockenspiel", "glock", "celesta", "bell", "handbell", "mallet", "glass", "chime", "carillon", "music box", "tubular bell" })) return C::Mallets;
        if (has ({ "voice", "vocal", "vocals", "choir", "choral", "soprano" })) return C::Voice;
        if (has ({ "sitar", "koto", "shamisen", "erhu", "world", "ethnic", "didgeridoo", "accordion", "harmonica", "bandoneon", "concertina", "hurdy gurdy", "dulcimer", "oud", "shakuhachi", "duduk", "bagpipe", "kalimba", "mbira", "balafon" })) return C::World;
        if (has ({ "vcsl", "vsco", "orchestra", "orchestral", "collection", "general midi", "soundbank" })) return C::Collection;
        return C::Other;
    };
    const auto primary = classify (name, true);
    if (primary != InstrumentCategory::Other) return primary;
    const auto secondary = classify (context, false);
    return secondary != InstrumentCategory::Other ? secondary : classify (fallback, false);
}
} // namespace duskstudio::sfz
