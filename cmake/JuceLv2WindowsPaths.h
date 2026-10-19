#pragma once

#include "../src/foundation/Fs.h"

#include <direct.h>
#include <io.h>
#include <sys/stat.h>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace duskstudio::lv2WindowsPaths
{
template <typename Action, typename Result>
Result protect (Action action, Result failure) noexcept
{
    try { return action(); }
    catch (...)
    {
        errno = EINVAL;
        ::SetLastError (ERROR_NO_UNICODE_TRANSLATION);
        return failure;
    }
}

inline auto path (const char* text)
{
    return dusk::fs::nativeIOPath (std::filesystem::u8path (text != nullptr ? text : ""));
}

inline char* copy (const std::filesystem::path& value)
{
    const auto text = value.u8string();
    auto* result = static_cast<char*> (std::malloc (text.size() + 1));
    if (result != nullptr) std::memcpy (result, text.c_str(), text.size() + 1);
    return result;
}

inline FILE* openFile (const char* name, const char* mode) noexcept
{
    return protect ([&]
    {
        FILE* file = nullptr;
        const auto error = ::_wfopen_s (&file, path (name).c_str(), std::filesystem::u8path (mode).c_str());
        if (error != 0) errno = error;
        return file;
    }, static_cast<FILE*> (nullptr));
}
inline int pathAccess (const char* name, int mode) noexcept
{
    return protect ([&]
    {
        const auto error = ::_waccess_s (path (name).c_str(), mode);
        if (error == 0) return 0;
        errno = error;
        return -1;
    }, -1);
}
inline int makeDirectory (const char* name) noexcept
{
    return protect ([&] { return ::_wmkdir (path (name).c_str()); }, -1);
}
inline int removeFile (const char* name) noexcept
{
    return protect ([&] { return ::_wremove (path (name).c_str()); }, -1);
}
inline int fileInfo (const char* name, struct _stat64* result) noexcept
{
    return protect ([&] { return ::_wstat64 (path (name).c_str(), result); }, -1);
}
inline HMODULE loadLibrary (const char* name) noexcept
{
    return protect ([&] { return ::LoadLibraryW (path (name).c_str()); }, static_cast<HMODULE> (nullptr));
}
inline BOOL hardLink (const char* name, const char* existing, LPSECURITY_ATTRIBUTES attributes) noexcept
{
    return protect ([&] { return ::CreateHardLinkW (path (name).c_str(), path (existing).c_str(), attributes); }, FALSE);
}
inline BOOL removeDirectory (const char* name) noexcept
{
    return protect ([&] { return ::RemoveDirectoryW (path (name).c_str()); }, FALSE);
}
inline char* currentDirectory() noexcept
{
    return protect ([] { return copy (std::filesystem::current_path()); }, static_cast<char*> (nullptr));
}
inline char* temporaryDirectory() noexcept
{
    return protect ([] { return copy (std::filesystem::temp_directory_path()); }, static_cast<char*> (nullptr));
}
inline char* absolutePath (const char* name) noexcept
{
    return protect ([&] { return copy (std::filesystem::absolute (std::filesystem::u8path (name)).lexically_normal()); },
                    static_cast<char*> (nullptr));
}
inline char* expandEnvironment (const char* name) noexcept
{
    return protect ([&] () -> char*
    {
        const auto input = std::filesystem::u8path (name).native();
        DWORD size = ::ExpandEnvironmentStringsW (input.c_str(), nullptr, 0);
        while (size != 0)
        {
            std::vector<wchar_t> buffer (size);
            const auto written = ::ExpandEnvironmentStringsW (input.c_str(), buffer.data(), size);
            if (written == 0) return nullptr;
            if (written <= size) return copy (std::filesystem::path (buffer.data()));
            size = written;
        }
        return nullptr;
    }, static_cast<char*> (nullptr));
}
inline void forEachDirectory (const char* name, void* data,
                              void (*visit) (const char*, const char*, void*)) noexcept
{
    protect ([&]
    {
        std::error_code error;
        std::filesystem::directory_iterator iterator (path (name), error);
        dusk::fs::walkDirectoryEntries (iterator, error, [&] (const auto& entry)
        {
            const auto filename = entry.path().filename().u8string();
            visit (name, filename.c_str(), data);
            return true;
        });
        return true;
    }, false);
}
}
