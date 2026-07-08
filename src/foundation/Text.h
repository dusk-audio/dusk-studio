#pragma once

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

// String helpers over std::string (UTF-8 bytes), replicating the JUCE String
// methods this codebase actually uses so call sites can drop juce_core. ASCII
// case/whitespace semantics match JUCE (which treats any byte <= ' ' as
// whitespace and folds case locale-independently over A-Z / a-z).
namespace dusk::text
{
inline char lowerAscii (char c) noexcept
{
    return (c >= 'A' && c <= 'Z') ? char (c - 'A' + 'a') : c;
}

inline char upperAscii (char c) noexcept
{
    return (c >= 'a' && c <= 'z') ? char (c - 'a' + 'A') : c;
}

inline std::string toLowerCase (std::string s)
{
    for (auto& c : s) c = lowerAscii (c);
    return s;
}

inline std::string toUpperCase (std::string s)
{
    for (auto& c : s) c = upperAscii (c);
    return s;
}

inline bool contains (std::string_view s, std::string_view sub) noexcept
{
    return s.find (sub) != std::string_view::npos;
}

inline bool containsIgnoreCase (std::string_view s, std::string_view sub)
{
    return contains (toLowerCase (std::string (s)), toLowerCase (std::string (sub)));
}

inline bool startsWith (std::string_view s, std::string_view p) noexcept
{
    return s.size() >= p.size() && s.compare (0, p.size(), p) == 0;
}

inline bool endsWith (std::string_view s, std::string_view p) noexcept
{
    return s.size() >= p.size() && s.compare (s.size() - p.size(), p.size(), p) == 0;
}

inline int indexOf (std::string_view s, std::string_view sub) noexcept
{
    const auto p = s.find (sub);
    return p == std::string_view::npos ? -1 : (int) p;
}

// JUCE trims any byte <= ' ' from both ends.
inline std::string trim (std::string_view s)
{
    size_t a = 0, b = s.size();
    while (a < b && (unsigned char) s[a] <= ' ') ++a;
    while (b > a && (unsigned char) s[b - 1] <= ' ') --b;
    return std::string (s.substr (a, b - a));
}

// JUCE clamps: start < 0 -> 0; end clamped to [start, len]; start past end -> "".
inline std::string substring (std::string_view s, int start, int end)
{
    const int len = (int) s.size();
    start = std::clamp (start, 0, len);
    end   = std::clamp (end, start, len);
    return std::string (s.substr ((size_t) start, (size_t) (end - start)));
}

inline std::string substring (std::string_view s, int start)
{
    return substring (s, start, (int) s.size());
}

inline std::string dropLastCharacters (std::string_view s, int n)
{
    n = std::clamp (n, 0, (int) s.size());
    return std::string (s.substr (0, s.size() - (size_t) n));
}

inline std::string upToFirstOccurrenceOf (std::string_view s, std::string_view sub, bool includeSub)
{
    const auto p = s.find (sub);
    if (p == std::string_view::npos) return std::string (s);
    return std::string (s.substr (0, p + (includeSub ? sub.size() : 0)));
}

inline std::string upToLastOccurrenceOf (std::string_view s, std::string_view sub, bool includeSub)
{
    const auto p = s.rfind (sub);
    if (p == std::string_view::npos) return std::string (s);
    return std::string (s.substr (0, p + (includeSub ? sub.size() : 0)));
}

// Not-found behaviour follows JUCE, which is asymmetric here: fromFirst returns
// empty, fromLast returns the whole string.
inline std::string fromFirstOccurrenceOf (std::string_view s, std::string_view sub, bool includeSub)
{
    const auto p = s.find (sub);
    if (p == std::string_view::npos) return {};
    return std::string (s.substr (p + (includeSub ? 0 : sub.size())));
}

inline std::string fromLastOccurrenceOf (std::string_view s, std::string_view sub, bool includeSub)
{
    const auto p = s.rfind (sub);
    if (p == std::string_view::npos) return std::string (s);
    return std::string (s.substr (p + (includeSub ? 0 : sub.size())));
}

inline std::string replace (std::string_view s, std::string_view from, std::string_view to)
{
    if (from.empty()) return std::string (s);
    std::string out;
    size_t i = 0;
    for (;;)
    {
        const auto p = s.find (from, i);
        if (p == std::string_view::npos) { out.append (s.substr (i)); break; }
        out.append (s.substr (i, p - i));
        out.append (to);
        i = p + from.size();
    }
    return out;
}

inline std::string retainCharacters (std::string_view s, std::string_view allowed)
{
    std::string out;
    for (char c : s)
        if (allowed.find (c) != std::string_view::npos) out += c;
    return out;
}

inline std::string paddedLeft (std::string_view s, char padChar, int minLength)
{
    if ((int) s.size() >= minLength) return std::string (s);
    return std::string ((size_t) minLength - s.size(), padChar) + std::string (s);
}

// strtol/strtod skip leading whitespace and parse the leading number, matching
// JUCE's "read a value from the front, 0 if none" contract.
inline int getIntValue (const std::string& s) noexcept
{
    return (int) std::strtol (s.c_str(), nullptr, 10);
}

inline double getDoubleValue (const std::string& s) noexcept
{
    return std::strtod (s.c_str(), nullptr);
}

inline float getFloatValue (const std::string& s) noexcept
{
    return (float) getDoubleValue (s);
}

inline std::string joinIntoString (const std::vector<std::string>& parts, std::string_view sep)
{
    std::string out;
    for (size_t i = 0; i < parts.size(); ++i)
    {
        if (i != 0) out.append (sep);
        out.append (parts[i]);
    }
    return out;
}

inline std::vector<std::string> split (std::string_view s, char delim)
{
    std::vector<std::string> out;
    size_t start = 0;
    for (size_t i = 0; i <= s.size(); ++i)
        if (i == s.size() || s[i] == delim)
        {
            out.emplace_back (s.substr (start, i - start));
            start = i + 1;
        }
    return out;
}
} // namespace dusk::text
