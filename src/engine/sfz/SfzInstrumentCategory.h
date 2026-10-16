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

inline InstrumentCategory instrumentCategory (std::string_view name, std::string_view context = {})
{
    const auto classify = [] (std::string_view value)
    {
        std::string words = " ";
        unsigned char previous = 0;
        for (const auto c : value)
        {
            const auto ch = static_cast<unsigned char> (c);
            if (ch >= 'A' && ch <= 'Z' && previous >= 'a' && previous <= 'z') words += ' ';
            words += std::isalnum (ch) ? static_cast<char> (std::tolower (ch)) : ' ';
            previous = ch;
        }
        words += ' ';
        const auto has = [&] (std::initializer_list<const char*> terms)
        {
            for (const auto* term : terms)
                if (words.find (std::string (" ") + term + " ") != std::string::npos
                    || words.find (std::string (" ") + term + "s ") != std::string::npos) return true;
            return false;
        };
        using C = InstrumentCategory;
        if (has ({ "electric piano", "rhodes", "wurlitzer", "wurli", "clavinet", "epiano" })) return C::ElectricPiano;
        if (has ({ "piano", "pianos", "grand", "upright", "scarypiano", "pianoforte" })) return C::Piano;
        if (has ({ "organ", "organs", "hammond", "tonewheel" })) return C::Organ;
        if (has ({ "drum", "drumkit", "drum kit", "drum set", "kick", "snare", "hihat", "cymbal", "tom" })) return C::Drums;
        if (has ({ "percussion", "conga", "bongo", "taiko", "tambourine", "shaker", "cajon", "timpani" })) return C::Percussion;
        if (has ({ "electric guitar", "distorted guitar", "overdriven guitar" })) return C::ElectricGuitar;
        if (has ({ "guitar", "guitars", "banjo", "ganjo", "mandolin", "ukulele", "nylon", "steel string" })) return C::AcousticGuitar;
        if (has ({ "bassoon", "clarinet", "flute", "piccolo", "oboe", "sax", "saxophone", "woodwind", "woodwinds", "recorder" })) return C::Woodwinds;
        if (has ({ "violin", "viola", "cello", "strings", "string ensemble", "pizzicato", "harp" })) return C::Strings;
        if (has ({ "trumpet", "trombone", "horn", "tuba", "brass", "cornet", "euphonium" })) return C::Brass;
        if (has ({ "bass", "basses", "contrabass" })) return C::Bass;
        if (has ({ "marimba", "vibraphone", "xylophone", "glockenspiel", "celesta", "bells", "bell", "mallet", "mallets", "glass" })) return C::Mallets;
        if (has ({ "synth", "synthesizer", "synthesizers", "pad", "pads", "analog", "analogue" })) return C::Synth;
        if (has ({ "voice", "vocal", "vocals", "choir", "choral", "soprano" })) return C::Voice;
        if (has ({ "sitar", "koto", "shamisen", "erhu", "world", "ethnic", "didgeridoo", "accordion", "harmonica" })) return C::World;
        if (has ({ "vcsl", "vsco", "orchestra", "orchestral", "collection", "general midi", "soundbank" })) return C::Collection;
        return C::Other;
    };
    const auto primary = classify (name);
    return primary != InstrumentCategory::Other ? primary : classify (context);
}
} // namespace duskstudio::sfz
