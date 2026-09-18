#pragma once

#include "../../foundation/Json.h"
#include "../../foundation/Text.h"

#include <string>
#include <vector>

namespace duskstudio::pipewire
{
// The graph publishes the desktop's audio devices in its "default" metadata
// object, as JSON of the form {"name":"<node.name>"}. Resolve one of those
// values against a scan's node names.
//
// default.audio.sink / .source are the pair in force. The parallel
// default.configured.audio.* keys record what the user last asked for, which
// can name a device that is not present right now (a switched-off Bluetooth
// sink, say), so they are not what to open.
//
// Returns the index into ids, or -1 when the value is absent, malformed, names
// a node this scan did not see, or names a monitor. A monitor is a real choice
// for the desktop and a bad one for a DAW: defaulting the record input to the
// loopback of your own output produces takes full of whatever else was playing,
// and nothing on screen says why. Those fall back to the caller's own order.
inline int indexOfMetadataDefault (const std::string& metadataValue,
                                   const std::vector<std::string>& ids,
                                   const std::vector<std::string>& names)
{
    if (metadataValue.empty())
        return -1;

    const auto parsed = dusk::json::Json::parse (metadataValue, nullptr,
                                                 /*allow_exceptions*/ false);
    const auto wanted = dusk::json::getString (parsed, "name");
    if (wanted.empty())
        return -1;

    for (int i = 0; i < (int) ids.size(); ++i)
    {
        if (ids[(size_t) i] != wanted)
            continue;
        if (dusk::text::containsIgnoreCase (ids[(size_t) i], "monitor"))
            return -1;
        if (i < (int) names.size()
            && dusk::text::containsIgnoreCase (names[(size_t) i], "monitor"))
            return -1;
        return i;
    }
    return -1;
}
} // namespace duskstudio::pipewire
