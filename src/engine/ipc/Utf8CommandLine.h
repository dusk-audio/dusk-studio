#pragma once

#if defined (_WIN32)

 #ifndef WIN32_LEAN_AND_MEAN
  #define WIN32_LEAN_AND_MEAN
 #endif
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <shellapi.h>

 #include <string>
 #include <vector>

namespace duskstudio::ipc
{
// Windows hands a narrow main() an argv converted from the UTF-16 command line
// through the process ANSI code page, while the plugin host reads every
// argument back as UTF-8. A plugin under a path the code page cannot represent
// would therefore be scanned under a mangled name, find nothing, and report an
// empty-but-valid payload - the parent adds no plugins and shows no warning.
// Re-derive the arguments from the real command line the parent built.
class Utf8CommandLine
{
public:
    Utf8CommandLine() { adopt (GetCommandLineW()); }

    // Test seam: the same parse + conversion over a caller-supplied command
    // line, so the mangling this exists to prevent can be asserted on.
    explicit Utf8CommandLine (const wchar_t* commandLine) { adopt (commandLine); }

    int argc() const noexcept                { return (int) pointers.size(); }
    const char* const* argv() const noexcept { return pointers.data(); }

private:
    void adopt (const wchar_t* commandLine)
    {
        if (commandLine == nullptr) return;

        int count = 0;
        wchar_t** wide = CommandLineToArgvW (commandLine, &count);
        if (wide == nullptr) return;

        storage.reserve ((size_t) count);
        for (int i = 0; i < count; ++i)
            storage.push_back (toUtf8 (wide[i]));
        LocalFree (wide);

        // Only after the vector has stopped growing, or the pointers dangle.
        pointers.reserve (storage.size());
        for (const auto& arg : storage)
            pointers.push_back (arg.c_str());
    }

    static std::string toUtf8 (const wchar_t* wide)
    {
        const int bytes = WideCharToMultiByte (CP_UTF8, 0, wide, -1,
                                               nullptr, 0, nullptr, nullptr);
        if (bytes <= 1) return {};
        std::string out ((size_t) bytes, '\0');
        WideCharToMultiByte (CP_UTF8, 0, wide, -1, out.data(), bytes, nullptr, nullptr);
        out.pop_back();   // the API wrote its own terminator
        return out;
    }

    std::vector<std::string> storage;
    std::vector<const char*> pointers;
};
} // namespace duskstudio::ipc

#endif
