#pragma once

// Real-time checking with Clang's RealtimeSanitizer (RTSan,
// -DDUSKSTUDIO_ENABLE_RTSAN=ON). A function declared DUSK_RT_NONBLOCKING puts
// its thread in a real-time context for the length of the call, and in an RTSan
// build any allocation, lock, blocking syscall or file I/O made under it is
// reported. GCC and MSVC see nothing; Clang without the sanitizer sees the
// attribute only.
//
// The attribute goes after the parameter list and any noexcept, before
// override, on the declaration:
//
//     void process (int n) noexcept DUSK_RT_NONBLOCKING;
//     void callback (int n) DUSK_RT_NONBLOCKING override;

#if defined(__clang__) && defined(__has_cpp_attribute)
 #if __has_cpp_attribute(clang::nonblocking)
  #define DUSK_RT_NONBLOCKING [[clang::nonblocking]]
 #endif
#endif
#ifndef DUSK_RT_NONBLOCKING
 #define DUSK_RT_NONBLOCKING
#endif

// DUSK_RT_SANITIZER is defined in an RTSan build, for code whose wall-time
// checks the sanitizer's own work would trip.
#if defined(__has_feature)
 #if __has_feature(realtime_sanitizer)
  #define DUSK_RT_SANITIZER 1
  #include <sanitizer/rtsan_interface.h>
 #endif
#endif

namespace dusk::rt
{
// Lets the enclosing scope block on a real-time thread without an RTSan report.
// Only for a wait the engine documents as deliberate; the reason says which, so
// every exemption reads as one line where it is taken. Real-time functions
// called inside the scope are not checked either, so the scope holds the
// blocking call and nothing else.
class ScopedBlockingAllowed
{
public:
   #if defined(DUSK_RT_SANITIZER)
    explicit ScopedBlockingAllowed (const char*) noexcept { __rtsan_disable(); }
    ~ScopedBlockingAllowed() { __rtsan_enable(); }
   #else
    explicit ScopedBlockingAllowed (const char*) noexcept {}
    ~ScopedBlockingAllowed() {}
   #endif

    ScopedBlockingAllowed (const ScopedBlockingAllowed&) = delete;
    ScopedBlockingAllowed& operator= (const ScopedBlockingAllowed&) = delete;
};
} // namespace dusk::rt
