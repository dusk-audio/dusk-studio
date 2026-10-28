#pragma once

#include <cstdint>

// Shared between the state-probe LV2 fixture and the tests that drive it.
namespace duskstudio::test::stateprobelv2
{
constexpr const char* kPluginUri = "urn:duskstudio:test:state-probe";
constexpr const char* kLevelUri = "urn:duskstudio:test:state-probe#level";
constexpr const char* kAnnounceUri = "urn:duskstudio:test:state-probe#Announce";
constexpr const char* kWhatUri = "urn:duskstudio:test:state-probe#what";

constexpr std::uint32_t kGainPort = 4;
constexpr std::uint32_t kMeterPort = 5;
constexpr float kDefaultGain = 0.25f;
constexpr float kAnnouncedLevel = 0.9f;

// The Announce object's #what value: what the plug-in sends back.
enum Announcement : std::int32_t
{
    StateChanged = 1,
    ChangeLevel = 2,
    RepeatLevel = 3
};
} // namespace duskstudio::test::stateprobelv2
