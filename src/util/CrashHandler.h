#pragma once

#include <string>

namespace duskstudio::crash_handler
{
// Install the global JUCE FileLogger + JUCE crash callback. Idempotent.
// Call once from DuskStudioApp::initialise BEFORE any audio init so the
// first thing a crashing build does is leave a report in the crashes directory.
//
// Layout under ${userApplicationData}/Dusk Studio/:
//   log/dusk-studio-YYYYMMDD.log   - rotating daily, FileLogger
//   crashes/crash-<iso>.txt  - one per terminate; backtrace + env summary
//
// Patreon support flow: ask user to attach the most recent file from
// each folder to their DM. With the --version output (see DuskStudioApp),
// that's enough to triage 90% of reports without back-and-forth.
void install (const std::string& appVersion);

// Detach the logger at shutdown, retaining its storage for in-flight logging.
// The crash callback stays registered. Idempotent.
void uninstall();
} // namespace duskstudio::crash_handler
