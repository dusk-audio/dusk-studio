#pragma once

#include <cstdint>

// Shared between the state-probe VST3 fixture and the tests that drive it.
namespace duskstudio::test::stateprobevst3
{
constexpr std::uint32_t kGain = 1;
constexpr std::uint32_t kMeter = 2;
constexpr std::uint32_t kFlushes = 3;
constexpr std::uint32_t kQueueErrors = 4;
constexpr std::uint32_t kActivations = 5;
constexpr std::uint32_t kRefuseFlushes = 6;   // set: process() rejects a zero-sample call
constexpr std::uint32_t kUndeclared = 99;

constexpr double kDefaultGain = 0.5;
constexpr double kReportedGain = 0.75;
constexpr double kCounterScale = 1000.0;

// The first input sample selects what the processor reports for the block.
constexpr float kReportGain = 0.5f;
constexpr float kReportUndeclared = 0.25f;
constexpr float kReportEmptyQueue = 0.125f;
} // namespace duskstudio::test::stateprobevst3
