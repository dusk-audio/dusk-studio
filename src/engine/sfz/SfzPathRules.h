#pragma once

#include <cstddef>
#include <string>
#include <string_view>

// One implementation of "is this relative path safe to create under a store
// root". The catalog schema and the archive extractor both gate on it, so a
// path the catalog accepts can never be a path the extractor would refuse to
// reason about, and neither can drift into accepting traversal.
namespace duskstudio::sfz::paths
{
constexpr std::size_t kMaxPathLength = 512;
constexpr std::size_t kMaxPathDepth = 32;

inline bool isAsciiDigit (unsigned char c) noexcept
{
    return c >= '0' && c <= '9';
}

inline bool isAsciiAlpha (unsigned char c) noexcept
{
    return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

inline bool isAsciiAlphaNumeric (unsigned char c) noexcept
{
    return isAsciiAlpha (c) || isAsciiDigit (c);
}

inline char toLowerAscii (char c) noexcept
{
    const auto byte = static_cast<unsigned char> (c);
    return byte >= 'A' && byte <= 'Z'
               ? static_cast<char> (byte - 'A' + 'a')
               : c;
}

inline std::string toLowerAscii (std::string_view value)
{
    std::string lowered;
    lowered.reserve (value.size());
    for (const auto c : value)
        lowered.push_back (toLowerAscii (c));
    return lowered;
}

inline bool equalsAsciiCaseInsensitive (std::string_view value,
                                        std::string_view expected) noexcept
{
    if (value.size() != expected.size())
        return false;
    for (std::size_t i = 0; i < value.size(); ++i)
    {
        auto c = static_cast<unsigned char> (value[i]);
        if (c >= 'a' && c <= 'z')
            c = static_cast<unsigned char> (c - 'a' + 'A');
        if (c != static_cast<unsigned char> (expected[i]))
            return false;
    }
    return true;
}

inline bool isWindowsReservedBasename (std::string_view segment) noexcept
{
    const auto dot = segment.find ('.');
    auto basename = segment.substr (0, dot);
    while (! basename.empty() && (basename.back() == ' ' || basename.back() == '.'))
        basename.remove_suffix (1);
    if (equalsAsciiCaseInsensitive (basename, "CON")
        || equalsAsciiCaseInsensitive (basename, "PRN")
        || equalsAsciiCaseInsensitive (basename, "AUX")
        || equalsAsciiCaseInsensitive (basename, "NUL"))
        return true;

    if (basename.size() < 4)
        return false;
    if (! equalsAsciiCaseInsensitive (basename.substr (0, 3), "COM")
        && ! equalsAsciiCaseInsensitive (basename.substr (0, 3), "LPT"))
        return false;

    const auto suffix = basename.substr (3);
    if (suffix.size() == 1)
        return suffix[0] >= '1' && suffix[0] <= '9';
    return suffix.size() == 2
        && static_cast<unsigned char> (suffix[0]) == 0xc2
        && (static_cast<unsigned char> (suffix[1]) == 0xb9
            || static_cast<unsigned char> (suffix[1]) == 0xb2
            || static_cast<unsigned char> (suffix[1]) == 0xb3);
}

// Returns nullptr when the path may be created below a store root, otherwise
// the reason it was refused. Rejects absolute paths, drive letters, UNC and
// backslash separators, traversal, empty and dot segments, control bytes,
// Windows device names, and anything past the depth or length ceiling.
inline const char* relativePathRejectionReason (
    std::string_view value,
    std::size_t maxLength = kMaxPathLength,
    std::size_t maxDepth = kMaxPathDepth) noexcept
{
    if (value.empty())
        return "must not be empty";
    if (value.size() > maxLength)
        return "exceeds the path length limit";
    if (value.front() == '/' || value.find ('\\') != std::string_view::npos)
        return "must be a forward-slash relative path";
    if (value.find_first_of (":<>\"|?*") != std::string_view::npos)
        return "contains a Win32-invalid filename character";

    std::size_t depth = 0;
    std::size_t begin = 0;
    while (begin <= value.size())
    {
        const auto end = value.find ('/', begin);
        const auto length = (end == std::string_view::npos ? value.size() : end) - begin;
        if (length == 0)
            return "must not contain empty path segments";

        const auto segment = value.substr (begin, length);
        if (segment == "." || segment == "..")
            return "must not contain dot path segments";
        if (segment.back() == '.' || segment.back() == ' ')
            return "path segments must not end in a dot or space";
        for (const auto c : segment)
        {
            const auto byte = static_cast<unsigned char> (c);
            if (byte == 0 || byte < 0x20 || byte == 0x7f)
                return "contains a control character";
        }
        if (isWindowsReservedBasename (segment))
            return "contains a reserved Windows device basename";

        if (++depth > maxDepth)
            return "exceeds the path depth limit";
        if (end == std::string_view::npos)
            break;
        begin = end + 1;
    }
    return nullptr;
}
} // namespace duskstudio::sfz::paths
