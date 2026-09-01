#pragma once

#include "IpcChannel.h"

#include <cstring>
#include <spawn.h>
#include <string>
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

    result = ::posix_spawn_file_actions_addclose (&actions, parentChannelEnd.fd);
    if (result == 0)
        result = ::posix_spawn_file_actions_adddup2 (
            &actions, childChannelEnd.fd, kChildInheritFd);
    if (result == 0 && childChannelEnd.fd != kChildInheritFd)
        result = ::posix_spawn_file_actions_addclose (&actions, childChannelEnd.fd);

    if (result != 0)
    {
        ::posix_spawn_file_actions_destroy (&actions);
        errorOut = std::string ("posix_spawn file action failed: ")
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
                            nullptr, argv.data(), environ);
    ::posix_spawn_file_actions_destroy (&actions);
    if (result != 0)
    {
        errorOut = std::string ("posix_spawn failed: ") + std::strerror (result);
        return false;
    }
    return true;
}

} // namespace duskstudio::ipc::platform::detail
