#include <catch2/catch_test_macros.hpp>

#include "util/CrashHandler.h"

#include <cstdlib>

using duskstudio::crash_handler::envFlagSet;

namespace
{
// Not the real DUSKSTUDIO_SUPPORT_DIAGNOSTICS name: diagnosticsEnabled() caches
// its first read for the life of the process, so setting the real flag here
// would leak into whichever later test happens to log first.
constexpr const char* kFlag = "DUSKSTUDIO_TEST_ENV_FLAG";

void setFlag (const char* value)
{
   #if defined (_WIN32)
    _putenv_s (kFlag, value);
   #else
    ::setenv (kFlag, value, 1);
   #endif
}

void clearFlag()
{
   #if defined (_WIN32)
    // An empty value is how Windows deletes a variable, so there the empty
    // case and the unset case are the same environment.
    _putenv_s (kFlag, "");
   #else
    ::unsetenv (kFlag);
   #endif
}
} // namespace

TEST_CASE ("env flags are on only for a non-zero integer, \"true\" or \"yes\"",
           "[env][diagnostics]")
{
    struct Guard { ~Guard() { clearFlag(); } } guard;

    SECTION ("enabling values")
    {
        for (const char* value : { "1", "2", "-1", "true", "TRUE", "True", "yes", "YES" })
        {
            setFlag (value);
            INFO ("value=" << value);
            REQUIRE (envFlagSet (kFlag));
        }
    }

    SECTION ("everything else is off")
    {
        for (const char* value : { "0", "false", "FALSE", "no", "NO", "off", "" })
        {
            setFlag (value);
            INFO ("value=" << value);
            REQUIRE_FALSE (envFlagSet (kFlag));
        }
    }

    SECTION ("unset is off")
    {
        clearFlag();
        REQUIRE_FALSE (envFlagSet (kFlag));
    }
}
