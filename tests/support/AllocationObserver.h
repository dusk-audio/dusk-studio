#pragma once

#include <cstddef>

namespace duskstudio::test
{
class AllocationObserver final
{
public:
    AllocationObserver() noexcept;
    ~AllocationObserver();
    AllocationObserver (const AllocationObserver&) = delete;
    AllocationObserver& operator= (const AllocationObserver&) = delete;

    std::size_t count() const noexcept { return allocations; }

private:
    friend void recordAllocation() noexcept;
    AllocationObserver* previous = nullptr;
    std::size_t allocations = 0;
};
} // namespace duskstudio::test
