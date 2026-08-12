#if defined(_WIN32)

#ifndef WIN32_LEAN_AND_MEAN
# define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
# define NOMINMAX
#endif
#include <windows.h>

#include "ImGuiGlLoaderWin32.h"

namespace duskstudio::glloader {

ProcPtr procAddress(const char* const name)
{
    return reinterpret_cast<ProcPtr>(::wglGetProcAddress(name));
}

}

#endif
