#include <catch2/catch_session.hpp>

#include "engine/sfz/SfzTransfer.h"

#include <cstdio>
#include <cstdlib>

#if defined(CATCH_CONFIG_WCHAR) && defined(_WIN32) && defined(_UNICODE) && ! defined(DO_NOT_USE_WMAIN)
extern "C" int __cdecl wmain (int argc, wchar_t* argv[], wchar_t* [])
#else
int main (int argc, char* argv[])
#endif
{
    if (! duskstudio::sfz::initializeHttpTransport())
    {
        std::fputs ("Cannot initialize the SFZ HTTPS transport\n", stderr);
        return EXIT_FAILURE;
    }
    return Catch::Session().run (argc, argv);
}
