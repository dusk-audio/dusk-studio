#include "IpcProcess.h"

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

// CreateProcessW with bInheritHandles = TRUE and an explicit
// PROC_THREAD_ATTRIBUTE_HANDLE_LIST. Only the channel child end and the
// caller-approved IPC handles flow into the child, at the same numeric HANDLE
// values. A Job object with
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

bool widenUtf8 (const std::string& utf8, std::wstring& wide,
                const char* description, std::string& errorOut)
{
    if (utf8.find ('\0') != std::string::npos
        || utf8.size() > (std::size_t) std::numeric_limits<int>::max())
    {
        errorOut = std::string (description) + " is not a valid Windows argument";
        return false;
    }

    if (utf8.empty())
    {
        wide.clear();
        return true;
    }

    const int sourceLength = (int) utf8.size();
    const int required = ::MultiByteToWideChar (
        CP_UTF8, MB_ERR_INVALID_CHARS, utf8.data(), sourceLength, nullptr, 0);
    if (required <= 0)
    {
        errorOut = std::string (description) + " is not valid UTF-8";
        return false;
    }

    wide.resize ((std::size_t) required);
    if (::MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS,
                               utf8.data(), sourceLength,
                               wide.data(), required) != required)
    {
        errorOut = std::string ("failed to convert ") + description + " to UTF-16";
        return false;
    }
    return true;
}

// Quote one argv element using the escaping rules consumed by
// CommandLineToArgvW and the Microsoft C runtime. Backslashes are doubled only
// when they precede a quote or the closing quote; elsewhere they remain literal.
void appendQuotedArgument (std::wstring& commandLine, const std::wstring& argument)
{
    commandLine.push_back (L'"');
    std::size_t backslashes = 0;
    for (const wchar_t ch : argument)
    {
        if (ch == L'\\')
        {
            ++backslashes;
            continue;
        }

        if (ch == L'"')
        {
            commandLine.append (backslashes * 2 + 1, L'\\');
            commandLine.push_back (L'"');
        }
        else
        {
            commandLine.append (backslashes, L'\\');
            commandLine.push_back (ch);
        }
        backslashes = 0;
    }

    commandLine.append (backslashes * 2, L'\\');
    commandLine.push_back (L'"');
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
                              const NativeHandle&,
                              const std::vector<NativeHandle>& additionalInheritedHandles,
                              std::string& errorOut) noexcept
{
    std::vector<HANDLE> inheritedHandles;
    inheritedHandles.reserve (additionalInheritedHandles.size() + 1);
    const auto addInheritedHandle = [&] (const NativeHandle& native)
    {
        const auto handle = reinterpret_cast<HANDLE> (native.h);
        if (handle == nullptr || handle == INVALID_HANDLE_VALUE)
            return false;
        if (std::find (inheritedHandles.begin(), inheritedHandles.end(), handle)
            == inheritedHandles.end())
            inheritedHandles.push_back (handle);
        return true;
    };

    if (! addInheritedHandle (childChannelEnd))
    {
        errorOut = "child channel handle is invalid";
        return false;
    }
    for (const auto& handle : additionalInheritedHandles)
    {
        if (! addInheritedHandle (handle))
        {
            errorOut = "additional child handle is invalid";
            return false;
        }
    }

    for (const auto handle : inheritedHandles)
    {
        DWORD flags = 0;
        if (! ::GetHandleInformation (handle, &flags)
            || (flags & HANDLE_FLAG_INHERIT) == 0)
        {
            errorOut = "child handle is not inheritable";
            return false;
        }
    }

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

    std::vector<std::string> childArgs = args;
    {
        // Pass the allowlisted channel HANDLE value on the command line so the
        // child can locate it at the numeric value preserved by inheritance.
        char buf[64];
        std::snprintf (buf, sizeof (buf), "--ipc-channel=0x%llx",
                        (unsigned long long) (std::uintptr_t)
                            reinterpret_cast<HANDLE> (childChannelEnd.h));
        childArgs.emplace_back (buf);
    }

    std::wstring wideExecutable;
    if (! widenUtf8 (executablePath, wideExecutable, "executable path", errorOut))
    {
        ::CloseHandle (state->job);
        delete state;
        return false;
    }

    std::wstring commandLine;
    appendQuotedArgument (commandLine, wideExecutable);
    for (const auto& argument : childArgs)
    {
        std::wstring wideArgument;
        if (! widenUtf8 (argument, wideArgument, "process argument", errorOut))
        {
            ::CloseHandle (state->job);
            delete state;
            return false;
        }
        commandLine.push_back (L' ');
        appendQuotedArgument (commandLine, wideArgument);
    }
    std::vector<wchar_t> commandBuffer (commandLine.begin(), commandLine.end());
    commandBuffer.push_back (L'\0');

    SIZE_T attributeBytes = 0;
    (void) ::InitializeProcThreadAttributeList (nullptr, 1, 0, &attributeBytes);
    if (attributeBytes == 0)
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "InitializeProcThreadAttributeList sizing failed: %lu",
            (unsigned long) ::GetLastError());
        errorOut = buf;
        ::CloseHandle (state->job);
        delete state;
        return false;
    }

    std::vector<unsigned char> attributeStorage (attributeBytes);
    auto* attributeList = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST> (
        attributeStorage.data());
    if (! ::InitializeProcThreadAttributeList (attributeList, 1, 0, &attributeBytes))
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "InitializeProcThreadAttributeList failed: %lu",
            (unsigned long) ::GetLastError());
        errorOut = buf;
        ::CloseHandle (state->job);
        delete state;
        return false;
    }

    if (! ::UpdateProcThreadAttribute (
            attributeList, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
            inheritedHandles.data(), inheritedHandles.size() * sizeof (HANDLE),
            nullptr, nullptr))
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "UpdateProcThreadAttribute failed: %lu", (unsigned long) ::GetLastError());
        errorOut = buf;
        ::DeleteProcThreadAttributeList (attributeList);
        ::CloseHandle (state->job);
        delete state;
        return false;
    }

    STARTUPINFOEXW si {};
    si.StartupInfo.cb = sizeof (si);
    si.lpAttributeList = attributeList;
    PROCESS_INFORMATION pi {};

    const BOOL ok = ::CreateProcessW (
        wideExecutable.c_str(),
        commandBuffer.data(),
        nullptr, nullptr,
        TRUE,                       // bInheritHandles
        CREATE_SUSPENDED | CREATE_NO_WINDOW | EXTENDED_STARTUPINFO_PRESENT,
        nullptr, nullptr,
        &si.StartupInfo, &pi);
    const DWORD createError = ok ? ERROR_SUCCESS : ::GetLastError();
    ::DeleteProcThreadAttributeList (attributeList);

    DWORD clearInheritError = ERROR_SUCCESS;
    for (const auto handle : inheritedHandles)
    {
        if (! ::SetHandleInformation (handle, HANDLE_FLAG_INHERIT, 0)
            && clearInheritError == ERROR_SUCCESS)
            clearInheritError = ::GetLastError();
    }

    if (! ok)
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "CreateProcess failed: %lu", (unsigned long) createError);
        errorOut = buf;
        ::CloseHandle (state->job);
        delete state;
        return false;
    }

    if (clearInheritError != ERROR_SUCCESS)
    {
        char buf[128]; std::snprintf (buf, sizeof (buf),
            "failed to clear child handle inheritance: %lu",
            (unsigned long) clearInheritError);
        errorOut = buf;
        ::TerminateProcess (pi.hProcess, 1);
        ::CloseHandle (pi.hProcess);
        ::CloseHandle (pi.hThread);
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
