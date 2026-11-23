#pragma once

#include <algorithm>
#include <cstring>
#include <string>
#include <vector>

namespace duskstudio::imgui
{
// Where a panel's last frame drew its controls, by name, for the scenario harness.
// A control is reported only while that frame drew all of it inside the panel and
// clear of every other control, so a control that is clipped, covered or not drawn
// at all reads as missing.
class ScenarioControlMap
{
public:
    struct Box
    {
        float x0 = 0.0f, y0 = 0.0f, x1 = 0.0f, y1 = 0.0f;

        bool empty() const noexcept { return ! (x1 > x0) || ! (y1 > y0); }
        bool overlaps (const Box& o) const noexcept { return x0 < o.x1 && o.x0 < x1 && y0 < o.y1 && o.y0 < y1; }
        bool inside (const Box& o) const noexcept { return x0 >= o.x0 && y0 >= o.y0 && x1 <= o.x1 && y1 <= o.y1; }
        Box unite (const Box& o) const noexcept
        {
            if (empty()) return o;
            if (o.empty()) return *this;
            return { std::min (x0, o.x0), std::min (y0, o.y0), std::max (x1, o.x1), std::max (y1, o.y1) };
        }
    };

    // Starts a frame: every control is undrawn until note() says otherwise.
    void begin (const Box& panel)
    {
        viewport = panel;
        for (auto& entry : entries)
        {
            entry.drawn = {};
            entry.hit = {};
            entry.clean = false;
        }
    }

    // `drawn` is all of the control, captions and readouts included; `hit`, where
    // it takes the pointer, when that is less than all of it.
    void note (const char* name, const Box& drawn) { note (name, drawn, drawn); }
    void note (const char* name, const Box& drawn, const Box& hit)
    {
        for (auto& entry : entries)
            if (std::strcmp (entry.name.c_str(), name) == 0)
            {
                entry.drawn = drawn;
                entry.hit = hit;
                return;
            }
        entries.push_back ({ name, drawn, false, hit });
    }

    void settle()
    {
        for (auto& entry : entries)
        {
            entry.clean = ! entry.drawn.empty() && entry.drawn.inside (viewport);
            for (const auto& other : entries)
                if (&other != &entry && ! other.drawn.empty() && entry.drawn.overlaps (other.drawn))
                    entry.clean = false;
        }
    }

    // The centre across of where the control takes the pointer, and `fraction` of
    // the way up it, half a pixel in from its edges; a box under a pixel tall gives
    // its centre.
    bool point (const std::string& name, float fraction, float& x, float& y) const
    {
        for (const auto& entry : entries)
            if (entry.name == name)
            {
                if (! entry.clean) return false;
                const auto& r = entry.hit.empty() ? entry.drawn : entry.hit;
                const float inset = std::min (0.5f, 0.5f * (r.y1 - r.y0));
                x = 0.5f * (r.x0 + r.x1);
                y = r.y1 - inset - std::clamp (fraction, 0.0f, 1.0f) * (r.y1 - r.y0 - 2.0f * inset);
                return true;
            }
        return false;
    }

private:
    struct Entry
    {
        std::string name;
        Box drawn;
        bool clean;
        Box hit;
    };

    std::vector<Entry> entries;
    Box viewport;
};
} // namespace duskstudio::imgui
