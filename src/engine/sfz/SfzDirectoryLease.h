#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <system_error>

namespace duskstudio::sfz
{
class DirectoryLease
{
public:
    enum class Access { Shared, Exclusive };

    DirectoryLease() noexcept = default;
    ~DirectoryLease();
    DirectoryLease (const DirectoryLease&) = delete;
    DirectoryLease& operator= (const DirectoryLease&) = delete;
    DirectoryLease (DirectoryLease&&) noexcept;
    DirectoryLease& operator= (DirectoryLease&&) noexcept;

    // Shared acquisition waits; exclusive maintenance returns immediately if an owner is active.
    // A busy exclusive request leaves error empty; filesystem failures supply an error.
    bool acquire (const std::filesystem::path&, Access, std::string& error,
                  std::error_code* nativeError = nullptr);
    explicit operator bool() const noexcept { return handle != -1; }

private:
    void release() noexcept;
    std::intptr_t handle = -1;
};
} // namespace duskstudio::sfz
