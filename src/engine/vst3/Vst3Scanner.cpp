#include "Vst3Scanner.h"

#include <cstdlib>

namespace duskstudio::vst3
{
std::vector<juce::File> Vst3Scanner::defaultSearchPaths()
{
    std::vector<juce::File> dirs;
    auto add = [&dirs] (const juce::File& d)
    {
        if (! d.isDirectory()) return;
        for (const auto& existing : dirs) if (existing == d) return;
        dirs.push_back (d);
    };

    // $VST3_PATH overrides / extends the defaults (':'-separated, like $PATH).
    if (const char* env = std::getenv ("VST3_PATH"))
        for (const auto& tok : juce::StringArray::fromTokens (juce::String (env), ":", ""))
            if (tok.isNotEmpty()) add (juce::File (tok.trim()));

    add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".vst3"));
    add (juce::File ("/usr/lib/vst3"));
    add (juce::File ("/usr/local/lib/vst3"));
    return dirs;
}

std::vector<juce::File> Vst3Scanner::findVst3Bundles (const std::vector<juce::File>& dirs)
{
    std::vector<juce::File> files;
    auto add = [&files] (const juce::File& f)
    {
        for (const auto& g : files) if (g == f) return;
        files.push_back (f);
    };

    for (const auto& dir : dirs)
    {
        if (! dir.isDirectory()) continue;
        // Bundle directories first — don't descend into them (their inner .so is
        // not a module path the SDK loader takes).
        for (const auto& f : dir.findChildFiles (juce::File::findFilesAndDirectories, false))
        {
            if (! f.hasFileExtension ("vst3")) continue;
            add (f);
        }
        // Plain subdirectories (vendor folders) one level down.
        for (const auto& sub : dir.findChildFiles (juce::File::findDirectories, false))
            if (! sub.hasFileExtension ("vst3"))
                for (const auto& f : sub.findChildFiles (juce::File::findFilesAndDirectories, false))
                    if (f.hasFileExtension ("vst3"))
                        add (f);
    }
    return files;
}

std::vector<ScannedVst3> Vst3Scanner::scan (const std::vector<juce::File>& dirs)
{
    std::vector<ScannedVst3> found;
    for (const auto& file : findVst3Bundles (dirs))
    {
        Vst3Bundle bundle;
        std::string err;
        if (! bundle.load (file.getFullPathName().toStdString(), err))
            continue;
        for (const auto& d : bundle.plugins())
            found.push_back ({ file.getFullPathName(), d });
    }
    return found;
}
} // namespace duskstudio::vst3
