#pragma once

#include <juce_core/juce_core.h>

#include <optional>
#include <vector>

namespace duskstudio
{
// Parses a Plogue ARIA bank manifest (e.g. "Swirly Drums.bank.xml") that
// sits next to an .sfz file. The manifest lists one or more programs,
// each pairing a Sforzando-style GUI XML with the .sfz it drives.
//
// We only consume the subset of the ARIA bank format that affects UI
// loading - <AriaProgram name=".." gui="path/to/GUI.xml"><AriaElement
// path="Programs/Foo.sfz"/></AriaProgram>. <Define>, scripts, key
// authentication etc. are ignored.
struct AriaProgram
{
    juce::String name;
    juce::File   sfzFile;     // resolved against bank dir
    juce::File   guiFile;     // resolved against bank dir; may not exist on disk
};

struct AriaBank
{
    juce::File              bankFile;       // the *.bank.xml itself
    juce::File              bankDir;        // parent of bankFile
    juce::String            bankName;       // <AriaBank name="..."> attribute
    juce::String            vendor;
    std::vector<AriaProgram> programs;
    // Index into programs[] whose sfzFile matches the .sfz the caller
    // loaded, or -1 if not found. Set by tryLoadFromSfz when it locates
    // the bank for a given file.
    int                      selectedIndex { -1 };

    // Search from sfzFile's directory upward (up to a few levels) for a
    // *.bank.xml file. Returns nullopt if none is found or the file
    // can't be parsed. The matching program is preselected when the
    // input .sfz path is listed in the bank.
    static std::optional<AriaBank> tryLoadFromSfz(const juce::File& sfzFile);
};
} // namespace duskstudio
