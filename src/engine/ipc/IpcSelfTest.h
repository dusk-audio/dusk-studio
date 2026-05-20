#pragma once

#include <string>

namespace duskstudio::ipc
{
// Parent-side IPC selftest. Forks the dusk-studio-plugin-host child in
// --ipc-stub mode, runs `iterations` round-trips of `numSamples`-sample
// stereo blocks, verifies output == input, prints latency stats to
// stdout, returns 0 on pass / non-zero on fail.
//
// `hostExecutablePath` should be the path to the dusk-studio-plugin-host
// binary. The Dusk Studio binary's sibling file is the production answer;
// the test passes the path explicitly so it can also run against a
// non-installed build.
//
// Triggered from DuskStudioApp's main via DUSKSTUDIO_RUN_IPC_SELFTEST=1.
int runIpcSelfTest (const std::string& hostExecutablePath,
                     int iterations = 10000,
                     int numSamples = 64);

// Phase 2 acceptance gate. Connects to dusk-studio-plugin-host in --ipc-host
// mode, scans + loads the VST3 at `pluginPath`, runs `iterations`
// process-block round-trips with deterministic input, asserts the
// plugin actually changed the signal (output != input for a non-trivial
// effect), reports latency stats. Returns 0 on pass.
int runIpcHostTest (const std::string& hostExecutablePath,
                     const std::string& pluginPath,
                     int iterations = 1000,
                     int numSamples = 64);
} // namespace duskstudio::ipc
