#pragma once

#include <cstdint>

namespace duskstudio::test
{
constexpr const char* kLv2UiLifetimeProbeUri = "urn:duskstudio:test:ui-lifetime";

struct Lv2UiLifetimeProbe
{
    std::uint64_t (*serial) (void* instance);
    void (*recordCleanup) (void* instance, std::uint64_t serial);
    std::uint64_t (*cleanupCount)();
    std::uint64_t (*staleCleanupCount)();
};
}
