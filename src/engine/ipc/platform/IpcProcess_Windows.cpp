#include "IpcProcess.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

// CreateProcess with bInheritHandles = TRUE. Inheritable handles (the
// channel child end + the SHM mapping) flow into the child at the same
// numeric HANDLE value. A Job object with
// JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE wraps the child so it dies if the
// parent crashes or exits, mirroring Linux's prctl(PR_SET_PDEATHSIG).
//
// Phase 2 stores the Win32 process + thread HANDLEs in a small impl
// struct rather than extending NativeHandle, so the public header stays
// platform-clean. The Job HANDLE owns the kill-on-close semantics and
// is closed by the destructor.

namespace duskstudio::ipc::platform
{
namespace
{
struct WinProcessState
{
    HANDLE process { nullptr };
    HANDLE thread  { nullptr };
    HANDLE job     { nullptr };
};

WinProcessState* impl (std::intptr_t opaque) noexcept
{
    return reinterpret_cast<WinProcessState*> (opaque);
}
} // namespace

ChildProcess::~ChildProcess()
{
    if (alive) terminate (500);

    if (auto* s = impl (pid))
    {
        if (s->process != nullptr) ::CloseHandle (s->process);
        if (s->thread  != nullptr) ::CloseHandle (s->thread);
        if (s->job     != nullptr) ::CloseHandle (s->job);
        delete s;
        pid = -1;
    }
}

bool ChildProcess::spawn (const std::string& executablePath,
                              const std::vector<std::string>& args,
                              NativeHandle& childChannelEnd,
                              std::string& errorOut) noexcept
{
    auto* state = new WinProcessState;

    state->job = ::CreateJobObjectA (nullptr, nullptr);
    if (state->job == nullptr)
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "CreateJobObject failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        delete state;
        return false;
    }

    JOBOBJECT_EXTENDED_LIMIT_INFORMATION jobLimits {};
    jobLimits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (! ::SetInformationJobObject (state->job, JobObjectExtendedLimitInformation,
                                       &jobLimits, sizeof (jobLimits)))
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "SetInformationJobObject failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        ::CloseHandle (state->job);
        delete state;
        return false;
    }

    std::string cmdline = "\"" + executablePath + "\"";
    for (const auto& a : args)
    {
        cmdline += " \"";
        cmdline += a;
        cmdline += "\"";
    }
    {
        // Pass the inherited channel HANDLE value on the command line
        // so the child can locate it after CreateProcess(bInheritHandles
        // = TRUE) reproduces the same numeric value in its handle table.
        char buf[64];
        std::snprintf (buf, sizeof (buf), " --ipc-channel=0x%llx",
                        (unsigned long long) (std::uintptr_t)
                            reinterpret_cast<HANDLE> (childChannelEnd.h));
        cmdline += buf;
    }
    std::vector<char> cmdBuf (cmdline.begin(), cmdline.end());
    cmdBuf.push_back (0);

    STARTUPINFOA si {};
    si.cb = sizeof (si);
    PROCESS_INFORMATION pi {};

    const BOOL ok = ::CreateProcessA (
        executablePath.c_str(),
        cmdBuf.data(),
        nullptr, nullptr,
        TRUE,                       // bInheritHandles
        CREATE_SUSPENDED | CREATE_NO_WINDOW,
        nullptr, nullptr,
        &si, &pi);

    if (! ok)
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "CreateProcess failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        ::CloseHandle (state->job);
        delete state;
        return false;
    }

    if (! ::AssignProcessToJobObject (state->job, pi.hProcess))
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "AssignProcessToJobObject failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        ::TerminateProcess (pi.hProcess, 1);
        ::CloseHandle (pi.hProcess);
        ::CloseHandle (pi.hThread);
        ::CloseHandle (state->job);
        delete state;
        return false;
    }

    ::ResumeThread (pi.hThread);

    state->process = pi.hProcess;
    state->thread  = pi.hThread;

    pid = reinterpret_cast<std::intptr_t> (state);
    alive = true;

    closeHandle (childChannelEnd);
    return true;
}

bool ChildProcess::pollExit() noexcept
{
    auto* state = impl (pid);
    if (state == nullptr || state->process == nullptr) return false;

    const DWORD r = ::WaitForSingleObject (state->process, 0);
    if (r == WAIT_OBJECT_0)
    {
        alive = false;
        return true;
    }
    return false;
}

void ChildProcess::terminate (int graceMs) noexcept
{
    auto* state = impl (pid);
    if (state == nullptr || state->process == nullptr) { alive = false; return; }

    // Best-effort soft shutdown: closing the pipe end on the parent side
    // is what triggers the child's read loop to exit. The caller does
    // that via disconnect() before terminate() runs, so by the time we
    // get here the child is usually already exiting. Give it `graceMs`
    // then force-terminate.
    const DWORD r = ::WaitForSingleObject (state->process,
                                              graceMs > 0 ? (DWORD) graceMs : 0);
    if (r != WAIT_OBJECT_0)
        ::TerminateProcess (state->process, 1);

    ::WaitForSingleObject (state->process, INFINITE);
    alive = false;
}

} // namespace duskstudio::ipc::platform
