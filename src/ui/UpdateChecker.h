#pragma once

#include <juce_core/juce_core.h>
#include <juce_events/juce_events.h>
#include <nlohmann/json.hpp>
#include <functional>
#include <algorithm>

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

// "v1.2.3", "1.2.3", "v1.2.3-beta.4". The prerelease suffix is kept so
// 1.2.3 ranks above 1.2.3-beta.4 (semver: a release outranks any of its
// prereleases).
struct ParsedVersion
{
    int nums[3] = { 0, 0, 0 };
    juce::String prerelease;   // empty = stable release
};

// False when the tag doesn't lead with three dot-separated ints.
inline bool parseVersion (juce::String tag, ParsedVersion& out)
{
    tag = tag.trim();
    if (tag.startsWithIgnoreCase ("v")) tag = tag.substring (1);
    out.prerelease = tag.fromFirstOccurrenceOf ("-", false, false);
    tag = tag.upToFirstOccurrenceOf ("-", false, false);
    auto parts = juce::StringArray::fromTokens (tag, ".", {});
    if (parts.size() < 3) return false;
    for (int i = 0; i < 3; ++i)
    {
        if (! parts[i].containsOnly ("0123456789") || parts[i].isEmpty())
            return false;
        out.nums[i] = parts[i].getIntValue();
    }
    return true;
}

// Semver-ish prerelease ordering: dot-separated identifiers compared
// left to right; numeric pairs compare numerically, numeric < alpha,
// alpha pairs compare lexically, and the shorter list loses when it's
// a prefix of the longer ("beta" < "beta.1").
inline int comparePrerelease (const juce::String& a, const juce::String& b)
{
    const auto pa = juce::StringArray::fromTokens (a, ".", {});
    const auto pb = juce::StringArray::fromTokens (b, ".", {});
    for (int i = 0; i < std::min (pa.size(), pb.size()); ++i)
    {
        const bool na = pa[i].containsOnly ("0123456789") && pa[i].isNotEmpty();
        const bool nb = pb[i].containsOnly ("0123456789") && pb[i].isNotEmpty();
        if (na && nb)
        {
            const int va = pa[i].getIntValue();
            const int vb = pb[i].getIntValue();
            if (va != vb) return va < vb ? -1 : 1;   // direct compare - no subtraction overflow
        }
        else if (na != nb)
        {
            return na ? -1 : 1;
        }
        else if (const int d = pa[i].compare (pb[i]); d != 0)
        {
            return d;
        }
    }
    return pa.size() < pb.size() ? -1 : (pa.size() > pb.size() ? 1 : 0);
}

inline bool isNewer (const ParsedVersion& candidate, const ParsedVersion& current)
{
    for (int i = 0; i < 3; ++i)
    {
        if (candidate.nums[i] > current.nums[i]) return true;
        if (candidate.nums[i] < current.nums[i]) return false;
    }
    // Equal triplets: a stable release outranks any prerelease of it.
    if (candidate.prerelease.isEmpty()) return current.prerelease.isNotEmpty();
    if (current.prerelease.isEmpty())   return false;
    return comparePrerelease (candidate.prerelease, current.prerelease) > 0;
}

// Blocking fetch - call from a background thread only. The GitHub API
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
// participate with semver ordering, so 0.12.0-beta.1 still flags from
// 0.11.0 but ranks below a stable 0.12.0.
inline juce::String highestTag (const juce::String& json)
{
    const auto parsed = nlohmann::json::parse (json.toStdString(), nullptr, false);
    if (! parsed.is_array()) return {};

    juce::String bestName;
    ParsedVersion best;
    best.nums[0] = best.nums[1] = best.nums[2] = -1;
    for (const auto& entry : parsed)
    {
        if (! entry.contains ("name") || ! entry["name"].is_string()) continue;
        const juce::String name { entry["name"].get<std::string>() };
        ParsedVersion v;
        if (parseVersion (name, v) && isNewer (v, best))
        {
            best = v;
            bestName = name;
        }
    }
    return bestName;
}

// Fire-and-forget. onNewer runs on the message thread with the newer tag's
// name; never called when up to date or on any failure.
//
// LIFETIME: onNewer fires after a network round-trip (seconds), so it may
// outlive the caller. It is NOT given a lifetime token - the caller MUST
// capture a juce::Component::SafePointer / std::weak_ptr to any owned object
// and re-check it inside onNewer (which runs on the message thread). See the
// call site in MainComponent for the SafePointer pattern.
inline void checkForNewerTagAsync (juce::String currentVersion,
                                   std::function<void (juce::String)> onNewer)
{
    juce::Thread::launch ([currentVersion = std::move (currentVersion),
                           onNewer = std::move (onNewer)]
    {
        ParsedVersion current;
        if (! parseVersion (currentVersion, current)) return;

        const auto tag = highestTag (fetchTagsJson());
        if (tag.isEmpty()) return;

        ParsedVersion latest;
        if (! parseVersion (tag, latest) || ! isNewer (latest, current))
            return;

        juce::MessageManager::callAsync ([tag, onNewer] { if (onNewer) onNewer (tag); });
    });
}
} // namespace updatecheck
} // namespace duskstudio
