#include "WinMmDeviceQuery.h"
#include "WinMmProtocol.h"

#include <mmddk.h>

#include <algorithm>
#include <array>
#include <utility>

namespace duskstudio::midi::winmm
{
namespace
{
std::optional<std::string> utf8 (const wchar_t* text, std::size_t capacity)
{
    const auto* end = std::find (text, text + capacity, L'\0');
    if (end == text + capacity) return std::nullopt;
    if (end == text) return std::string {};
    const auto length = static_cast<int> (end - text);
    const auto bytes = WideCharToMultiByte (CP_UTF8, WC_ERR_INVALID_CHARS, text, length,
                                           nullptr, 0, nullptr, nullptr);
    if (bytes == 0) return std::nullopt;
    std::string result (static_cast<std::size_t> (bytes), '\0');
    if (WideCharToMultiByte (CP_UTF8, WC_ERR_INVALID_CHARS, text, length,
                             result.data(), bytes, nullptr, nullptr) != bytes)
        return std::nullopt;
    return result;
}
}

std::vector<Endpoint> enumerateDevices (Direction direction, const DeviceQueryApi& api)
{
    const bool input = direction == Direction::Input;
    const auto count = input ? api.inputCount() : api.outputCount();
    std::vector<unsigned int> indices;
    std::vector<BackendDeviceInfo> devices;
    for (unsigned int index = 0; index < count; ++index)
    {
        std::optional<std::string> name;
        if (input)
        {
            MIDIINCAPSW caps {};
            if (api.inputCaps (index, &caps, sizeof (caps)) != MMSYSERR_NOERROR) continue;
            name = utf8 (caps.szPname, MAXPNAMELEN);
        }
        else
        {
            MIDIOUTCAPSW caps {};
            if (api.outputCaps (index, &caps, sizeof (caps)) != MMSYSERR_NOERROR) continue;
            name = utf8 (caps.szPname, MAXPNAMELEN);
        }
        if (! name) continue;

        const auto query = [&] (UINT command, DWORD_PTR first, DWORD_PTR second)
        {
            const auto id = static_cast<UINT_PTR> (index);
            return input ? api.inputMessage (reinterpret_cast<HMIDIIN> (id), command, first, second)
                         : api.outputMessage (reinterpret_cast<HMIDIOUT> (id), command, first, second);
        };
        std::string identifier;
        ULONG bytes = 0;
        std::array<wchar_t, 512> interfaceName {};
        if (query (DRV_QUERYDEVICEINTERFACESIZE, reinterpret_cast<DWORD_PTR> (&bytes), 0) == MMSYSERR_NOERROR
            && bytes >= sizeof (wchar_t) && bytes < sizeof (interfaceName) && bytes % sizeof (wchar_t) == 0
            && query (DRV_QUERYDEVICEINTERFACE, reinterpret_cast<DWORD_PTR> (interfaceName.data()),
                      sizeof (interfaceName)) == MMSYSERR_NOERROR)
            if (auto value = utf8 (interfaceName.data(), interfaceName.size()))
                identifier = std::move (*value);
        indices.push_back (index);
        devices.push_back ({ std::move (*name), std::move (identifier) });
    }
    finishDeviceEnumeration (devices);
    std::vector<Endpoint> result;
    result.reserve (devices.size());
    for (std::size_t i = 0; i < devices.size(); ++i)
        result.push_back ({ indices[i], std::move (devices[i]) });
    return result;
}

std::optional<unsigned int> deviceIndexForIdentifier (const std::vector<Endpoint>& devices,
                                                     std::string_view identifier)
{
    if (identifier.empty()) return std::nullopt;
    std::optional<unsigned int> result;
    for (const auto& device : devices)
        if (device.info.identifier == identifier)
        {
            if (result) return std::nullopt;
            result = device.deviceIndex;
        }
    return result;
}
} // namespace duskstudio::midi::winmm
