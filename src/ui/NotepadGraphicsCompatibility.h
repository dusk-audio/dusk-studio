#pragma once

#include <cctype>
#include <string_view>

namespace duskstudio::notepad
{
enum class GraphicsCompatibility
{
    supported,
    noOpenGL3,
    unsafeMesaD3D12
};

inline GraphicsCompatibility assessGraphicsCompatibility (
    std::string_view version, std::string_view renderer) noexcept
{
    // GL_VERSION is "<major>.<minor>[ vendor text]", sometimes behind a
    // prefix ("OpenGL ES 3.2"). Read the whole major number rather than its
    // first digit, so a two-digit major does not read as a 1.x driver.
    const auto digit = version.find_first_of ("0123456789");
    if (digit == std::string_view::npos)
        return GraphicsCompatibility::noOpenGL3;
    int major = 0;
    for (auto i = digit; i < version.size()
                         && std::isdigit (static_cast<unsigned char> (version[i])); ++i)
        major = major * 10 + (version[i] - '0');
    if (major < 3)
        return GraphicsCompatibility::noOpenGL3;

    // Microsoft's OpenGL Compatibility Pack exposes Mesa's GL-on-D3D12
    // renderer. Its first presented ImGui frame has been observed to terminate
    // the whole host process, so capability/version checks alone are unsafe.
    // Native AMD, Intel and NVIDIA drivers do not identify their OpenGL
    // renderer as D3D12; llvmpipe identifies itself by name and remains usable.
    if (renderer.find ("D3D12") != std::string_view::npos)
        return GraphicsCompatibility::unsafeMesaD3D12;

    return GraphicsCompatibility::supported;
}
} // namespace duskstudio::notepad
