#pragma once

#include "Fs.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>
#include <utility>

#if ! defined(_WIN32)
 #include <cstdio>
 #include <fcntl.h>
 #include <unistd.h>
#endif

namespace dusk::fs
{
namespace detail
{
inline std::string atomicTemporaryName()
{
    static std::atomic<std::uint64_t> sequence { 0 };
#if defined(_WIN32)
    const auto process = std::to_string ((unsigned long) GetCurrentProcessId());
#else
    const auto process = std::to_string ((long) ::getpid());
#endif
    return ".dusk-" + process + "-" + std::to_string (sequence.fetch_add (1, std::memory_order_relaxed)) + ".tmp";
}

#if ! defined(_WIN32)
// What a file system answers when it cannot do a kind of sync at all, rather
// than failing to.
inline bool syncUnsupported (int code) noexcept
{
   #if defined(EOPNOTSUPP) && EOPNOTSUPP != ENOTSUP
    if (code == EOPNOTSUPP) return true;
   #endif
    return code == EINVAL || code == ENOTSUP;
}

// On macOS fsync leaves the bytes in the drive's own cache, which a power loss
// can still take; F_FULLFSYNC asks the drive to write them out. Only a file
// system that cannot do it gets the plain fsync instead: any other failure is
// the write's own and is reported.
inline int fullSync (int handle) noexcept
{
   #if defined(__APPLE__) && defined(F_FULLFSYNC)
    int flushed;
    do { flushed = ::fcntl (handle, F_FULLFSYNC); } while (flushed != 0 && errno == EINTR);
    if (flushed == 0) return 0;
    if (! syncUnsupported (errno) && errno != ENOTTY) return flushed;
   #endif
    int synced;
    do { synced = ::fsync (handle); } while (synced != 0 && errno == EINTR);
    return synced;
}
#endif
} // namespace detail

// Brings what has been written to `file` to the disk. False, with `error`
// saying why, when it cannot be opened or synced; nothing is thrown.
inline bool syncFile (const std::filesystem::path& file, std::error_code& error) noexcept
{
    error.clear();
#if defined(_WIN32)
    try
    {
        // FlushFileBuffers needs a handle that may write.
        const auto native = nativeIOPath (file).native();
        const HANDLE handle = CreateFileW (native.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
                                           OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (handle == INVALID_HANDLE_VALUE)
        {
            error = { static_cast<int> (GetLastError()), std::system_category() };
            return false;
        }
        const bool flushed = FlushFileBuffers (handle) != 0;
        if (! flushed) error = { static_cast<int> (GetLastError()), std::system_category() };
        CloseHandle (handle);
        return flushed;
    }
    catch (...)
    {
        error = std::make_error_code (std::errc::not_enough_memory);
        return false;
    }
#else
    const int handle = ::open (file.c_str(), O_RDONLY | O_CLOEXEC);
    if (handle < 0)
    {
        error = { errno, std::generic_category() };
        return false;
    }
    const int synced = detail::fullSync (handle);
    if (synced != 0) error = { errno, std::generic_category() };
    ::close (handle);
    return synced == 0;
#endif
}

// On POSIX, brings the names in `folder` to the disk, so a file made, renamed
// or removed there lasts; a file system that cannot sync a folder at all counts
// as done. Nothing to do on Windows, whose NTFS has no folder flush.
inline bool syncFolder (const std::filesystem::path& folder, std::error_code& error) noexcept
{
    error.clear();
#if defined(_WIN32)
    (void) folder;
    return true;
#else
    const int handle = ::open (folder.c_str(), O_RDONLY | O_DIRECTORY | O_CLOEXEC);
    if (handle < 0)
    {
        error = { errno, std::generic_category() };
        return false;
    }
    const int synced = detail::fullSync (handle);
    const int syncError = synced != 0 ? errno : 0;
    ::close (handle);
    if (syncError != 0 && ! detail::syncUnsupported (syncError))
    {
        error = { syncError, std::generic_category() };
        return false;
    }
    return true;
#endif
}

// Replaces `target` with `size` bytes so that a reader, or the next start after
// a crash or a power loss, finds the old file or the whole new one and never
// part of either: the bytes go to a new file beside the target, reach the disk,
// and are renamed over it, and on POSIX the folder is synced so the rename
// lasts too. The folder must exist.
//
// False, with `error` saying why, when any step fails; nothing is thrown, and
// the temporary file is removed. The target then holds the old file, unless
// only the final folder sync failed: the new file is in place, but a power loss
// could still take the rename back.
inline bool writeFileAtomically (const std::filesystem::path& target, const void* data, std::size_t size,
                                 std::error_code& error) noexcept
{
    error.clear();
    const auto* bytes = static_cast<const char*> (data);
    static constexpr std::size_t kChunk = std::size_t { 1 } << 30;
    static constexpr int kNameAttempts = 64;

#if defined(_WIN32)
    std::wstring temporary;
    HANDLE handle = INVALID_HANDLE_VALUE;
    const auto fail = [&] (std::error_code why) noexcept
    {
        if (handle != INVALID_HANDLE_VALUE) CloseHandle (handle);
        if (! temporary.empty()) DeleteFileW (temporary.c_str());
        error = why;
        return false;
    };
    const auto lastError = [] { return std::error_code (static_cast<int> (GetLastError()), std::system_category()); };

    try
    {
        const auto folder = target.has_parent_path() ? target.parent_path() : std::filesystem::path (".");
        for (int attempt = 1; handle == INVALID_HANDLE_VALUE; ++attempt)
        {
            auto candidate = nativeIOPath (folder / detail::atomicTemporaryName()).native();
            handle = CreateFileW (candidate.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle != INVALID_HANDLE_VALUE)
            {
                temporary = std::move (candidate);
                break;
            }
            const auto code = GetLastError();
            if ((code != ERROR_FILE_EXISTS && code != ERROR_ALREADY_EXISTS) || attempt == kNameAttempts)
                return fail ({ static_cast<int> (code), std::system_category() });
        }

        for (std::size_t done = 0; done < size;)
        {
            DWORD written = 0;
            if (! WriteFile (handle, bytes + done, static_cast<DWORD> ((std::min) (size - done, kChunk)), &written, nullptr))
                return fail (lastError());
            if (written == 0) return fail (std::make_error_code (std::errc::io_error));
            done += written;
        }
        if (! FlushFileBuffers (handle)) return fail (lastError());
        const bool closed = CloseHandle (handle) != 0;
        handle = INVALID_HANDLE_VALUE;
        if (! closed) return fail (lastError());

        // Write-through carries the rename to the disk; NTFS has no folder flush.
        const auto destination = nativeIOPath (target).native();
        if (! MoveFileExW (temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            return fail (lastError());
        return true;
    }
    catch (const std::filesystem::filesystem_error& failure) { return fail (failure.code()); }
    catch (...) { return fail (std::make_error_code (std::errc::not_enough_memory)); }
#else
    std::filesystem::path temporary;
    int handle = -1;
    const auto fail = [&] (std::error_code why) noexcept
    {
        if (handle >= 0) ::close (handle);
        if (! temporary.empty()) ::unlink (temporary.c_str());
        error = why;
        return false;
    };
    const auto lastError = [] { return std::error_code (errno, std::generic_category()); };

    try
    {
        const auto folder = target.has_parent_path() ? target.parent_path() : std::filesystem::path (".");
        for (int attempt = 1; handle < 0; ++attempt)
        {
            auto candidate = folder / detail::atomicTemporaryName();
            handle = ::open (candidate.c_str(), O_WRONLY | O_CREAT | O_EXCL | O_CLOEXEC, 0666);
            if (handle >= 0)
            {
                temporary = std::move (candidate);
                break;
            }
            if (errno != EEXIST || attempt == kNameAttempts) return fail (lastError());
        }

        for (std::size_t done = 0; done < size;)
        {
            const auto written = ::write (handle, bytes + done, std::min (size - done, kChunk));
            if (written < 0 && errno == EINTR) continue;
            if (written < 0) return fail (lastError());
            if (written == 0) return fail (std::make_error_code (std::errc::io_error));
            done += static_cast<std::size_t> (written);
        }
        if (detail::fullSync (handle) != 0) return fail (lastError());
        const bool closed = ::close (handle) == 0;
        handle = -1;
        if (! closed) return fail (lastError());

        if (std::rename (temporary.c_str(), target.c_str()) != 0) return fail (lastError());
        temporary.clear();

        std::error_code folderError;
        if (! syncFolder (folder, folderError)) return fail (folderError);
        return true;
    }
    catch (const std::filesystem::filesystem_error& failure) { return fail (failure.code()); }
    catch (...) { return fail (std::make_error_code (std::errc::not_enough_memory)); }
#endif
}
} // namespace dusk::fs
