#pragma once

// Windows' GL header and opengl32 stop at OpenGL 1.1, so every GL 3.x entry point Dear
// ImGui's OpenGL3 backend calls is undeclared and unlinkable there. DPF-Widgets compiles
// that backend with IMGUI_IMPL_OPENGL_LOADER_CUSTOM (opengl/DearImGui.cpp) and points it at
// DGL's OpenGL.hpp, which carries the prototypes on Linux and macOS only.
//
// Resolve the entry points the way DGL resolves its own (dgl/src/OpenGL3.cpp), except on
// first use: this translation unit is reached from the render path, not from context
// creation, so there is no one place to load them from.
//
// Force-included into the DearImGui.cpp compile - see the notepad block in CMakeLists.txt.

#if defined(_WIN32)

#include "OpenGL-include.hpp"
#include "WglProcAddressSentinel.h"

#include <utility>

namespace duskstudio::glloader {

using ProcPtr = void (*)();

ProcPtr procAddress(const char* name);

template <typename Fn>
struct GlProc
{
    const char* const name;
    Fn fn = nullptr;
    bool resolutionAttempted = false;

    template <typename... Args>
    auto operator()(Args... args) -> decltype(std::declval<Fn>()(args...))
    {
        if (! resolutionAttempted)
        {
            resolutionAttempted = true;
            fn = reinterpret_cast<Fn>(procAddress(name));
        }

        // An unresolved entry point costs the drawing; calling through the null pointer
        // would cost the app.
        if (fn == nullptr)
            return decltype(std::declval<Fn>()(args...))();

        return fn(args...);
    }
};

}

#define DUSK_GL_PROC(type, name) inline duskstudio::glloader::GlProc<type> name { #name };

DUSK_GL_PROC(PFNGLACTIVETEXTUREPROC,            glActiveTexture)
DUSK_GL_PROC(PFNGLATTACHSHADERPROC,             glAttachShader)
DUSK_GL_PROC(PFNGLBINDBUFFERPROC,               glBindBuffer)
DUSK_GL_PROC(PFNGLBINDSAMPLERPROC,              glBindSampler)
DUSK_GL_PROC(PFNGLBINDVERTEXARRAYPROC,          glBindVertexArray)
DUSK_GL_PROC(PFNGLBLENDEQUATIONPROC,            glBlendEquation)
DUSK_GL_PROC(PFNGLBLENDEQUATIONSEPARATEPROC,    glBlendEquationSeparate)
DUSK_GL_PROC(PFNGLBLENDFUNCSEPARATEPROC,        glBlendFuncSeparate)
DUSK_GL_PROC(PFNGLBUFFERDATAPROC,               glBufferData)
DUSK_GL_PROC(PFNGLBUFFERSUBDATAPROC,            glBufferSubData)
DUSK_GL_PROC(PFNGLCOMPILESHADERPROC,            glCompileShader)
DUSK_GL_PROC(PFNGLCREATEPROGRAMPROC,            glCreateProgram)
DUSK_GL_PROC(PFNGLCREATESHADERPROC,             glCreateShader)
DUSK_GL_PROC(PFNGLDELETEBUFFERSPROC,            glDeleteBuffers)
DUSK_GL_PROC(PFNGLDELETEPROGRAMPROC,            glDeleteProgram)
DUSK_GL_PROC(PFNGLDELETESHADERPROC,             glDeleteShader)
DUSK_GL_PROC(PFNGLDELETEVERTEXARRAYSPROC,       glDeleteVertexArrays)
DUSK_GL_PROC(PFNGLDETACHSHADERPROC,             glDetachShader)
DUSK_GL_PROC(PFNGLDRAWELEMENTSBASEVERTEXPROC,   glDrawElementsBaseVertex)
DUSK_GL_PROC(PFNGLENABLEVERTEXATTRIBARRAYPROC,  glEnableVertexAttribArray)
DUSK_GL_PROC(PFNGLGENBUFFERSPROC,               glGenBuffers)
DUSK_GL_PROC(PFNGLGENVERTEXARRAYSPROC,          glGenVertexArrays)
DUSK_GL_PROC(PFNGLGETATTRIBLOCATIONPROC,        glGetAttribLocation)
DUSK_GL_PROC(PFNGLGETPROGRAMINFOLOGPROC,        glGetProgramInfoLog)
DUSK_GL_PROC(PFNGLGETPROGRAMIVPROC,             glGetProgramiv)
DUSK_GL_PROC(PFNGLGETSHADERINFOLOGPROC,         glGetShaderInfoLog)
DUSK_GL_PROC(PFNGLGETSHADERIVPROC,              glGetShaderiv)
DUSK_GL_PROC(PFNGLGETSTRINGIPROC,               glGetStringi)
DUSK_GL_PROC(PFNGLGETUNIFORMLOCATIONPROC,       glGetUniformLocation)
DUSK_GL_PROC(PFNGLISPROGRAMPROC,                glIsProgram)
DUSK_GL_PROC(PFNGLLINKPROGRAMPROC,              glLinkProgram)
DUSK_GL_PROC(PFNGLSHADERSOURCEPROC,             glShaderSource)
DUSK_GL_PROC(PFNGLUNIFORM1IPROC,                glUniform1i)
DUSK_GL_PROC(PFNGLUNIFORMMATRIX4FVPROC,         glUniformMatrix4fv)
DUSK_GL_PROC(PFNGLUSEPROGRAMPROC,               glUseProgram)
DUSK_GL_PROC(PFNGLVERTEXATTRIBPOINTERPROC,      glVertexAttribPointer)

#undef DUSK_GL_PROC

#endif
