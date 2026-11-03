#pragma once

#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>

#include <fcntl.h>
#include <unistd.h>

namespace duskstudio::test
{
class ScopedEnvironmentVariable
{
public:
    ScopedEnvironmentVariable (std::string variable, const std::string& value)
        : name (std::move (variable))
    {
        if (const char* existing = std::getenv (name.c_str())) previous = existing;
        if (::setenv (name.c_str(), value.c_str(), 1) != 0) throw std::runtime_error ("setenv failed");
    }

    ~ScopedEnvironmentVariable()
    {
        if (previous) ::setenv (name.c_str(), previous->c_str(), 1);
        else ::unsetenv (name.c_str());
    }

    ScopedEnvironmentVariable (const ScopedEnvironmentVariable&) = delete;
    ScopedEnvironmentVariable& operator= (const ScopedEnvironmentVariable&) = delete;

private:
    std::string name;
    std::optional<std::string> previous;
};

// Captures what the call writes to file descriptor 2, which is where lilv
// reports its warnings. The descriptor is restored even if the call throws.
template <typename Fn>
std::string captureStderr (const std::filesystem::path& directory, Fn&& fn)
{
    const auto sinkPath = directory / "stderr.txt";
    const int saved = ::dup (STDERR_FILENO);
    if (saved < 0) throw std::runtime_error ("dup failed");
    const int sink = ::open (sinkPath.c_str(), O_WRONLY | O_CREAT | O_TRUNC, 0600);
    if (sink < 0) { ::close (saved); throw std::runtime_error ("could not open the stderr sink"); }
    std::fflush (stderr);
    const bool redirected = ::dup2 (sink, STDERR_FILENO) >= 0;
    ::close (sink);
    if (! redirected) { ::close (saved); throw std::runtime_error ("dup2 failed"); }
    struct Restore
    {
        int saved;
        ~Restore()
        {
            std::fflush (stderr);
            ::dup2 (saved, STDERR_FILENO);
            ::close (saved);
        }
    };
    {
        const Restore restore { saved };
        fn();
    }
    std::ifstream input (sinkPath);
    return { std::istreambuf_iterator<char> (input), std::istreambuf_iterator<char>() };
}
} // namespace duskstudio::test
