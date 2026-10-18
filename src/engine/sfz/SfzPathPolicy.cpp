#include "SfzPathPolicy.h"

#include <utf8proc.h>

#include <algorithm>
#include <cstdlib>
#include <limits>
#include <memory>

namespace duskstudio::sfz
{
namespace
{
bool equalsAscii (std::string_view value, std::string_view expected)
{
    if (value.size() != expected.size()) return false;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        auto c = static_cast<unsigned char> (value[i]);
        if (c >= 'a' && c <= 'z') c = static_cast<unsigned char> (c - 'a' + 'A');
        if (c != static_cast<unsigned char> (expected[i])) return false;
    }
    return true;
}

bool reservedBasename (std::string_view segment)
{
    auto name = segment.substr (0, segment.find ('.'));
    while (! name.empty() && (name.back() == ' ' || name.back() == '.'))
        name.remove_suffix (1);
    if (equalsAscii (name, "CON") || equalsAscii (name, "PRN")
        || equalsAscii (name, "AUX") || equalsAscii (name, "NUL")
        || equalsAscii (name, "CONIN$") || equalsAscii (name, "CONOUT$")) return true;
    if (name.size() < 4
        || (! equalsAscii (name.substr (0, 3), "COM")
            && ! equalsAscii (name.substr (0, 3), "LPT"))) return false;
    const auto suffix = name.substr (3);
    return (suffix.size() == 1 && suffix[0] >= '0' && suffix[0] <= '9')
        || (suffix.size() == 2 && static_cast<unsigned char> (suffix[0]) == 0xc2
            && (static_cast<unsigned char> (suffix[1]) == 0xb9
                || static_cast<unsigned char> (suffix[1]) == 0xb2
                || static_cast<unsigned char> (suffix[1]) == 0xb3));
}

bool invalidSegment (std::string_view segment)
{
    return segment.empty() || segment == "." || segment == ".."
        || segment.back() == '.' || segment.back() == ' ' || reservedBasename (segment);
}
} // namespace

std::optional<std::string> portablePathKey (std::string_view path)
{
    if (path.size() > static_cast<std::size_t> (
            std::numeric_limits<utf8proc_ssize_t>::max())) return std::nullopt;
    utf8proc_uint8_t* mapped = nullptr;
    // Windows compares uppercase forms, which adds aliases to Unicode casefold.
    const auto size = utf8proc_map_custom (
        reinterpret_cast<const utf8proc_uint8_t*> (path.data()),
        static_cast<utf8proc_ssize_t> (path.size()), &mapped,
        static_cast<utf8proc_option_t> (UTF8PROC_STABLE | UTF8PROC_COMPOSE
                                       | UTF8PROC_CASEFOLD | UTF8PROC_IGNORE),
        [] (utf8proc_int32_t codepoint, void*) { return utf8proc_toupper (codepoint); },
        nullptr);
    if (size < 0) return std::nullopt;
    const std::unique_ptr<utf8proc_uint8_t, decltype (&std::free)> owned (mapped, std::free);
    if (size == 0) return std::string {};
    const std::string key (reinterpret_cast<const char*> (mapped),
                           static_cast<std::size_t> (size));
    return key;
}

bool validateRelativePath (std::string_view path, std::string& error,
                           const PathLimits& limits)
{
    const auto fail = [&error] (const char* reason) { error = reason; return false; };
    if (path.empty()) return fail ("path is empty");
    if (path.size() > limits.maxBytes) return fail ("path exceeds the byte limit");
    if (path.front() == '/' || path.find ('\\') != std::string_view::npos)
        return fail ("path must be relative and use forward slashes");
    if (path.find_first_of (":<>\"|?*") != std::string_view::npos)
        return fail ("path contains a Windows-invalid character");

    std::size_t offset = 0;
    while (offset < path.size())
    {
        utf8proc_int32_t codepoint = 0;
        const auto bytes = utf8proc_iterate (
            reinterpret_cast<const utf8proc_uint8_t*> (path.data() + offset),
            static_cast<utf8proc_ssize_t> (path.size() - offset), &codepoint);
        if (bytes <= 0) return fail ("path is not valid UTF-8");
        if (codepoint < 0x20 || (codepoint >= 0x7f && codepoint <= 0x9f))
            return fail ("path contains a control character");
        offset += static_cast<std::size_t> (bytes);
    }

    const auto key = portablePathKey (path);
    if (! key) return fail ("path cannot be normalized");
    std::size_t depth = 0;
    for (std::size_t begin = 0; begin <= path.size();)
    {
        const auto end = path.find ('/', begin);
        const auto segment = path.substr (begin, (end == std::string_view::npos
                                                ? path.size() : end) - begin);
        if (invalidSegment (segment)) return fail ("path contains an unsafe segment");
        if (segment.size() > limits.maxSegmentBytes)
            return fail ("path segment exceeds the byte limit");
        if (++depth > limits.maxDepth) return fail ("path exceeds the depth limit");
        if (end == std::string_view::npos) break;
        begin = end + 1;
    }
    for (std::size_t begin = 0; begin <= key->size();)
    {
        const auto end = key->find ('/', begin);
        const auto segment = std::string_view (*key).substr (
            begin, (end == std::string::npos ? key->size() : end) - begin);
        if (invalidSegment (segment))
            return fail ("path contains a non-portable normalized segment");
        if (end == std::string::npos) break;
        begin = end + 1;
    }
    error.clear();
    return true;
}

bool isSha256 (std::string_view value) noexcept
{
    return value.size() == 64 && std::all_of (value.begin(), value.end(), [] (char c)
        { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); });
}
} // namespace duskstudio::sfz
