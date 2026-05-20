#pragma once

#include "IpcChannel.h"

#include <string>
#include <vector>

// Spawn + supervise the dusk-studio-plugin-host child binary.
//
// Linux  : fork() + dup2(channel, kChildInheritFd) + prctl(PR_SET_PDEATHSIG)
//          + execv(). Reaping via waitpid(WNOHANG); termination via SIGTERM
//          then SIGKILL.
// macOS  : posix_spawn() with file actions to remap the channel endpoint
//          to kChildInheritFd. Parent-death tracking via kqueue NOTE_EXIT
//          on the child PID; termination via SIGTERM + waitpid.
// Windows: CreateProcess() with bInheritHandles=TRUE, the child handle
//          duplicated into the new process and recorded in
//          STARTUPINFOEX::lpAttributeList. Job object configured with
//          JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE so the child dies with us;
//          termination via TerminateProcess() + WaitForSingleObject.

namespace duskstudio::ipc::platform
{

class ChildProcess
{
public:
    ChildProcess() = default;
    ~ChildProcess();

    ChildProcess (const ChildProcess&) = delete;
    ChildProcess& operator= (const ChildProcess&) = delete;

    // Spawn `executablePath` with `args` (the executable path itself is
    // argv[0]; pass only the trailing flags in `args`). The child end
    // of `channelToInherit` is duped to kChildInheritFd in the child
    // process; the parent end stays with the caller.
    //
    // On success the child's end of the channel is closed in the parent
    // (the child sees the dup'd copy at kChildInheritFd) and the
    // ChildProcess takes ownership of supervising the spawn.
    bool spawn (const std::string& executablePath,
                  const std::vector<std::string>& args,
                  NativeHandle& childChannelEnd,
                  std::string& errorOut) noexcept;

    // Non-blocking check. Returns true if the child has exited.
    // Idempotent — once reaped, subsequent calls return false (the pid
    // is cleared).
    bool pollExit() noexcept;

    // SIGTERM, then up to `graceMs` for the child to exit cleanly, then
    // SIGKILL. Idempotent. On Windows: TerminateProcess after the grace.
    void terminate (int graceMs) noexcept;

    bool isAlive() const noexcept { return alive; }

private:
    int  pid   { -1 };
    bool alive { false };
};

} // namespace duskstudio::ipc::platform
