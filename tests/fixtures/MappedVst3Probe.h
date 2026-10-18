#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace duskstudio::test::mappedvst3
{
constexpr const char* kName = "Dusk Mapped Panic Probe Fixture";
constexpr int kChannels = 16;
enum Controller : std::size_t { Modulation, Sustain, AllSoundsOff, ResetControllers, AllNotesOff, PitchBend, Pressure };
constexpr std::array<int, 7> kControllers { 1, 64, 120, 121, 123, 129, 128 };
constexpr std::uint32_t kParameterBase = 1000;
constexpr std::uint32_t parameterId (int channel, Controller controller) noexcept
{
    return kParameterBase + (std::uint32_t) ((std::size_t) channel * kControllers.size() + controller);
}
enum Diagnostic : std::uint32_t
{
    HeldNotes = 200, NoteOffsSeen, PointsSeen, QueueErrors,
    ModulationPoints, ModulationOffset, PitchPoints, PitchOffset, PressurePoints, PressureOffset
};
constexpr double kCounterScale = 65535.0;
} // namespace duskstudio::test::mappedvst3
