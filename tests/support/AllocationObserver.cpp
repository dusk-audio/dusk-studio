#include "AllocationObserver.h"

#if defined(__SANITIZE_ADDRESS__)
 #define DUSK_TEST_ASAN_ALLOCATION_HOOK 1
#elif defined(__has_feature)
 #if __has_feature(address_sanitizer)
  #define DUSK_TEST_ASAN_ALLOCATION_HOOK 1
 #endif
#endif

#if ! defined(DUSK_TEST_ASAN_ALLOCATION_HOOK)
 #include <cstdlib>
 #include <new>
 #if defined(_WIN32)
  #include <malloc.h>
 #endif
#endif

namespace duskstudio::test
{
namespace
{
thread_local AllocationObserver* activeObserver = nullptr;
}

AllocationObserver::AllocationObserver() noexcept : previous (activeObserver)
{
    activeObserver = this;
}

AllocationObserver::~AllocationObserver()
{
    activeObserver = previous;
}

void recordAllocation() noexcept
{
    for (auto* observer = activeObserver; observer != nullptr; observer = observer->previous)
        ++observer->allocations;
}
} // namespace duskstudio::test

#if defined(DUSK_TEST_ASAN_ALLOCATION_HOOK)
// Keep the sanitizer's allocation-kind checks intact.
extern "C" void __sanitizer_malloc_hook (const volatile void*, std::size_t)
{
    duskstudio::test::recordAllocation();
}
#else
namespace
{
void* allocate (std::size_t size, std::size_t alignment = 0)
{
    duskstudio::test::recordAllocation();
    if (size == 0) size = 1;
    for (;;)
    {
        void* pointer = nullptr;
        if (alignment == 0) pointer = std::malloc (size);
        else
        {
           #if defined(_WIN32)
            pointer = _aligned_malloc (size, alignment);
           #else
            if (posix_memalign (&pointer, alignment, size) != 0) pointer = nullptr;
           #endif
        }
        if (pointer != nullptr) return pointer;
        const auto handler = std::get_new_handler();
        if (handler == nullptr) throw std::bad_alloc();
        handler();
    }
}

void* allocateNoThrow (std::size_t size, std::size_t alignment = 0) noexcept
{
    try { return allocate (size, alignment); }
    catch (...) { return nullptr; }
}

void freeAligned (void* pointer) noexcept
{
   #if defined(_WIN32)
    _aligned_free (pointer);
   #else
    std::free (pointer);
   #endif
}
}

void* operator new (std::size_t size) { return allocate (size); }
void* operator new[] (std::size_t size) { return allocate (size); }
void operator delete (void* pointer) noexcept { std::free (pointer); }
void operator delete[] (void* pointer) noexcept { std::free (pointer); }
void operator delete (void* pointer, std::size_t) noexcept { std::free (pointer); }
void operator delete[] (void* pointer, std::size_t) noexcept { std::free (pointer); }

void* operator new (std::size_t size, std::align_val_t alignment)
{ return allocate (size, static_cast<std::size_t> (alignment)); }
void* operator new[] (std::size_t size, std::align_val_t alignment)
{ return allocate (size, static_cast<std::size_t> (alignment)); }
void operator delete (void* pointer, std::align_val_t) noexcept { freeAligned (pointer); }
void operator delete[] (void* pointer, std::align_val_t) noexcept { freeAligned (pointer); }
void operator delete (void* pointer, std::size_t, std::align_val_t) noexcept { freeAligned (pointer); }
void operator delete[] (void* pointer, std::size_t, std::align_val_t) noexcept { freeAligned (pointer); }

void* operator new (std::size_t size, const std::nothrow_t&) noexcept { return allocateNoThrow (size); }
void* operator new[] (std::size_t size, const std::nothrow_t&) noexcept { return allocateNoThrow (size); }
void operator delete (void* pointer, const std::nothrow_t&) noexcept { std::free (pointer); }
void operator delete[] (void* pointer, const std::nothrow_t&) noexcept { std::free (pointer); }

void* operator new (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{ return allocateNoThrow (size, static_cast<std::size_t> (alignment)); }
void* operator new[] (std::size_t size, std::align_val_t alignment, const std::nothrow_t&) noexcept
{ return allocateNoThrow (size, static_cast<std::size_t> (alignment)); }
void operator delete (void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned (pointer); }
void operator delete[] (void* pointer, std::align_val_t, const std::nothrow_t&) noexcept { freeAligned (pointer); }
#endif
