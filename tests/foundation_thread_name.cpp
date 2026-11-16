#include <catch2/catch_test_macros.hpp>

#include "../src/foundation/ThreadName.h"

#include <string>

#if defined(__linux__) || defined(__APPLE__)
 #include <pthread.h>
#endif

TEST_CASE ("A thread name longer than Linux allows is kept to its first 15 bytes",
           "[foundation][thread-name]")
{
#if defined(__linux__) || defined(__APPLE__)
    char original[64] {};
    REQUIRE (pthread_getname_np (pthread_self(), original, sizeof original) == 0);

    dusk::setCurrentThreadName ("DuskThreadNameTest");
    char named[64] {};
    REQUIRE (pthread_getname_np (pthread_self(), named, sizeof named) == 0);
   #if defined(__linux__)
    CHECK (std::string (named) == "DuskThreadNameT");
   #else
    CHECK (std::string (named) == "DuskThreadNameTest");
   #endif

    dusk::setCurrentThreadName (original);
#else
    dusk::setCurrentThreadName ("DuskThreadNameTest");
    SUCCEED ("named through SetThreadDescription where the system has it");
#endif
}
