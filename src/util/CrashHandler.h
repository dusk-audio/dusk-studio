#pragma once

#include <juce_core/juce_core.h>
#include <string>

namespace duskstudio::crash_handler
{
// Install the global JUCE FileLogger + JUCE crash callback. Idempotent.
// Call once from DuskStudioApp::initialise BEFORE any audio init so the
// first thing a crashing build does is leave a paste-able report at
// getCrashDir().
//
// Layout under ${userApplicationData}/Dusk Studio/:
//   log/dusk-studio-YYYYMMDD.log   - rotating daily, FileLogger
//   crashes/crash-<iso>.txt  - one per terminate; backtrace + env summary
//
// Patreon support flow: ask user to attach the most recent file from
// each folder to their DM. With the --version output (see DuskStudioApp),
// that's enough to triage 90% of reports without back-and-forth.
void install (const std::string& appVersion);

// Detach + delete the FileLogger so JUCE's leak detector stays quiet at
// shutdown. The crash callback registered via SystemStats stays put
// (process is exiting). Idempotent.
void uninstall();

// Truthiness for the DUSKSTUDIO_* env-flag convention: a non-zero integer,
// "true", or "yes" enables it; everything else - including "false", "no" and
// an empty value - leaves it off. Uncached, so tests can vary the environment.
bool envFlagSet (const char* name);

// Extra support logging is deliberately opt-in so normal sessions keep the
// existing log volume. Set DUSKSTUDIO_SUPPORT_DIAGNOSTICS=1 before launch.
bool diagnosticsEnabled();
void writeDiagnostics (const std::string& message);

juce::File getLogDir();
juce::File getCrashDir();
juce::File getCurrentLogFile();
} // namespace duskstudio::crash_handler
