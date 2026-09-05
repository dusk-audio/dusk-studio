// The Windows IPC backend compiles only on Windows, so these read its source.
// Each invariant guards a transport failure that cannot be reproduced from a
// Linux or macOS test run: a stream left desynchronised by a stopped transfer,
// a pipe name a hostile local process can take first, a child whose
// diagnostics go nowhere, and an exit that is reaped forever.

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

TEST_CASE ("Windows IPC channel bounds both directions and poisons a half-moved frame",
           "[ipc][windows]")
{
    const auto channel = readSource ("src/engine/ipc/platform/IpcChannel_Windows.cpp");

    // CancelIoEx only cancels the chunk that is still pending. A deadline that
    // expires part-way through a multi-chunk frame, or a cancellation that
    // races an operation to completion, has already taken bytes off the pipe
    // that cannot be put back, so the endpoint has to fail every later
    // transfer instead of resuming inside a frame.
    REQUIRE (channel.find ("poisonChannel") != std::string::npos);
    REQUIRE (channel.find ("if (h.poisoned) return false;") != std::string::npos);

    // An unbounded write blocks the message thread with the control mutex held
    // for as long as the child leaves the pipe undrained.
    REQUIRE (channel.find ("kWriteTimeoutMs") != std::string::npos);
    REQUIRE (channel.find ("finishOverlapped (handle, operation, INFINITE")
             == std::string::npos);

    // A guessable name lets another local process create the pipe first, which
    // fails our CreateNamedPipe and silently downgrades to in-process hosting.
    REQUIRE (channel.find ("BCryptGenRandom") != std::string::npos);
    REQUIRE (channel.find ("FILE_FLAG_FIRST_PIPE_INSTANCE") != std::string::npos);

    const auto header = readSource ("src/engine/ipc/platform/IpcChannel.h");
    REQUIRE (header.find ("poisoned") != std::string::npos);
}

TEST_CASE ("Windows IPC spawn gives the child stderr and reaps its exit once",
           "[ipc][windows]")
{
    const auto process = readSource ("src/engine/ipc/platform/IpcProcess_Windows.cpp");

    // PROC_THREAD_ATTRIBUTE_HANDLE_LIST is exhaustive, so the std handles have
    // to be in it as well as in STARTUPINFO, or every diagnostic the child
    // prints on a failed handshake is discarded.
    REQUIRE (process.find ("STARTF_USESTDHANDLES") != std::string::npos);
    REQUIRE (process.find ("inheritableStdHandle") != std::string::npos);
    REQUIRE (process.find ("attributeHandles") != std::string::npos);

    // IpcProcess.h promises a one-shot reap. Leaving the process handle open
    // reports the same exit on every poll and re-runs crash handling with it.
    REQUIRE (process.find ("state->process = nullptr;") != std::string::npos);
}
