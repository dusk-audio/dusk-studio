#pragma once

#include "../foundation/Text.h"

#include <cmath>
#include <string>

namespace duskstudio
{
// The aux send knob's bottom end stop. The strip stores the OFF sentinel
// there, so the knob's lowest audible level is one step above it.
constexpr double kAuxSendKnobOffDb = -60.0;

inline bool auxSendIsOff (double db) noexcept
{
    return db <= kAuxSendKnobOffDb + 0.01;
}

inline double auxSendShownDb (double db) noexcept
{
    const double shown = std::round (db * 10.0) / 10.0;
    return std::abs (shown) < 0.05 ? 0.0 : shown;
}

// The caption under a send knob, sized for the narrow strip column: U+2212
// when OFF, whole dB from 10 dB out, one decimal inside it. " PRE" follows at
// any level, so a send parked at OFF still shows where it will tap.
inline std::string auxSendCaption (double db, bool preFader)
{
    std::string text;
    if (auxSendIsOff (db))
        text = "\xe2\x88\x92";
    else if (std::abs (db) >= 10.0)
        text = dusk::text::format ("%d", (int) std::round (db));
    else
        text = dusk::text::format ("%.1f", auxSendShownDb (db));
    if (preFader)
        text += " PRE";
    return text;
}

// The knob's value as a screen reader announces it.
inline std::string auxSendValueText (double db)
{
    return auxSendIsOff (db) ? std::string ("OFF")
                             : dusk::text::format ("%.1f dB", auxSendShownDb (db));
}

// A typed or assistive-technology value back to a knob position: OFF, or a
// number with an optional leading '+' and trailing unit.
inline double auxSendFromText (const std::string& text)
{
    auto trimmed = dusk::text::trim (text);
    if (dusk::text::toLowerCase (trimmed) == "off")
        return kAuxSendKnobOffDb;
    while (! trimmed.empty() && trimmed.front() == '+')
        trimmed = dusk::text::trim (trimmed.substr (1));
    const double db = dusk::text::getDoubleValue (trimmed);
    return std::isnan (db) || db < kAuxSendKnobOffDb ? kAuxSendKnobOffDb : db;
}
} // namespace duskstudio
