#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>

#include "ImGuiGlLoaderWin32.h"

#include <cstdio>

namespace duskstudio::glloader {

ProcPtr procAddress(const char* const name)
{
    const auto address = ::wglGetProcAddress(name);
    if (isInvalidWglProcAddressValue (reinterpret_cast<std::intptr_t> (address)))
    {
        char message[192] {};
        std::snprintf (message, sizeof message,
                       "[Dusk Studio/notepad] OpenGL entry point unavailable: %s\n",
                       name);
        std::fputs (message, stderr);
        ::OutputDebugStringA (message);
        return nullptr;
    }
    return reinterpret_cast<ProcPtr> (address);
}

}

#endif
