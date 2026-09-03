#include "AuBundle.h"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdio>
#include <utility>

namespace duskstudio::au
{
namespace
{
constexpr const char* kIdentifierPrefix = "AudioUnit:";

std::string fourChar (std::uint32_t value)
{
    std::string out (4, ' ');
    out[0] = static_cast<char> ((value >> 24) & 0xffu);
    out[1] = static_cast<char> ((value >> 16) & 0xffu);
    out[2] = static_cast<char> ((value >> 8)  & 0xffu);
    out[3] = static_cast<char> ( value        & 0xffu);
    return out;
}

bool parseFourChar (const std::string& text, std::uint32_t& out) noexcept
{
    if (text.size() != 4) return false;
    out = (static_cast<std::uint32_t> (static_cast<unsigned char> (text[0])) << 24)
        | (static_cast<std::uint32_t> (static_cast<unsigned char> (text[1])) << 16)
        | (static_cast<std::uint32_t> (static_cast<unsigned char> (text[2])) << 8)
        |  static_cast<std::uint32_t> (static_cast<unsigned char> (text[3]));
    return true;
}

std::string cfString (CFStringRef value)
{
    if (value == nullptr) return {};
    const auto length = CFStringGetLength (value);
    const auto maximum = CFStringGetMaximumSizeForEncoding (length, kCFStringEncodingUTF8) + 1;
    if (maximum <= 1) return {};
    std::string out (static_cast<std::size_t> (maximum), '\0');
    if (! CFStringGetCString (value, out.data(), maximum, kCFStringEncodingUTF8))
        return {};
    out.resize (std::char_traits<char>::length (out.c_str()));
    return out;
}

std::string trim (std::string value)
{
    auto notSpace = [] (unsigned char c) { return ! std::isspace (c); };
    value.erase (value.begin(), std::find_if (value.begin(), value.end(), notSpace));
    value.erase (std::find_if (value.rbegin(), value.rend(), notSpace).base(), value.end());
    return value;
}

AudioComponentDescription componentDescription (const ComponentId& id) noexcept
{
    AudioComponentDescription result {};
    result.componentType = id.type;
    result.componentSubType = id.subtype;
    result.componentManufacturer = id.manufacturer;
    return result;
}
} // namespace

std::string ComponentId::toString() const
{
    return std::string (kIdentifierPrefix) + AuBundle::categoryForType (type) + "/"
        + fourChar (type) + "," + fourChar (subtype) + "," + fourChar (manufacturer);
}

bool ComponentId::parse (const std::string& text, ComponentId& out) noexcept
{
    if (text.rfind (kIdentifierPrefix, 0) != 0) return false;
    const auto slash = text.rfind ('/');
    const auto colon = text.rfind (':');
    const auto start = (slash != std::string::npos ? slash : colon) + 1;
    if (start == 0 || start >= text.size()) return false;

    const auto firstComma = text.find (',', start);
    const auto secondComma = firstComma == std::string::npos
        ? std::string::npos : text.find (',', firstComma + 1);
    if (firstComma == std::string::npos || secondComma == std::string::npos
        || text.find (',', secondComma + 1) != std::string::npos)
        return false;

    ComponentId parsed;
    if (! parseFourChar (text.substr (start, firstComma - start), parsed.type)
        || ! parseFourChar (text.substr (firstComma + 1, secondComma - firstComma - 1),
                            parsed.subtype)
        || ! parseFourChar (text.substr (secondComma + 1), parsed.manufacturer))
        return false;

    out = parsed;
    return true;
}

bool AuBundle::isSupportedType (std::uint32_t type) noexcept
{
    return type == kAudioUnitType_Effect
        || type == kAudioUnitType_MusicEffect
        || type == kAudioUnitType_MusicDevice;
}

bool AuBundle::isInstrumentType (std::uint32_t type) noexcept
{
    return type == kAudioUnitType_MusicDevice;
}

std::string AuBundle::categoryForType (std::uint32_t type)
{
    if (type == kAudioUnitType_MusicDevice) return "Synths";
    if (type == kAudioUnitType_Effect || type == kAudioUnitType_MusicEffect) return "Effects";
    if (type == kAudioUnitType_Generator) return "Generators";
    if (type == kAudioUnitType_Panner) return "Panners";
    if (type == kAudioUnitType_Mixer) return "Mixers";
    if (type == kAudioUnitType_MIDIProcessor) return "MidiEffects";
    return "Other";
}

bool AuBundle::describe (AudioComponent component, PluginDesc& out) noexcept
{
    if (component == nullptr) return false;
    AudioComponentDescription description {};
    if (AudioComponentGetDescription (component, &description) != noErr
        || ! isSupportedType (description.componentType))
        return false;

    PluginDesc result;
    result.id = { description.componentType, description.componentSubType,
                  description.componentManufacturer };
    result.category = categoryForType (description.componentType);
    result.isInstrument = isInstrumentType (description.componentType);

    CFStringRef copiedName = nullptr;
    if (AudioComponentCopyName (component, &copiedName) == noErr && copiedName != nullptr)
    {
        auto fullName = cfString (copiedName);
        CFRelease (copiedName);
        const auto separator = fullName.find (':');
        if (separator == std::string::npos)
            result.name = trim (std::move (fullName));
        else
        {
            result.manufacturer = trim (fullName.substr (0, separator));
            result.name = trim (fullName.substr (separator + 1));
        }
    }
    if (result.name.empty()) result.name = fourChar (description.componentSubType);
    if (result.manufacturer.empty()) result.manufacturer = fourChar (description.componentManufacturer);

    UInt32 version = 0;
    if (AudioComponentGetVersion (component, &version) == noErr)
    {
        char formatted[32] {};
        std::snprintf (formatted, sizeof formatted, "%u.%u.%u",
                       static_cast<unsigned> (version >> 16),
                       static_cast<unsigned> ((version >> 8) & 0xffu),
                       static_cast<unsigned> (version & 0xffu));
        result.version = formatted;
    }

    out = std::move (result);
    return true;
}

bool AuBundle::load (const std::string& identifier, std::string& errorOut)
{
    audioComponent = nullptr;
    descriptions.clear();

    ComponentId id;
    if (! ComponentId::parse (identifier, id))
    {
        errorOut = "invalid Audio Unit identifier";
        return false;
    }
    if (! isSupportedType (id.type))
    {
        errorOut = "unsupported Audio Unit component type";
        return false;
    }

    auto query = componentDescription (id);
    audioComponent = AudioComponentFindNext (nullptr, &query);
    if (audioComponent == nullptr)
    {
        errorOut = "Audio Unit is not registered";
        return false;
    }

    PluginDesc description;
    if (! describe (audioComponent, description))
    {
        audioComponent = nullptr;
        errorOut = "Audio Unit metadata is unavailable";
        return false;
    }
    descriptions.push_back (std::move (description));
    return true;
}

bool AuBundle::exists (const std::string& identifier) noexcept
{
    ComponentId id;
    if (! ComponentId::parse (identifier, id) || ! isSupportedType (id.type))
        return false;
    auto query = componentDescription (id);
    return AudioComponentFindNext (nullptr, &query) != nullptr;
}

std::vector<PluginDesc> AuBundle::enumerate (const std::atomic<bool>* abort)
{
    std::vector<PluginDesc> found;
    constexpr std::array<OSType, 3> types {
        kAudioUnitType_Effect, kAudioUnitType_MusicEffect, kAudioUnitType_MusicDevice
    };
    for (const auto type : types)
    {
        if (abort != nullptr && abort->load (std::memory_order_relaxed)) break;
        AudioComponentDescription query {};
        query.componentType = type;
        AudioComponent component = nullptr;
        while ((component = AudioComponentFindNext (component, &query)) != nullptr)
        {
            if (abort != nullptr && abort->load (std::memory_order_relaxed)) break;
            PluginDesc description;
            if (describe (component, description))
                found.push_back (std::move (description));
        }
    }
    return found;
}
} // namespace duskstudio::au
