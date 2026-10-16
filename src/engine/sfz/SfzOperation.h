#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace duskstudio::sfz
{
enum class OperationStatus { Complete, Paused, Cancelled, Failed };
enum class FailureKind { None, DiskFull, Integrity };

struct OperationProgress
{
    std::uint64_t completedBytes = 0;
    std::uint64_t totalBytes = 0;
    std::uint64_t completedFiles = 0;
};

struct OperationControl
{
    std::function<bool()> cancelled;
    std::function<bool()> paused;
    std::function<void (const OperationProgress&)> progress;

    OperationStatus status() const
    {
        if (cancelled && cancelled()) return OperationStatus::Cancelled;
        if (paused && paused()) return OperationStatus::Paused;
        return OperationStatus::Complete;
    }
};

struct OperationResult
{
    OperationStatus status = OperationStatus::Failed;
    std::string error;
    FailureKind failureKind = FailureKind::None;
    explicit operator bool() const noexcept { return status == OperationStatus::Complete; }
};
} // namespace duskstudio::sfz
