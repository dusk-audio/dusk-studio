#pragma once

#include <cstdint>

// Shared between the many-parameters VST3 fixture and the test that drives it.
namespace duskstudio::test::manyparamsvst3
{
// More parameters than any fixed output-queue ceiling a host might pick.
constexpr std::uint32_t kParameterCount = 5000;
// Ids 0 .. kParameterCount - 1 are the reported parameters; this one counts the
// output queues the host refused.
constexpr std::uint32_t kRefusedQueues = kParameterCount;
constexpr double kCounterScale = 10000.0;

// A block whose first input sample is this reports every parameter at once, as
// a plug-in echoing its values after a preset load does.
constexpr float kReportAll = 0.5f;
constexpr double kReportedValue = 0.25;
} // namespace duskstudio::test::manyparamsvst3
