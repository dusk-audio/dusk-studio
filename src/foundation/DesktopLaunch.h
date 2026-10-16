#pragma once

#include <algorithm>
#include <string>

#if defined(_WIN32)
 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shellapi.h>
#else
 #include <cerrno>
 #include <future>
 #include <spawn.h>
 #include <sys/wait.h>
 #include <thread>
 #include <unistd.h>
 #include <utility>
 #if defined(__APPLE__)
  #include <crt_externs.h>
 #endif
#endif

namespace dusk::desktop
{
inline bool openHttpsUrl (const std::string& url)
{
    if (url.substr (0, 8) != "https://" || url.size() > 8192
        || std::any_of (url.begin(), url.end(), [] (unsigned char c)
           { return c <= 0x20 || c == 0x7f || c == '\\'; })) return false;
    const auto authorityEnd = url.find_first_of ("/?#", 8);
    const auto authority = url.substr (8, authorityEnd == std::string::npos ? std::string::npos : authorityEnd - 8);
    if (authority.empty() || authority.find ('@') != std::string::npos) return false;
   #if defined(_WIN32)
    const auto count = MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, url.data(),
                                           static_cast<int> (url.size()), nullptr, 0);
    if (count <= 0) return false;
    std::wstring wide (static_cast<std::size_t> (count), L'\0');
    if (MultiByteToWideChar (CP_UTF8, MB_ERR_INVALID_CHARS, url.data(), static_cast<int> (url.size()),
                             wide.data(), count) != count) return false;
    const auto result = ShellExecuteW (nullptr, L"open", wide.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    return reinterpret_cast<INT_PTR> (result) > 32;
   #else
    try
    {
        // Arrange the reaper before spawning, so a failed thread allocation never leaves a child behind.
        std::promise<pid_t> spawned;
        auto process = spawned.get_future();
        std::thread reaper ([childProcess = std::move (process)]() mutable
        {
            try
            {
                const auto pid = childProcess.get();
                if (pid <= 0) return;
                int status = 0;
                while (::waitpid (pid, &status, 0) < 0 && errno == EINTR) {}
            }
            catch (...) {}
        });
        try { reaper.detach(); }
        catch (...)
        {
            spawned.set_value (-1);
            if (reaper.joinable()) reaper.join();
            return false;
        }
       #if defined(__APPLE__)
        const char* program = "/usr/bin/open";
        char** environment = *::_NSGetEnviron();
       #else
        const char* program = "xdg-open";
        char** environment = ::environ;
       #endif
        char* arguments[] = { const_cast<char*> (program), const_cast<char*> (url.c_str()), nullptr };
        pid_t pid = -1;
        const auto result = ::posix_spawnp (&pid, program, nullptr, nullptr, arguments, environment);
        spawned.set_value (result == 0 ? pid : -1);
        return result == 0;
    }
    catch (...) { return false; }
   #endif
}
} // namespace dusk::desktop
