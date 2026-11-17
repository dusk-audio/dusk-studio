#include <catch2/catch_test_macros.hpp>

#include "foundation/RtAttributes.h"

#include <numeric>
#include <vector>

// The attribute sits where the engine puts it: after noexcept on a member,
// before override on a virtual. In an RTSan build the allocation below runs in
// a real-time context, and only the exemption keeps it from being reported.
namespace
{
struct Callback
{
    virtual ~Callback() = default;
    virtual int process (int n) = 0;
};

struct Engine final : Callback
{
    int process (int n) DUSK_RT_NONBLOCKING override { return lane (n); }

    int lane (int n) noexcept DUSK_RT_NONBLOCKING
    {
        const dusk::rt::ScopedBlockingAllowed allowed { "test: an exempted allocation" };
        std::vector<int> values ((std::size_t) n);
        std::iota (values.begin(), values.end(), 1);
        return std::accumulate (values.begin(), values.end(), 0);
    }
};
} // namespace

TEST_CASE ("RtAttributes: a real-time function runs an exempted scope", "[rt]")
{
    Engine engine;
    Callback& callback = engine;
    REQUIRE (callback.process (4) == 10);
    REQUIRE (callback.process (0) == 0);
}
