#pragma once

namespace duskstudio::imgui
{
constexpr bool requiresScaleRecreation (double applied, double requested) noexcept
{
    return applied < requested || requested < applied;
}
} // namespace duskstudio::imgui
