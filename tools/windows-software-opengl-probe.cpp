#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <GL/gl.h>

#include "platform/WindowsSoftwareOpenGL.h"

#include <cstdio>
#include <cstring>

namespace
{
const duskstudio::platform::ForcePackagedSoftwareOpenGL forcePackagedSoftwareOpenGL;
}

int main()
{
    if (! forcePackagedSoftwareOpenGL.succeeded())
    {
        std::fprintf (stderr, "could not select llvmpipe in the CRT and process environments\n");
        return 10;
    }

    WNDCLASSW windowClass {};
    windowClass.lpfnWndProc = ::DefWindowProcW;
    windowClass.hInstance = ::GetModuleHandleW (nullptr);
    windowClass.lpszClassName = L"DuskSoftwareOpenGLProbe";
    if (::RegisterClassW (&windowClass) == 0)
        return 11;

    const auto window = ::CreateWindowW (
        windowClass.lpszClassName, L"probe", WS_OVERLAPPED,
        0, 0, 16, 16, nullptr, nullptr, windowClass.hInstance, nullptr);
    if (window == nullptr)
        return 12;
    const auto dc = ::GetDC (window);
    if (dc == nullptr)
        return 13;

    PIXELFORMATDESCRIPTOR format {};
    format.nSize = sizeof format;
    format.nVersion = 1;
    format.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    format.iPixelType = PFD_TYPE_RGBA;
    format.cColorBits = 24;
    format.cDepthBits = 24;
    const auto pixelFormat = ::ChoosePixelFormat (dc, &format);
    if (pixelFormat == 0 || ! ::SetPixelFormat (dc, pixelFormat, &format))
        return 14;

    const auto context = ::wglCreateContext (dc);
    if (context == nullptr || ! ::wglMakeCurrent (dc, context))
        return 15;

    const auto* const renderer = reinterpret_cast<const char*> (::glGetString (GL_RENDERER));
    const auto* const version = reinterpret_cast<const char*> (::glGetString (GL_VERSION));
    std::printf ("renderer=%s\nversion=%s\n",
                 renderer != nullptr ? renderer : "(null)",
                 version != nullptr ? version : "(null)");
    return renderer != nullptr && std::strstr (renderer, "llvmpipe") != nullptr ? 0 : 16;
}
