#include "IpcProcess.h"
#include "IpcProcess_Posix.h"

#include <csignal>
#include <sys/wait.h>
#include <unistd.h>

namespace duskstudio::ipc::platform
{

ChildProcess::~ChildProcess()
{
    if (alive) terminate (500);
}

bool ChildProcess::spawn (const std::string& executablePath,
                              const std::vector<std::string>& args,
                              NativeHandle& childChannelEnd,
                              const NativeHandle& parentChannelEnd,
                              const std::vector<NativeHandle>&,
                              std::string& errorOut) noexcept
{
    auto childArgs = args;
    childArgs.push_back ("--ipc-parent-pid=" + std::to_string ((long long) ::getpid()));

    pid_t spawned = -1;
    if (! detail::posixSpawn (executablePath, childArgs, childChannelEnd,
                              parentChannelEnd,
                              spawned, errorOut))
        return false;

    pid = (std::intptr_t) spawned;
    closeHandle (childChannelEnd);
    alive = true;
    return true;
}

bool ChildProcess::pollExit() noexcept
{
    if (pid <= 0) return false;
    int status = 0;
    const pid_t r = ::waitpid ((pid_t) pid, &status, WNOHANG);
    if (r == 0) return false;
    if (r < 0)  return false;
    pid   = -1;
    alive = false;
    return true;
}

void ChildProcess::terminate (int graceMs) noexcept
{
    if (pid <= 0) { alive = false; return; }

    ::kill ((pid_t) pid, SIGTERM);

    const int slices = (graceMs > 0 ? graceMs : 1) / 10;
    for (int i = 0; i < slices; ++i)
    {
        int status = 0;
        const pid_t r = ::waitpid ((pid_t) pid, &status, WNOHANG);
        if (r == (pid_t) pid) { pid = -1; alive = false; return; }
        ::usleep (10000);
    }

    if (pid > 0)
    {
        ::kill ((pid_t) pid, SIGKILL);
        int status = 0;
        ::waitpid ((pid_t) pid, &status, 0);
        pid = -1;
        alive = false;
    }
}

} // namespace duskstudio::ipc::platform
