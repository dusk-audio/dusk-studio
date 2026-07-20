#include "DeviceStateBlob.h"

#include "../../foundation/Json.h"

#include <nlohmann/json.hpp>

#include <cctype>
#include <cstdint>
#include <cstdlib>
#include <map>
#include <vector>

namespace duskstudio::device
{
namespace
{
std::vector<int> channelIndices (const ChannelSet& cs)
{
    std::vector<int> out;
    for (int i = 0; i < ChannelSet::kMaxChannels; ++i)
        if (cs[i]) out.push_back (i);
    return out;
}

ChannelSet maskFromJsonArray (const nlohmann::json& arr)
{
    ChannelSet cs;
    for (const auto& e : arr)
        if (e.is_number_integer() || e.is_number_unsigned())
        {
            const auto v = e.get<std::int64_t>();
            if (v >= 0 && v < ChannelSet::kMaxChannels) cs.setBit ((int) v);
        }
    return cs;
}

// MSB-first base-2 mask parse, matching JUCE's BigInteger::parseString(str, 2):
// leading whitespace and a leading '-' are skipped, each '0'/'1' shifts the
// accumulator left (high bit first), and any other character is ignored while
// scanning continues to the end. Channel masks never exceed 64 bits (ChannelSet's
// documented ceiling), so a uint64 accumulator is sufficient.
ChannelSet parseMaskBase2 (const std::string& t)
{
    std::uint64_t v = 0;
    size_t i = 0;
    while (i < t.size() && std::isspace ((unsigned char) t[i])) ++i;
    if (i < t.size() && t[i] == '-') ++i;   // negative flag has no meaning for a mask
    for (; i < t.size(); ++i)
    {
        if      (t[i] == '0') v <<= 1;
        else if (t[i] == '1') v = (v << 1) | 1u;
        // any other character is skipped, matching BigInteger's digit >= base path
    }
    return ChannelSet::fromRaw (v);
}

void appendUtf8 (std::string& out, long cp)
{
    if (cp < 0) return;
    if (cp <= 0x7F)        out += (char) cp;
    else if (cp <= 0x7FF)  { out += (char) (0xC0 | (cp >> 6));  out += (char) (0x80 | (cp & 0x3F)); }
    else if (cp <= 0xFFFF) { out += (char) (0xE0 | (cp >> 12)); out += (char) (0x80 | ((cp >> 6) & 0x3F)); out += (char) (0x80 | (cp & 0x3F)); }
    else if (cp <= 0x10FFFF)
    {
        out += (char) (0xF0 | (cp >> 18));
        out += (char) (0x80 | ((cp >> 12) & 0x3F));
        out += (char) (0x80 | ((cp >> 6) & 0x3F));
        out += (char) (0x80 | (cp & 0x3F));
    }
}

bool iequals (const std::string& a, const char* b)
{
    size_t i = 0;
    for (; i < a.size() && b[i] != 0; ++i)
        if (std::tolower ((unsigned char) a[i]) != std::tolower ((unsigned char) b[i])) return false;
    return i == a.size() && b[i] == 0;
}

// Decode the five standard XML entities + decimal/hex numeric references, exactly
// as JUCE's XmlDocument::readEntity does. Unknown entities and a stray '&' are left
// verbatim (an unterminated '&' can't be an entity).
std::string decodeXmlEntities (const std::string& in)
{
    std::string out;
    out.reserve (in.size());
    for (size_t i = 0; i < in.size(); )
    {
        if (in[i] != '&') { out += in[i]; ++i; continue; }

        const size_t semi = in.find (';', i + 1);
        if (semi == std::string::npos) { out += in[i]; ++i; continue; }

        const std::string ent = in.substr (i + 1, semi - (i + 1));
        if      (iequals (ent, "amp"))  out += '&';
        else if (iequals (ent, "quot")) out += '"';
        else if (iequals (ent, "apos")) out += '\'';
        else if (iequals (ent, "lt"))   out += '<';
        else if (iequals (ent, "gt"))   out += '>';
        else if (! ent.empty() && ent[0] == '#')
        {
            const long cp = (ent.size() >= 2 && (ent[1] == 'x' || ent[1] == 'X'))
                          ? std::strtol (ent.c_str() + 2, nullptr, 16)
                          : std::strtol (ent.c_str() + 1, nullptr, 10);
            appendUtf8 (out, cp);
        }
        else { out += '&'; out += ent; out += ';'; }   // unknown entity: leave literal

        i = semi + 1;
    }
    return out;
}

std::optional<DeviceStateBlob> parseJson (const std::string& s)
{
    const auto j = nlohmann::json::parse (s, nullptr, false);
    if (j.is_discarded() || ! j.is_object()) return std::nullopt;

    DeviceStateBlob b;
    b.deviceType             = dusk::json::getString (j, "deviceType");
    b.setup.outputDeviceName = dusk::json::getString (j, "outputDevice");
    b.setup.inputDeviceName  = dusk::json::getString (j, "inputDevice");
    b.setup.sampleRate       = dusk::json::getDouble (j, "sampleRate", 0.0);
    b.setup.bufferSize       = dusk::json::getInt (j, "bufferSize", 0);

    // Presence of the channel array is the useDefault toggle: key absent leaves
    // useDefault*Channels at its true default and the mask cleared.
    if (dusk::json::has (j, "outputChans"))
    {
        b.setup.useDefaultOutputChannels = false;
        b.setup.outputChannels = maskFromJsonArray (dusk::json::array (j, "outputChans"));
    }
    if (dusk::json::has (j, "inputChans"))
    {
        b.setup.useDefaultInputChannels = false;
        b.setup.inputChannels = maskFromJsonArray (dusk::json::array (j, "inputChans"));
    }
    return b;
}

// Bespoke legacy <DEVICESETUP> reader: skip any <?...?> / <!...> prolog, require
// the DEVICESETUP element, collect its name="value" attributes (entities decoded)
// and map them the way JUCE's AudioDeviceManager::initialiseFromXML does. A tag
// mismatch or a structurally malformed element reads as unparseable (nullopt).
std::optional<DeviceStateBlob> parseLegacyXml (const std::string& s)
{
    const size_t n = s.size();
    size_t i = 0;
    std::string tag;

    while (i < n)
    {
        while (i < n && s[i] != '<') ++i;
        if (i >= n) return std::nullopt;
        ++i;   // past '<'

        if (i < n && s[i] == '?')            // processing instruction (e.g. <?xml?>)
        {
            const size_t e = s.find ("?>", i);
            if (e == std::string::npos) return std::nullopt;
            i = e + 2; continue;
        }
        if (i < n && s[i] == '!')            // comment / doctype
        {
            const size_t e = s.find ('>', i);
            if (e == std::string::npos) return std::nullopt;
            i = e + 1; continue;
        }

        const size_t ts = i;
        while (i < n && ! std::isspace ((unsigned char) s[i]) && s[i] != '>' && s[i] != '/') ++i;
        tag = s.substr (ts, i - ts);
        break;
    }
    if (tag != "DEVICESETUP") return std::nullopt;

    std::map<std::string, std::string> attrs;
    while (i < n)
    {
        while (i < n && std::isspace ((unsigned char) s[i])) ++i;
        if (i >= n) return std::nullopt;                 // element never closed
        if (s[i] == '>' || s[i] == '/') break;           // end of open tag / self-close

        const size_t ns = i;
        while (i < n && s[i] != '=' && ! std::isspace ((unsigned char) s[i]) && s[i] != '>' && s[i] != '/') ++i;
        const std::string name = s.substr (ns, i - ns);

        while (i < n && std::isspace ((unsigned char) s[i])) ++i;
        if (i >= n || s[i] != '=') return std::nullopt;
        ++i;
        while (i < n && std::isspace ((unsigned char) s[i])) ++i;
        if (i >= n || (s[i] != '"' && s[i] != '\'')) return std::nullopt;

        const char quote = s[i++];
        const size_t vs = i;
        while (i < n && s[i] != quote) ++i;
        if (i >= n) return std::nullopt;                 // unterminated attribute value
        const std::string raw = s.substr (vs, i - vs);
        ++i;   // past closing quote

        if (! name.empty()) attrs[name] = decodeXmlEntities (raw);
    }

    const auto find = [&attrs] (const char* k) -> const std::string*
    {
        const auto it = attrs.find (k);
        return it == attrs.end() ? nullptr : &it->second;
    };

    DeviceStateBlob b;
    if (auto* v = find ("deviceType"))            b.deviceType             = *v;
    if (auto* v = find ("audioOutputDeviceName")) b.setup.outputDeviceName = *v;
    if (auto* v = find ("audioInputDeviceName"))  b.setup.inputDeviceName  = *v;
    if (auto* v = find ("audioDeviceRate"))       b.setup.sampleRate = std::strtod (v->c_str(), nullptr);
    if (auto* v = find ("audioDeviceBufferSize")) b.setup.bufferSize = (int) std::strtol (v->c_str(), nullptr, 10);

    const std::string* inCh  = find ("audioDeviceInChans");
    const std::string* outCh = find ("audioDeviceOutChans");
    b.setup.useDefaultInputChannels  = (inCh  == nullptr);
    b.setup.useDefaultOutputChannels = (outCh == nullptr);
    // Absent attribute still parses the "11" default the way JUCE does, so the
    // stored mask matches even though useDefault makes the engine ignore it.
    b.setup.inputChannels  = parseMaskBase2 (inCh  != nullptr ? *inCh  : std::string ("11"));
    b.setup.outputChannels = parseMaskBase2 (outCh != nullptr ? *outCh : std::string ("11"));
    return b;
}
} // namespace

std::string DeviceStateBlob::toJson() const
{
    nlohmann::ordered_json j;
    j["version"]      = 1;
    j["deviceType"]   = deviceType;
    j["outputDevice"] = setup.outputDeviceName;
    j["inputDevice"]  = setup.inputDeviceName;
    j["sampleRate"]   = setup.sampleRate;
    j["bufferSize"]   = setup.bufferSize;
    if (! setup.useDefaultOutputChannels) j["outputChans"] = channelIndices (setup.outputChannels);
    if (! setup.useDefaultInputChannels)  j["inputChans"]  = channelIndices (setup.inputChannels);
    return j.dump (2);
}

std::optional<DeviceStateBlob> DeviceStateBlob::parse (const std::string& blob)
{
    size_t i = 0;
    while (i < blob.size() && std::isspace ((unsigned char) blob[i])) ++i;
    if (i >= blob.size()) return std::nullopt;

    if (blob[i] == '{') return parseJson (blob);
    if (blob[i] == '<') return parseLegacyXml (blob);
    return std::nullopt;
}

std::string DeviceStateBlob::outputDeviceName (const std::string& blob)
{
    const auto b = parse (blob);
    if (! b) return {};
    return ! b->setup.outputDeviceName.empty() ? b->setup.outputDeviceName
                                               : b->setup.inputDeviceName;
}
} // namespace duskstudio::device
