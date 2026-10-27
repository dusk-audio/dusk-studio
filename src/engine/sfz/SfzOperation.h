#pragma once

#include <atomic>
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
    // Borrowed flags outlive the operation and can be polled without callbacks.
    const std::atomic<bool>* stopFlag = nullptr;
    const std::atomic<bool>* cancelFlag = nullptr;
    const std::atomic<bool>* pauseFlag = nullptr;

    OperationStatus status() const
    {
        if ((stopFlag && stopFlag->load (std::memory_order_acquire))
            || (cancelFlag && cancelFlag->load (std::memory_order_acquire))) return OperationStatus::Cancelled;
        if (pauseFlag && pauseFlag->load (std::memory_order_acquire)) return OperationStatus::Paused;
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
