#include "IpcProcess.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <sys/prctl.h>
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
                              std::string& errorOut) noexcept
{
    pid = ::fork();
    if (pid < 0)
    {
        errorOut = std::string ("fork failed: ") + std::strerror (errno);
        return false;
    }

    if (pid == 0)
    {
        if (! moveHandleToFd (childChannelEnd, kChildInheritFd))
            ::_exit (127);

        // Best-effort: kill the child if the parent dies. Cannot fail
        // in a way the child can report; ignore the return value.
        (void) ::prctl (PR_SET_PDEATHSIG, SIGTERM);

        std::vector<const char*> argv;
        argv.reserve (args.size() + 2);
        argv.push_back (executablePath.c_str());
        for (const auto& a : args) argv.push_back (a.c_str());
        argv.push_back (nullptr);

        ::execv (executablePath.c_str(), const_cast<char* const*> (argv.data()));
        std::fprintf (stderr, "[dusk-studio-plugin-host] execv failed: %s\n",
                       std::strerror (errno));
        ::_exit (127);
    }

    closeHandle (childChannelEnd);
    alive = true;
    return true;
}

bool ChildProcess::pollExit() noexcept
{
    if (pid <= 0) return false;
    int status = 0;
    const pid_t r = ::waitpid (pid, &status, WNOHANG);
    if (r == 0) return false;
    if (r < 0)  return false;
    pid   = -1;
    alive = false;
    return true;
}

void ChildProcess::terminate (int graceMs) noexcept
{
    if (pid <= 0) { alive = false; return; }

    ::kill (pid, SIGTERM);

    const int slices = (graceMs > 0 ? graceMs : 1) / 10;
    for (int i = 0; i < slices; ++i)
    {
        int status = 0;
        const pid_t r = ::waitpid (pid, &status, WNOHANG);
        if (r == pid) { pid = -1; alive = false; return; }
        ::usleep (10000);
    }

    if (pid > 0)
    {
        ::kill (pid, SIGKILL);
        int status = 0;
        ::waitpid (pid, &status, 0);
        pid = -1;
        alive = false;
    }
}

} // namespace duskstudio::ipc::platform
