#include "ClapScanner.h"

#include <cstdlib>

namespace duskstudio::clap
{
std::vector<juce::File> ClapScanner::defaultSearchPaths()
{
    std::vector<juce::File> dirs;
    auto add = [&dirs] (const juce::File& d)
    {
        if (! d.isDirectory()) return;
        for (const auto& existing : dirs) if (existing == d) return;
        dirs.push_back (d);
    };

    // $CLAP_PATH overrides / extends the defaults (':'-separated, like $PATH).
    if (const char* env = std::getenv ("CLAP_PATH"))
        for (const auto& tok : juce::StringArray::fromTokens (juce::String (env), ":", ""))
            if (tok.isNotEmpty()) add (juce::File (tok.trim()));

    add (juce::File::getSpecialLocation (juce::File::userHomeDirectory).getChildFile (".clap"));
    add (juce::File ("/usr/lib/clap"));
    add (juce::File ("/usr/local/lib/clap"));
    return dirs;
}

std::vector<juce::File> ClapScanner::findClapFiles (const std::vector<juce::File>& dirs)
{
    std::vector<juce::File> files;
    for (const auto& dir : dirs)
    {
        if (! dir.isDirectory()) continue;
        for (const auto& f : dir.findChildFiles (juce::File::findFiles, true, "*.clap"))
        {
            bool seen = false;
            for (const auto& g : files) if (g == f) { seen = true; break; }
            if (! seen) files.push_back (f);
        }
    }
    return files;
}

std::vector<ScannedClap> ClapScanner::scan (const std::vector<juce::File>& dirs)
{
    std::vector<ScannedClap> found;
    for (const auto& file : findClapFiles (dirs))
    {
        ClapBundle bundle;
        std::string err;
        if (! bundle.load (file.getFullPathName().toStdString(), err))
            continue;
        for (const auto& d : bundle.plugins())
            found.push_back ({ file.getFullPathName(), d });
    }
    return found;
}
} // namespace duskstudio::clap
