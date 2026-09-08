#pragma once

#include <cstdint>
#include <string>

namespace duskstudio::diagnostics
{
struct HostInfo
{
    std::string operatingSystem;
    std::string cpuModel;
    unsigned int logicalCpus = 0;
    std::uint64_t memoryMiB = 0;
};

// Blocking, non-RT probes. Capture and format before installing a crash callback.
// Empty strings and zero counts mean the platform query was unavailable.
HostInfo collectHostInfo();
std::string formatHostInfo (const HostInfo&);
} // namespace duskstudio::diagnostics
