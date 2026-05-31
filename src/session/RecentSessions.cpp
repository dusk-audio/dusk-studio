#include "RecentSessions.h"

namespace duskstudio
{
juce::File RecentSessions::getStoreFile()
{
    auto cfgDir = juce::File::getSpecialLocation (juce::File::userApplicationDataDirectory)
                      .getChildFile ("Dusk Studio");
    if (! cfgDir.isDirectory() && cfgDir.createDirectory().failed())
        return {};   // empty File - load() returns empty, add() drops the write
    return cfgDir.getChildFile ("recent.txt");
}

juce::Array<juce::File> RecentSessions::load()
{
    juce::Array<juce::File> result;
    const auto store = getStoreFile();
    if (! store.existsAsFile()) return result;

    juce::StringArray lines;
    store.readLines (lines);
    for (auto& line : lines)
    {
        const auto trimmed = line.trim();
        if (trimmed.isEmpty()) continue;

        const juce::File f (trimmed);
        // Prune stale entries - a removed session directory is worse than
        // useless in the picker UI. Also dedupe (load can be called after a
        // partial write).
        if (f.isDirectory() && ! result.contains (f))
            result.add (f);
    }
    return result;
}

void RecentSessions::add (const juce::File& sessionDirectory)
{
    if (sessionDirectory == juce::File()) return;

    auto entries = load();

    // Newest goes to the front; remove any existing copy first so we don't
    // promote a duplicate.
    entries.removeAllInstancesOf (sessionDirectory);
    entries.insert (0, sessionDirectory);

    while (entries.size() > kMaxEntries)
        entries.remove (entries.size() - 1);

    juce::StringArray lines;
    for (auto& f : entries) lines.add (f.getFullPathName());
    getStoreFile().replaceWithText (lines.joinIntoString ("\n"));
}

void RecentSessions::clear()
{
    const auto store = getStoreFile();
    if (store == juce::File()) return;
    if (store.existsAsFile()) store.deleteFile();
}
} // namespace duskstudio
