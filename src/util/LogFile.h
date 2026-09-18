#pragma once

#include <cstddef>
#include <filesystem>
#include <mutex>
#include <string_view>

namespace duskstudio::diagnostics
{
// Blocking, non-RT storage for the application log. The owner must outlive
// all callers; prepare once before publishing it to logging threads.
class LogFile
{
public:
    explicit LogFile (std::filesystem::path file);

    // Creates parents and trims an existing log at a line boundary. Failure
    // leaves the original file intact; append can still be attempted.
    bool prepare();
    bool append (std::string_view message);

    static constexpr std::size_t kMaxInitialBytes = 128 * 1024;

private:
    std::filesystem::path path;
    std::mutex mutex;
};
} // namespace duskstudio::diagnostics
