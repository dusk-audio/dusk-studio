// The macOS IPC backend compiles only on Apple, so these read its source
// instead. Both invariants sit on paths that cannot be exercised from a Linux
// or Windows test run, and both were regressions waiting to happen: an audio
// callback that drains a full wake pipe one read at a time, and a shared-memory
// name whose unique part is truncated away.

#include <catch2/catch_test_macros.hpp>

#include <fstream>
#include <iterator>
#include <string>

#ifndef DUSKSTUDIO_SOURCE_DIR
#define DUSKSTUDIO_SOURCE_DIR "."
#endif

namespace
{
std::string readSource (const char* relativePath)
{
    std::ifstream input (std::string (DUSKSTUDIO_SOURCE_DIR) + "/" + relativePath);
    REQUIRE (input.good());
    return { std::istreambuf_iterator<char> (input),
             std::istreambuf_iterator<char>() };
}
} // namespace

TEST_CASE ("macOS IPC backend bounds its audio-thread drain and keeps SHM names unique",
           "[ipc][mac]")
{
    const auto sync = readSource ("src/engine/ipc/platform/IpcSync_Mac.cpp");

    // processBlockSync usually settles in its spin phase without ever entering
    // wait(), so the child's per-block wake byte stays queued. At 64 samples and
    // 48 kHz the pipe fills in about a minute and a half, and the first block
    // that does have to wait pays for the whole backlog inside the audio
    // callback, exactly when the plugin is already late.
    REQUIRE (sync.find ("kMaxDrainReads") != std::string::npos);
    REQUIRE (sync.find ("kDrainChunkBytes") != std::string::npos);

    const auto shm = readSource ("src/engine/ipc/platform/IpcShm_Mac.cpp");

    // shm_open honours 31 characters. The old name spent all of them on a fixed
    // prefix, so the pid and counter that made it unique were cut off: every
    // connection asked for the same name, and O_EXCL turned a second concurrent
    // load, or one stale name left by a crash, into a failure to connect.
    REQUIRE (shm.find ("name[31] = '\\0'") == std::string::npos);
    REQUIRE (shm.find ("\"/ds.") != std::string::npos);
    REQUIRE (shm.find ("EEXIST") != std::string::npos);
}
