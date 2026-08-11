#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>

namespace duskstudio::sfz
{
struct TransferLimits
{
    // Ceiling on the complete resource, resume offset included. A response that
    // would push the total past it is aborted mid-stream rather than buffered.
    std::uint64_t maximumBytes { 0 };
    unsigned connectTimeoutSeconds { 20 };
    unsigned overallTimeoutSeconds { 3600 };
    // A transfer moving slower than stallBytesPerSecond for this long is a dead
    // connection; without it a silent peer holds the worker until the overall
    // timeout expires.
    unsigned stallTimeoutSeconds { 60 };
    unsigned stallBytesPerSecond { 512 };
    unsigned maximumRedirects { 5 };
};

struct TransferRequest
{
    std::string url;
    std::uint64_t resumeOffset { 0 };
    TransferLimits limits;
};

enum class TransferStatus
{
    completed,
    cancelled,
    failed
};

struct TransferResult
{
    TransferStatus status { TransferStatus::failed };
    // Bytes handed to onData during this transfer, excluding the resume offset.
    std::uint64_t receivedBytes { 0 };
    std::string error;
};

// Every callback runs on the thread that called fetch. Returning false from any
// of them stops the transfer and yields TransferStatus::cancelled, which is how
// both user cancellation and the activity gate unwind an in-flight download.
struct TransferCallbacks
{
    // Called once, before any data. resumeAccepted is false when a resume was
    // requested and the server answered with the whole resource anyway, so the
    // sink must discard what it already holds. totalBytes is the full resource
    // size when the response declares it, zero when it does not.
    std::function<bool (bool resumeAccepted, std::uint64_t totalBytes)> onResponse;
    std::function<bool (const unsigned char* data, std::size_t bytes)> onData;
    std::function<bool (std::uint64_t receivedBytes, std::uint64_t totalBytes)> onProgress;
};

// Seam between the install layer and the network. The libcurl implementation
// lives in its own translation unit so tests link a deterministic fake instead.
class Transport
{
public:
    virtual ~Transport() = default;
    virtual TransferResult fetch (const TransferRequest& request,
                                  const TransferCallbacks& callbacks) = 0;
};
} // namespace duskstudio::sfz
