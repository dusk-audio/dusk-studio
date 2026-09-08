#include <catch2/catch_test_macros.hpp>
#include <juce_core/juce_core.h>

#include "util/HostInfo.h"

using namespace duskstudio::diagnostics;

TEST_CASE ("Host diagnostics preserve supplied values and explicit units", "[issue-313][host-info]")
{
    REQUIRE (formatHostInfo ({ "Test OS 1.2", "Test CPU", 24, 32768 })
        == "OS:           Test OS 1.2\nCPU:          Test CPU (24 logical cores)\nRAM:          32768 MiB\n\n");
}

TEST_CASE ("Unavailable host probes stay identifiable in crash diagnostics", "[issue-313][host-info]")
{
    REQUIRE (formatHostInfo ({})
        == "OS:           unavailable\nCPU:          unavailable (unavailable logical cores)\nRAM:          unavailable\n\n");
}

TEST_CASE ("Native host probes retain current platform diagnostic facts", "[issue-313][host-info]")
{
    const auto info = collectHostInfo();
    INFO (formatHostInfo (info));
    REQUIRE_FALSE (info.operatingSystem.empty());
    REQUIRE (info.cpuModel == juce::SystemStats::getCpuModel().toStdString());
    REQUIRE (info.logicalCpus == (unsigned int) juce::SystemStats::getNumCpus());
   #if defined(_WIN32)
    // The prior provider added one MiB unconditionally; report the actual floor.
    REQUIRE (info.memoryMiB + 1 == (std::uint64_t) juce::SystemStats::getMemorySizeInMegabytes());
    REQUIRE (info.operatingSystem.find ("Windows ") == 0);
    REQUIRE (info.operatingSystem.find (" (build ") != std::string::npos);
   #else
    REQUIRE (info.memoryMiB == (std::uint64_t) juce::SystemStats::getMemorySizeInMegabytes());
    REQUIRE (info.operatingSystem == juce::SystemStats::getOperatingSystemName().toStdString());
   #endif
}
