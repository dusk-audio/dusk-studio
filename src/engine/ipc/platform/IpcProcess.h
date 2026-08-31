#pragma once

#include "IpcChannel.h"

#include <cstdint>
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
// Windows: CreateProcessW() with bInheritHandles=TRUE and an explicit
//          PROC_THREAD_ATTRIBUTE_HANDLE_LIST containing only the channel and
//          caller-approved IPC handles. Job object configured with
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
    // of `childChannelEnd` is duped to kChildInheritFd in a POSIX child;
    // Windows inherits it plus `additionalInheritedHandles` through an
    // explicit handle allowlist. The parent end stays with the caller.
    //
    // On success the child's end of the channel is closed in the parent
    // (the child sees the dup'd copy at kChildInheritFd) and the
    // ChildProcess takes ownership of supervising the spawn.
    bool spawn (const std::string& executablePath,
                  const std::vector<std::string>& args,
                  NativeHandle& childChannelEnd,
                  const std::vector<NativeHandle>& additionalInheritedHandles,
                  std::string& errorOut) noexcept;

    // Non-blocking check. Returns true if the child has exited.
    // Idempotent - once reaped, subsequent calls return false (the pid
    // is cleared).
    bool pollExit() noexcept;

    // SIGTERM, then up to `graceMs` for the child to exit cleanly, then
    // SIGKILL. Idempotent. On Windows: TerminateProcess after the grace.
    void terminate (int graceMs) noexcept;

    bool isAlive() const noexcept { return alive; }

private:
    // intptr_t (not int) so the Windows impl can pack a heap pointer
    // to its WinProcessState block here without truncation on x64.
    // Linux stores the pid_t value directly; -1 = not spawned.
    std::intptr_t pid   { -1 };
    bool          alive { false };
};

} // namespace duskstudio::ipc::platform
