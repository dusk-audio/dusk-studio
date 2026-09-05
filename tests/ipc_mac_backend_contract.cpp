// The macOS IPC backend compiles only on Apple, so these read its source
// instead. Every invariant here sits on a path that cannot be exercised from a
// Linux or Windows test run, and each was a regression waiting to happen: an
// audio callback that drains a full wake pipe one read at a time, a shared-
// memory name whose unique part is truncated away, and a parameter listener
// that locked and wrote a socket on the plugin's own audio thread.

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

std::string sectionBetween (const std::string& source,
                            const std::string& from,
                            const std::string& to)
{
    const auto begin = source.find (from);
    REQUIRE (begin != std::string::npos);
    const auto end = source.find (to, begin);
    REQUIRE (end != std::string::npos);
    return source.substr (begin, end - begin);
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

TEST_CASE ("macOS parameter mirror keeps the socket write off the plugin's threads",
           "[ipc][mac]")
{
    const auto host = readSource ("src/engine/ipc/PluginHostMain.cpp");

    const auto listener = sectionBetween (
        host,
        "void parameterValueChanged (int paramIndex, float newValue) override",
        "void parameterGestureChanged");

    // A parameter listener fires on whatever thread the plugin picked, which
    // for anything emitting automation per block is the child's audio worker
    // inside processBlock. Taking the channel write mutex or writing the
    // socket there stalls that worker while the parent's audio thread is
    // parked on replySeq, and the parent latches the missed deadline as a
    // sticky auto-bypass.
    REQUIRE (listener.find ("host.channel") == std::string::npos);
    REQUIRE (listener.find ("channelWriteMutex") == std::string::npos);
    REQUIRE (listener.find ("writeExact") == std::string::npos);
    REQUIRE (listener.find ("paramPushQueue.push") != std::string::npos);
    REQUIRE (listener.find ("outboundParamSeq") != std::string::npos);

    const auto drain = sectionBetween (host, "class ParamMirrorDrain", "HostState& host;");

    REQUIRE (drain.find ("public dusk::Timer") != std::string::npos);
    REQUIRE (drain.find ("paramPushQueue.pop") != std::string::npos);
    REQUIRE (drain.find ("writeParamChangedLocked") != std::string::npos);

    REQUIRE (host.find ("startTimerHz (kParamMirrorDrainHz)") != std::string::npos);

    // A value queued by the plugin being swapped out must not be forwarded
    // against its replacement's parameter of the same index, so the staleness
    // check has to sit inside the lock the LoadPlugin reply also takes.
    REQUIRE (host.find ("mirrorGeneration.fetch_add") != std::string::npos);
    const auto guard = sectionBetween (drain, "std::lock_guard", "writeParamChangedLocked");
    REQUIRE (guard.find ("queued.generation") != std::string::npos);
    REQUIRE (guard.find ("mirrorGeneration") != std::string::npos);
}
