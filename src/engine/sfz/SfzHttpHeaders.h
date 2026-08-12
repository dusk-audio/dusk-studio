#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>

// The pure response-header parsing the libcurl transport relies on, split out so
// it can be unit-tested without a socket. The transport translation unit keeps
// only the libcurl glue; every rule that decides whether a resume is honoured or
// a size is trustworthy lives here where a test can reach it.
namespace duskstudio::sfz::http
{
// libcurl 7.85.0 is the first release with the string protocol allowlists
// (CURLOPT_PROTOCOLS_STR). The transport's #if uses the literal because a
// constexpr cannot drive the preprocessor; this names the same value so the
// meaning is documented and test-checked.
constexpr long kProtocolsStrVersion = 0x075500;

inline std::string trimHeaderValue (const char* data, std::size_t bytes)
{
    std::size_t begin = 0;
    while (begin < bytes && (data[begin] == ' ' || data[begin] == '\t'))
        ++begin;
    std::size_t end = bytes;
    while (end > begin
           && (data[end - 1] == '\r' || data[end - 1] == '\n'
               || data[end - 1] == ' ' || data[end - 1] == '\t'))
        --end;
    return std::string (data + begin, end - begin);
}

inline bool parseUnsigned (const std::string& value, std::uint64_t& out)
{
    if (value.empty())
        return false;

    std::uint64_t parsed = 0;
    for (const auto c : value)
    {
        if (c < '0' || c > '9')
            return false;
        const auto digit = static_cast<std::uint64_t> (c - '0');
        if (parsed > (std::numeric_limits<std::uint64_t>::max() - digit) / 10U)
            return false;
        parsed = parsed * 10U + digit;
    }
    out = parsed;
    return true;
}

// "bytes 100-199/1234" - the value after the slash is the full resource size,
// which is what the caller budgets against. "*" means the server declined to
// say, so the size stays unknown.
inline bool parseContentRangeTotal (const std::string& value, std::uint64_t& out)
{
    const auto slash = value.rfind ('/');
    if (slash == std::string::npos)
        return false;
    return parseUnsigned (value.substr (slash + 1), out);
}

// "bytes 100-199/1234" - the first number is where the returned bytes begin. A
// resume that asked for offset N must get N back; a 206 that starts at 0 (or an
// unsatisfied "bytes */1234" with no start at all) would append a fresh whole
// resource after the bytes already on disk, so the start has to be checked.
inline bool parseContentRangeStart (const std::string& value, std::uint64_t& out)
{
    const auto dash = value.find ('-');
    if (dash == std::string::npos)
        return false;
    std::size_t begin = dash;
    while (begin > 0 && value[begin - 1] >= '0' && value[begin - 1] <= '9')
        --begin;
    if (begin == dash)
        return false;
    return parseUnsigned (value.substr (begin, dash - begin), out);
}

inline bool headerNameMatches (const std::string& name, const char* expected)
{
    const std::size_t length = std::strlen (expected);
    if (name.size() != length)
        return false;
    for (std::size_t i = 0; i < length; ++i)
    {
        auto c = static_cast<unsigned char> (name[i]);
        if (c >= 'A' && c <= 'Z')
            c = static_cast<unsigned char> (c - 'A' + 'a');
        if (c != static_cast<unsigned char> (expected[i]))
            return false;
    }
    return true;
}

// A redirect chain delivers one header block per hop; only the final one
// describes the body, so a status line is the caller's cue to reset what it
// collected. Returns true when the line is an HTTP status line and reports the
// code (zero when it cannot be parsed).
inline bool parseHttpStatus (const char* buffer, std::size_t bytes, long& statusOut)
{
    if (bytes < 5 || std::memcmp (buffer, "HTTP/", 5) != 0)
        return false;
    statusOut = 0;
    const std::string line (buffer, bytes);
    const auto space = line.find (' ');
    if (space != std::string::npos)
    {
        std::uint64_t status = 0;
        if (parseUnsigned (trimHeaderValue (line.data() + space + 1,
                                            std::min<std::size_t> (3, line.size() - space - 1)),
                           status))
            statusOut = static_cast<long> (status);
    }
    return true;
}
} // namespace duskstudio::sfz::http
