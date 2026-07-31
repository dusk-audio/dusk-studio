#pragma once

#include <AudioToolbox/AudioToolbox.h>

#include <cstdint>
#include <string>
#include <vector>

namespace duskstudio::au
{
struct ComponentId
{
    std::uint32_t type = 0;
    std::uint32_t subtype = 0;
    std::uint32_t manufacturer = 0;

    std::string toString() const;
    static bool parse (const std::string& text, ComponentId& out) noexcept;

    bool operator== (const ComponentId& other) const noexcept
    {
        return type == other.type && subtype == other.subtype
            && manufacturer == other.manufacturer;
    }
};

struct PluginDesc
{
    ComponentId id;
    std::string name;
    std::string manufacturer;
    std::string version;
    std::string category;
    bool isInstrument = false;
};

// Audio Units are registered OS components rather than path-loaded bundles.
// AuBundle gives the shared NativeInsertSlot a bundle-shaped owner while keeping
// the stable identity to the component's type/subtype/manufacturer triple.
class AuBundle
{
public:
    AuBundle() = default;
    AuBundle (const AuBundle&) = delete;
    AuBundle& operator= (const AuBundle&) = delete;

    bool load (const std::string& identifier, std::string& errorOut);

    AudioComponent component() const noexcept { return audioComponent; }
    const std::vector<PluginDesc>& plugins() const noexcept { return descriptions; }

    static bool exists (const std::string& identifier) noexcept;
    static bool isSupportedType (std::uint32_t type) noexcept;
    static bool isInstrumentType (std::uint32_t type) noexcept;
    static std::string categoryForType (std::uint32_t type);
    static bool describe (AudioComponent component, PluginDesc& out) noexcept;
    static std::vector<PluginDesc> enumerate();

private:
    AudioComponent audioComponent = nullptr;
    std::vector<PluginDesc> descriptions;
};
} // namespace duskstudio::au
