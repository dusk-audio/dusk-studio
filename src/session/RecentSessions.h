#pragma once

#include <juce_core/juce_core.h>

namespace duskstudio
{
// Persistent list of session directories the user has recently saved/loaded.
// Stored as a newline-delimited file at <userApplicationDataDirectory>/Focal/recent.txt
// (one absolute path per line, most recent first). Cap at kMaxEntries - older
// entries are evicted on overflow. Stale paths (directory removed) are pruned
// on read so the startup dialog never shows broken entries.
//
// Threading: read on the message thread (startup dialog, save/load callbacks);
// not safe to call concurrently. The file is small enough that this isn't a
// real constraint.
class RecentSessions
{
public:
    static constexpr int kMaxEntries = 10;

    static juce::Array<juce::File> load();
    static void                    add (const juce::File& sessionDirectory);

private:
    static juce::File getStoreFile();
};
} // namespace duskstudio
