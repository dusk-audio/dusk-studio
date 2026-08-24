#pragma once

#if defined (_WIN32) && defined (DUSKSTUDIO_USE_WINDOWS_SOFTWARE_OPENGL)

#include <cstdlib>
#include <windows.h>

namespace duskstudio::platform
{
// The Windows MSI installs Mesa's opengl32.dll beside each executable. Its
// default D3D12 Gallium backend is unsafe on the basic display adapter, so
// select llvmpipe before any framework code creates a GL context. Update both
// the CRT view used by getenv() and the Win32 process environment inherited by
// plugin-host children; these can be separate stores under the MSVC runtime.
struct ForcePackagedSoftwareOpenGL
{
    ForcePackagedSoftwareOpenGL()
    {
        crtUpdated = ::_wputenv_s (L"GALLIUM_DRIVER", L"llvmpipe") == 0;
        processUpdated = ::SetEnvironmentVariableW (L"GALLIUM_DRIVER", L"llvmpipe") != FALSE;
    }

    bool succeeded() const noexcept { return crtUpdated && processUpdated; }

private:
    bool crtUpdated = false;
    bool processUpdated = false;
};
} // namespace duskstudio::platform

#endif
