#pragma once

#include "../foundation/Text.h"

#include <cmath>
#include <locale>
#include <sstream>
#include <string>
#include <string_view>

namespace duskstudio
{
// Strict, full-string float parse — reject "123abc" / junk rather than taking a
// numeric prefix the way JUCE's String::getDoubleValue() does. Locale-INDEPENDENT:
// the classic locale forces '.' as the decimal point, so "127.6" parses the same
// on comma-decimal systems (fr_FR/de_DE) as on en_*. Shared by every tempo-entry
// field (TransportBar, TapeStrip) so the validation lives in one place.
// Not noexcept: the string + stream allocations can throw std::bad_alloc.
// operator>> itself doesn't throw — it sets failbit, which the check below
// handles — so parse failure stays exception-free.
inline bool parseFullFloat (std::string_view s, float& out)
{
    const auto t = dusk::text::trim (s);
    if (t.empty()) return false;
    std::istringstream ss (t);
    ss.imbue (std::locale::classic());
    float v {};
    ss >> v;
    if (ss.fail() || ! ss.eof() || ! std::isfinite (v)) return false;   // junk / partial / inf / nan
    out = v;
    return true;
}
} // namespace duskstudio
