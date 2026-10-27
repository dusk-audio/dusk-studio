#pragma once

#include <cstddef>
#include <memory>
#include <string>
#include <vector>

namespace duskstudio
{
class ScanProcess
{
public:
    enum class ReadStatus { Data, Waiting, Closed, Failed };
    struct ReadResult
    {
        ReadStatus status;
        std::size_t bytes = 0;
    };

    ScanProcess();
    ~ScanProcess();
    ScanProcess (const ScanProcess&) = delete;
    ScanProcess& operator= (const ScanProcess&) = delete;

    bool start (const std::vector<std::string>& arguments);
    ReadResult readOutput (void* buffer, std::size_t capacity, int waitMs = 5);
    void stop() noexcept;

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio
