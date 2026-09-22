#pragma once

#include <string>

namespace duskstudio
{
// Copy for the plugin-scan completion notice. Two call sites report the same
// scan - the progress modal's own final state and the alert that follows it -
// so the title lives here rather than being spelled out twice.
inline std::string scanCompleteTitle (bool cancelled)
{
    return cancelled ? "Plugin scan cancelled" : "Plugin scan complete";
}

// A cancelled scan still reports what it added before it stopped, so the count
// is the only input.
inline std::string scanCompleteBody (int added)
{
    return std::to_string (added) + " new plugin" + (added == 1 ? "" : "s") + " added.";
}
} // namespace duskstudio
