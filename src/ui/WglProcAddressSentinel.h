#pragma once

#include <cstdint>

namespace duskstudio::glloader {

// Some Windows OpenGL drivers report lookup failure with small non-null
// sentinel values instead of the null pointer documented by Microsoft.
constexpr bool isInvalidWglProcAddressValue (std::intptr_t value) noexcept
{
    return value == 0 || value == 1 || value == 2 || value == 3 || value == -1;
}

}
