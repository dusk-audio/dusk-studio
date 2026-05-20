#include "IpcProcess.h"

#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <sys/wait.h>
#include <unistd.h>

// macOS has no prctl(PR_SET_PDEATHSIG). If the parent crashes without
// running its disconnect()/destructor, the child is orphaned and
// continues running until the user kills it. Acceptable trade-off
// for Phase 3 — the normal teardown path (parent's ~ChildProcess
// invokes terminate()) covers the common case. A future revision
// could add a kqueue(NOTE_EXIT) watcher thread in the child, or have
// the child poll getppid()==1 (reparent to launchd) periodically.

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
    const pid_t forked = ::fork();
    if (forked < 0)
    {
        errorOut = std::string ("fork failed: ") + std::strerror (errno);
        return false;
    }
    pid = (std::intptr_t) forked;

    if (forked == 0)
    {
        if (! moveHandleToFd (childChannelEnd, kChildInheritFd))
            ::_exit (127);

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
