#pragma once

#include "IpcChannel.h"

#include <cerrno>
#include <cstring>
#include <signal.h>
#include <spawn.h>
#include <string>
#include <unistd.h>
#include <vector>

extern "C" char** environ;

namespace duskstudio::ipc::platform::detail
{

inline bool posixSpawn (const std::string& executablePath,
                        const std::vector<std::string>& args,
                        const NativeHandle& childChannelEnd,
                        const NativeHandle& parentChannelEnd,
                        pid_t& childPidOut,
                        std::string& errorOut) noexcept
{
    if (! isValid (childChannelEnd) || ! isValid (parentChannelEnd))
    {
        errorOut = "posix_spawn received an invalid channel";
        return false;
    }

    posix_spawn_file_actions_t actions;
    int result = ::posix_spawn_file_actions_init (&actions);
    if (result != 0)
    {
        errorOut = std::string ("posix_spawn file-actions init failed: ")
            + std::strerror (result);
        return false;
    }

    // The channel is created CLOEXEC, so the descriptor the child inherits has
    // to be one the dup2 action re-creates. A dup2 action whose source already
    // equals its target is specified to clear FD_CLOEXEC in place, but Darwin
    // has not always honoured that, and the child would then exec with no
    // channel at all. Duplicating first (dup never carries FD_CLOEXEC over)
    // keeps the source and target distinct on every platform.
    int inheritSource = childChannelEnd.fd;
    int duplicatedSource = -1;
    if (inheritSource == kChildInheritFd)
    {
        duplicatedSource = ::dup (inheritSource);
        if (duplicatedSource < 0)
        {
            ::posix_spawn_file_actions_destroy (&actions);
            errorOut = std::string ("could not duplicate the child channel: ")
                + std::strerror (errno);
            return false;
        }
        inheritSource = duplicatedSource;
    }

    const auto closeDuplicatedSource = [&duplicatedSource]
    {
        if (duplicatedSource >= 0)
        {
            ::close (duplicatedSource);
            duplicatedSource = -1;
        }
    };

    result = ::posix_spawn_file_actions_addclose (&actions, parentChannelEnd.fd);
    if (result == 0)
        result = ::posix_spawn_file_actions_adddup2 (
            &actions, inheritSource, kChildInheritFd);
    if (result == 0 && inheritSource != kChildInheritFd)
        result = ::posix_spawn_file_actions_addclose (&actions, inheritSource);

    if (result != 0)
    {
        ::posix_spawn_file_actions_destroy (&actions);
        closeDuplicatedSource();
        errorOut = std::string ("posix_spawn file action failed: ")
            + std::strerror (result);
        return false;
    }

    posix_spawnattr_t attributes;
    result = ::posix_spawnattr_init (&attributes);
    if (result != 0)
    {
        ::posix_spawn_file_actions_destroy (&actions);
        closeDuplicatedSource();
        errorOut = std::string ("posix_spawn attributes init failed: ")
            + std::strerror (result);
        return false;
    }

    sigset_t emptyMask;
    sigset_t defaultSignals;
    (void) sigemptyset (&emptyMask);
    (void) sigfillset (&defaultSignals);
    (void) sigdelset (&defaultSignals, SIGKILL);
    (void) sigdelset (&defaultSignals, SIGSTOP);

    result = ::posix_spawnattr_setsigmask (&attributes, &emptyMask);
    if (result == 0)
        result = ::posix_spawnattr_setsigdefault (&attributes, &defaultSignals);
    if (result == 0)
    {
        constexpr short flags = POSIX_SPAWN_SETSIGMASK | POSIX_SPAWN_SETSIGDEF;
        result = ::posix_spawnattr_setflags (&attributes, flags);
    }

    if (result != 0)
    {
        ::posix_spawnattr_destroy (&attributes);
        ::posix_spawn_file_actions_destroy (&actions);
        closeDuplicatedSource();
        errorOut = std::string ("posix_spawn attribute setup failed: ")
            + std::strerror (result);
        return false;
    }

    std::vector<char*> argv;
    argv.reserve (args.size() + 2);
    argv.push_back (const_cast<char*> (executablePath.c_str()));
    for (const auto& arg : args)
        argv.push_back (const_cast<char*> (arg.c_str()));
    argv.push_back (nullptr);

    result = ::posix_spawn (&childPidOut, executablePath.c_str(), &actions,
                            &attributes, argv.data(), environ);
    ::posix_spawnattr_destroy (&attributes);
    ::posix_spawn_file_actions_destroy (&actions);
    closeDuplicatedSource();
    if (result != 0)
    {
        errorOut = std::string ("posix_spawn failed: ") + std::strerror (result);
        return false;
    }
    return true;
}

} // namespace duskstudio::ipc::platform::detail
