#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <functional>

namespace duskstudio
{
namespace updatecheck
{
// Best-effort "newer tag exists" probe against the public source repo.
// Fail-silent by design: offline, rate-limited, no curl binary, or any
// parse failure simply means no callback. Never blocks the message
// thread; the callback is marshalled back to it.
inline constexpr const char* kTagsUrl =
    "https://api.github.com/repos/dusk-audio/dusk-studio/tags?per_page=20";

// "v1.2.3", "1.2.3", "v1.2.3-beta.4" -> {1,2,3}. False when the tag
// doesn't lead with three dot-separated ints.
inline bool parseVersionTriplet (juce::String tag, int (&out)[3])
{
    tag = tag.trim();
    if (tag.startsWithIgnoreCase ("v")) tag = tag.substring (1);
    tag = tag.upToFirstOccurrenceOf ("-", false, false);  // drop prerelease suffix
    auto parts = juce::StringArray::fromTokens (tag, ".", {});
    if (parts.size() < 3) return false;
    for (int i = 0; i < 3; ++i)
    {
        if (! parts[i].containsOnly ("0123456789") || parts[i].isEmpty())
            return false;
        out[i] = parts[i].getIntValue();
    }
    return true;
}

inline bool isNewer (const int (&candidate)[3], const int (&current)[3])
{
    for (int i = 0; i < 3; ++i)
    {
        if (candidate[i] > current[i]) return true;
        if (candidate[i] < current[i]) return false;
    }
    return false;   // equal base versions are not "newer" (prerelease ignored)
}

// Blocking fetch — call from a background thread only. The GitHub API
// rejects requests without a User-Agent, so both paths set one.
inline juce::String fetchTagsJson()
{
   #if JUCE_LINUX
    // JUCE_USE_CURL=0 build: juce::URL can't do HTTPS on Linux, so use
    // the system curl binary (present on effectively every desktop
    // distro). Missing binary or non-zero exit -> empty string.
    juce::ChildProcess proc;
    if (! proc.start (juce::StringArray { "curl", "-fsSL", "--max-time", "5",
                                          "-A", "DuskStudio-update-check",
                                          juce::String (kTagsUrl) }))
        return {};
    const auto out = proc.readAllProcessOutput();
    return proc.getExitCode() == 0 ? out : juce::String();
   #else
    const juce::URL url { juce::String (kTagsUrl) };
    auto stream = url.createInputStream (
        juce::URL::InputStreamOptions (juce::URL::ParameterHandling::inAddress)
            .withConnectionTimeoutMs (5000)
            .withExtraHeaders ("User-Agent: DuskStudio-update-check"));
    return stream != nullptr ? stream->readEntireStreamAsString() : juce::String();
   #endif
}

// Highest release tag in the JSON, or empty. Prerelease tags ("-beta")
// count by their base triplet, so 0.12.0-beta.1 still flags from 0.11.0.
inline juce::String highestTag (const juce::String& json)
{
    const auto parsed = juce::JSON::parse (json);
    const auto* arr = parsed.getArray();
    if (arr == nullptr) return {};

    juce::String bestName;
    int best[3] = { -1, -1, -1 };
    for (const auto& entry : *arr)
    {
        const auto name = entry.getProperty ("name", {}).toString();
        int v[3];
        if (parseVersionTriplet (name, v) && isNewer (v, best))
        {
            best[0] = v[0]; best[1] = v[1]; best[2] = v[2];
            bestName = name;
        }
    }
    return bestName;
}

// Fire-and-forget. onNewer runs on the message thread with the newer
// tag's name; never called when up to date or on any failure.
inline void checkForNewerTagAsync (juce::String currentVersion,
                                   std::function<void (juce::String)> onNewer)
{
    juce::Thread::launch ([currentVersion = std::move (currentVersion),
                           onNewer = std::move (onNewer)]
    {
        int current[3];
        if (! parseVersionTriplet (currentVersion, current)) return;

        const auto tag = highestTag (fetchTagsJson());
        if (tag.isEmpty()) return;

        int latest[3];
        if (! parseVersionTriplet (tag, latest) || ! isNewer (latest, current))
            return;

        juce::MessageManager::callAsync ([tag, onNewer] { if (onNewer) onNewer (tag); });
    });
}
} // namespace updatecheck
} // namespace duskstudio
