#pragma once

#include <juce_core/juce_core.h>

namespace focal::crash_handler
{
// Install the global JUCE FileLogger + JUCE crash callback. Idempotent.
// Call once from FocalApp::initialise BEFORE any audio init so the
// first thing a crashing build does is leave a paste-able report at
// getCrashDir().
//
// Layout under ${userApplicationData}/Focal/:
//   log/focal-YYYYMMDD.log   — rotating daily, FileLogger
//   crashes/crash-<iso>.txt  — one per terminate; backtrace + env summary
//
// Patreon support flow: ask user to attach the most recent file from
// each folder to their DM. With the --version output (see FocalApp),
// that's enough to triage 90% of reports without back-and-forth.
void install (const juce::String& appVersion);

// Detach + delete the FileLogger so JUCE's leak detector stays quiet at
// shutdown. The crash callback registered via SystemStats stays put
// (process is exiting). Idempotent.
void uninstall();

juce::File getLogDir();
juce::File getCrashDir();
juce::File getCurrentLogFile();
} // namespace focal::crash_handler
