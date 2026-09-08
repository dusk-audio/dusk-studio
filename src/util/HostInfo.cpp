#include "HostInfo.h"
#include "../foundation/Text.h"

#include <cstring>
#include <fstream>

#if defined(_WIN32)
 #ifndef NOMINMAX
  #define NOMINMAX
 #endif
 #include <windows.h>
 #include <winternl.h>
 #if defined(_M_IX86) || defined(_M_X64)
  #include <intrin.h>
 #endif
#elif defined(__APPLE__)
 #include <sys/sysctl.h>
#elif defined(__linux__)
 #include <sys/sysinfo.h>
 #include <unistd.h>
#endif

namespace duskstudio::diagnostics
{
namespace
{
#if defined(__APPLE__)
std::string sysctlString (const char* key)
{
    char buffer[256] {};
    auto size = sizeof (buffer) - 1;
    if (sysctlbyname (key, buffer, &size, nullptr, 0) != 0) return {};
    return dusk::text::trim (buffer);
}
#endif
}

HostInfo collectHostInfo()
{
    HostInfo info;
   #if defined(_WIN32)
    info.operatingSystem = "Windows";
    const auto module = GetModuleHandleW (L"ntdll.dll");
    const auto address = module ? GetProcAddress (module, "RtlGetVersion") : nullptr;
    using VersionQuery = LONG (WINAPI*) (PRTL_OSVERSIONINFOW);
    VersionQuery query = nullptr;
    static_assert (sizeof (query) == sizeof (address));
    std::memcpy (&query, &address, sizeof (query));
    RTL_OSVERSIONINFOW version {};
    version.dwOSVersionInfoSize = sizeof (version);
    if (query != nullptr && query (&version) == 0)
    {
        info.operatingSystem += " " + std::to_string (version.dwMajorVersion)
            + "." + std::to_string (version.dwMinorVersion)
            + " (build " + std::to_string (version.dwBuildNumber) + ")";
    }
    SYSTEM_INFO system {};
    GetNativeSystemInfo (&system);
    info.logicalCpus = system.dwNumberOfProcessors;
    MEMORYSTATUSEX memory {};
    memory.dwLength = sizeof (memory);
    if (GlobalMemoryStatusEx (&memory))
        info.memoryMiB = memory.ullTotalPhys / (1024 * 1024);
    #if defined(_M_IX86) || defined(_M_X64)
    int registers[4] {};
    __cpuid (registers, (int) 0x80000000u);
    if ((unsigned int) registers[0] >= 0x80000004u)
    {
        char model[49] {};
        for (unsigned int leaf = 0; leaf < 3; ++leaf)
        {
            __cpuid (registers, (int) (0x80000002u + leaf));
            std::memcpy (model + leaf * 16, registers, sizeof (registers));
        }
        info.cpuModel = dusk::text::trim (model);
    }
    #elif defined(_M_ARM) || defined(_M_ARM64)
    wchar_t model[256] {};
    DWORD modelBytes = sizeof (model);
    info.cpuModel = "Unknown Model";
    if (RegGetValueW (HKEY_LOCAL_MACHINE, L"HARDWARE\\DESCRIPTION\\System\\CentralProcessor\\0",
                      L"ProcessorNameString", RRF_RT_REG_SZ, nullptr, model, &modelBytes) == ERROR_SUCCESS)
    {
        char utf8[768] {};
        if (WideCharToMultiByte (CP_UTF8, 0, model, -1, utf8, sizeof (utf8), nullptr, nullptr) > 0)
        {
            const auto value = dusk::text::trim (utf8);
            if (! value.empty()) info.cpuModel = value;
        }
    }
    #endif
   #elif defined(__APPLE__)
    info.operatingSystem = "Mac OSX";
    if (const auto version = sysctlString ("kern.osproductversion"); ! version.empty())
        info.operatingSystem += " " + version;
    info.cpuModel = sysctlString ("machdep.cpu.brand_string");
    auto size = sizeof (info.logicalCpus);
    if (sysctlbyname ("hw.activecpu", &info.logicalCpus, &size, nullptr, 0) != 0)
        info.logicalCpus = 0;
    std::uint64_t bytes = 0;
    size = sizeof (bytes);
    if (sysctlbyname ("hw.memsize", &bytes, &size, nullptr, 0) == 0)
        info.memoryMiB = bytes / (1024 * 1024);
   #elif defined(__linux__)
    info.operatingSystem = "Linux";
    const auto cpus = sysconf (_SC_NPROCESSORS_ONLN);
    if (cpus > 0) info.logicalCpus = (unsigned int) cpus;
    struct sysinfo memory {};
    if (sysinfo (&memory) == 0)
        info.memoryMiB = (std::uint64_t) memory.totalram * memory.mem_unit / (1024 * 1024);
    std::ifstream cpuInfo ("/proc/cpuinfo");
    std::string line;
    while (std::getline (cpuInfo, line))
    {
        const auto colon = line.find (':');
        if (colon != std::string::npos && dusk::text::trim (std::string_view (line).substr (0, colon)) == "model name")
            info.cpuModel = dusk::text::trim (std::string_view (line).substr (colon + 1));
    }
   #endif
    return info;
}

std::string formatHostInfo (const HostInfo& info)
{
    const auto known = [] (const std::string& value) { return value.empty() ? "unavailable" : value; };
    const auto cpus = info.logicalCpus == 0 ? "unavailable" : std::to_string (info.logicalCpus);
    const auto memory = info.memoryMiB == 0 ? "unavailable" : std::to_string (info.memoryMiB) + " MiB";
    return "OS:           " + known (info.operatingSystem) + "\nCPU:          " + known (info.cpuModel)
        + " (" + cpus + " logical cores)\nRAM:          " + memory + "\n\n";
}
} // namespace duskstudio::diagnostics
