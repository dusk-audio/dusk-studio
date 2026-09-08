#include "LogFile.h"

#include <atomic>
#include <chrono>
#include <fstream>
#include <limits>
#include <string>
#include <thread>
#include <utility>

namespace duskstudio::diagnostics
{
namespace
{
class TrimDirectory
{
public:
    explicit TrimDirectory (const std::filesystem::path& parent)
    {
        static std::atomic<unsigned long long> sequence { 0 };
        const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
        for (int attempt = 0; attempt < 128; ++attempt)
        {
            const auto candidate = parent / (".dusk-log-trim-" + std::to_string (tick) + "-"
                                             + std::to_string (sequence.fetch_add (1, std::memory_order_relaxed)));
            std::error_code error;
            if (std::filesystem::create_directory (candidate, error))
            {
                path = candidate;
                break;
            }
            if (error) break;
        }
    }

    ~TrimDirectory()
    {
        if (path.empty()) return;
        std::error_code error;
        std::filesystem::remove (path / "log", error);
        std::filesystem::remove (path, error);
    }

    std::filesystem::path path;
};
} // namespace

LogFile::LogFile (std::filesystem::path file) : path (std::move (file)) {}

bool LogFile::prepare()
{
    const std::lock_guard<std::mutex> lock (mutex);
    if (path.empty() || path.filename().empty()) return false;
    std::error_code error;
    const auto parent = path.has_parent_path() ? path.parent_path() : std::filesystem::path (".");
    std::filesystem::create_directories (parent, error);
    if (error) return false;

    const auto fileStatus = std::filesystem::status (path, error);
    if (error) return error == std::errc::no_such_file_or_directory;
    if (! std::filesystem::exists (fileStatus)) return true;
    if (! std::filesystem::is_regular_file (fileStatus)) return false;

    const auto size = std::filesystem::file_size (path, error);
    if (error) return false;
    if (size <= kMaxInitialBytes) return true;
    if (size > static_cast<std::uintmax_t> (std::numeric_limits<std::streamoff>::max())) return false;

    std::string tail (kMaxInitialBytes, '\0');
    {
        std::ifstream input (path, std::ios::binary);
        if (! input) return false;
        input.seekg (static_cast<std::streamoff> (size - kMaxInitialBytes));
        input.read (tail.data(), static_cast<std::streamsize> (tail.size()));
        if (input.gcount() != static_cast<std::streamsize> (tail.size())) return false;
    }
    const auto boundary = tail.find_first_of ("\r\n");
    // Keep an unbroken or binary fragment intact, matching existing startup
    // trimming. A later append supplies a boundary for the next startup.
    if (boundary == std::string::npos || tail.find ('\0') < boundary) return true;

    const TrimDirectory temporary (parent);
    if (temporary.path.empty()) return false;
    const auto replacement = temporary.path / "log";
    {
        std::ofstream output (replacement, std::ios::binary | std::ios::trunc);
        if (! output) return false;
        output.write (tail.data() + boundary, static_cast<std::streamsize> (tail.size() - boundary));
        output.close();
        if (! output) return false;
    }
    // Match startup's existing retry window when another process briefly
    // holds the destination open (notably Windows file scanners).
    for (int attempt = 0; attempt < 5; ++attempt)
    {
        std::filesystem::rename (replacement, path, error);
        if (! error) return true;
        std::this_thread::sleep_for (std::chrono::milliseconds (100));
    }
    return false;
}

bool LogFile::append (std::string_view message)
{
    const std::lock_guard<std::mutex> lock (mutex);
    if (path.empty() || message.size() > static_cast<std::size_t> (std::numeric_limits<std::streamsize>::max()))
        return false;
    std::ofstream output (path, std::ios::binary | std::ios::app);
    if (! output) return false;
    if (! message.empty()) output.write (message.data(), static_cast<std::streamsize> (message.size()));
    output.write ("\r\n", 2);
    output.close();
    return static_cast<bool> (output);
}
} // namespace duskstudio::diagnostics
