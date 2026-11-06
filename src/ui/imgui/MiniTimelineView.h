#pragma once

#include "DuskPanelWindow.h"
#include "../GuiHost.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <exception>
#include <functional>
#include <string>
#include <utility>
#include <vector>

namespace duskstudio::imgui
{
class MiniTimelineView final : public DuskPanelView
{
public:
    struct Marker
    {
        std::string name;
        std::int64_t sample = 0;
        std::uint32_t colour = 0;
    };
    struct State
    {
        std::int64_t end = 1, playhead = 0;
        std::vector<Marker> markers;
    };

    State state;
    std::function<void (std::int64_t)> seek;
    std::function<void (int)> editMarker;
    std::function<void (std::string)> shellKey;
    std::function<void (const std::string&, float, float)> tooltip;
    std::vector<scenario::MiniMarkerPaint> markerPaint;

    ImVec2 preferredSize() const override { return { 800.0f, 20.0f }; }
    bool wantsPlate() const override { return false; }
    bool escapeDismisses() const override { return false; }
    bool claimsShortcut (ShellShortcut) const override { return true; }
    bool marksMissingGlyphs() const override { return true; }
    GlyphTally nameGlyphsForScenario() const override { return glyphs; }

    bool controlPointForScenario (const std::string& control, ImVec2& point, float = 0.5f) const override
    {
        if (size.x <= 0.0f) return false;
        try
        {
            if (control.rfind ("sample:", 0) == 0)
            {
                point = { xForSample (std::stoll (control.substr (7))), origin.y + size.y * 0.5f };
                return true;
            }
            if (control.rfind ("marker:", 0) == 0)
            {
                const auto index = std::stoul (control.substr (7));
                if (index >= markerPaint.size() || markerPaint[index].labelWidth <= 0) return false;
                const auto& row = markerPaint[index];
                point = { static_cast<float> (row.labelX) + static_cast<float> (row.labelWidth) * 0.5f,
                          static_cast<float> (row.labelY) };
                return true;
            }
        }
        catch (const std::exception&) {}
        return false;
    }

    void draw (DuskWidgets::Context& ctx, ImVec2 at, ImVec2 bounds) override
    {
        origin = at;
        size = bounds;
        scale = ctx.scale;
        glyphs = {};
        markerPaint.clear();
        const ImVec2 bottom (at.x + size.x, at.y + size.y);
        auto* dl = ctx.dl;
        dl->AddRectFilled (at, bottom, rgba (0xff15151a), ctx.s (3.0f));
        dl->AddRect ({ at.x + 0.5f, at.y + 0.5f }, { bottom.x - 0.5f, bottom.y - 0.5f },
                     rgba (0xff32323a), ctx.s (3.0f), 0, ctx.s (0.8f));
        const float mid = at.y + size.y * 0.5f;
        const float left = xForSample (0), right = xForSample (state.end);
        dl->AddLine ({ left, mid }, { right, mid }, rgba (0xff3a3a44), ctx.s (1.5f));
        for (const float x : { left, right })
            dl->AddLine ({ x, mid - size.y * 0.25f }, { x, mid + size.y * 0.25f },
                         rgba (0xff90a0b0), ctx.s (1.6f));

        int active = -1;
        for (int i = 0; i < static_cast<int> (state.markers.size()); ++i)
            if (state.markers[static_cast<size_t> (i)].sample <= state.playhead) active = i;
            else break;

        for (size_t i = 0; i < state.markers.size(); ++i)
        {
            const auto& marker = state.markers[i];
            const bool current = static_cast<int> (i) == active;
            const float x = xForSample (marker.sample);
            const float height = size.y * (current ? 0.80f : 0.52f);
            const float width = ctx.s (current ? 2.0f : 1.4f);
            const auto colour = markerColour (marker.colour);
            const auto tick = current ? brighter (colour, 0.30f) : alpha (colour, 217);
            dl->AddLine ({ x, mid - height * 0.5f }, { x, mid + height * 0.5f }, rgba (tick), width);
            markerPaint.push_back ({ marker.name, tick, 0, height, width });
        }

        auto* font = nameFace (*ctx.fonts);
        const float fontSize = ctx.s (11.0f);
        for (size_t i = 0; i < state.markers.size(); ++i)
        {
            const auto& marker = state.markers[i];
            const auto text = panelText (marker.name);
            const float x = xForSample (marker.sample) + ctx.s (5.0f);
            const float next = i + 1 < state.markers.size() ? xForSample (state.markers[i + 1].sample) : bottom.x;
            const float available = std::min (bottom.x - x - ctx.s (3.0f), next - x - ctx.s (2.0f));
            if (available < ctx.s (12.0f)) continue;
            const bool current = static_cast<int> (i) == active;
            const float width = std::min (available, font->CalcTextSizeA (fontSize, 1.0e10f, 0.0f, text.c_str()).x + ctx.s (6.0f));
            const auto colour = markerColour (marker.colour);
            const auto label = current ? brighter (colour, 0.35f) : alpha (colour, 230);
            dl->AddRectFilled ({ x, at.y + ctx.s (2.0f) }, { x + width, bottom.y - ctx.s (2.0f) },
                               rgba (alpha (0xff15151a, current ? 230 : 184)), ctx.s (2.0f));
            auto shown = text;
            const float textWidth = width - ctx.s (6.0f);
            if (font->CalcTextSizeA (fontSize, 1.0e10f, 0.0f, text.c_str()).x > textWidth)
            {
                const char* remaining = text.c_str();
                const float dots = font->CalcTextSizeA (fontSize, 1.0e10f, 0.0f, "...").x;
                font->CalcTextSizeA (fontSize, std::max (0.0f, textWidth - dots), 0.0f,
                                    text.c_str(), nullptr, &remaining);
                shown.assign (text.c_str(), remaining);
                shown += "...";
            }
            const ImVec4 clip (x + ctx.s (3.0f), at.y, x + width - ctx.s (3.0f), bottom.y);
            dl->AddText (font, fontSize, { clip.x, mid - fontSize * 0.5f }, rgba (label),
                         shown.c_str(), nullptr, 0.0f, &clip);
            glyphs.add (font, shown);
            auto& row = markerPaint[i];
            row.labelColour = label;
            row.labelX = static_cast<int> (std::lround (x));
            row.labelY = static_cast<int> (std::lround (mid));
            row.labelWidth = static_cast<int> (std::lround (width));
        }
        const float playhead = xForSample (state.playhead);
        dl->AddLine ({ playhead, at.y + ctx.s (1.0f) }, { playhead, bottom.y - ctx.s (1.0f) },
                     rgba (0xffe04040), ctx.s (1.4f));
        dl->AddTriangleFilled ({ playhead - ctx.s (3.0f), at.y + ctx.s (1.0f) },
                               { playhead + ctx.s (3.0f), at.y + ctx.s (1.0f) },
                               { playhead, at.y + ctx.s (4.0f) }, rgba (0xffe04040));

        const bool hovered = DuskWidgets::hitArea (ctx, "mini-timeline", at, bottom);
        const auto mouse = ImGui::GetIO().MousePos;
        const int hit = hovered ? markerAtX (mouse.x) : -1;
        if (hovered) ImGui::SetMouseCursor (hit >= 0 ? ImGuiMouseCursor_Hand : ImGuiMouseCursor_Arrow);
        if (hovered && ImGui::IsMouseClicked (ImGuiMouseButton_Left))
        {
            if (seek) seek (hit >= 0 ? state.markers[static_cast<size_t> (hit)].sample : sampleForX (mouse.x));
            if (hit >= 0 && ImGui::IsMouseDoubleClicked (ImGuiMouseButton_Left) && editMarker) editMarker (hit);
        }
        if (shellKey)
        {
            const auto& io = ImGui::GetIO();
            for (int key = ImGuiKey_NamedKey_BEGIN; key < ImGuiKey_NamedKey_END; ++key)
            {
                if (! ImGui::IsKeyPressed (static_cast<ImGuiKey> (key))) continue;
                auto description = shellKeyName (static_cast<ImGuiKey> (key));
                if (description.empty()) continue;
                if (io.KeyShift) description = "shift + " + description;
                if (io.KeyAlt) description = "alt + " + description;
               #if defined (__APPLE__)
                if (io.KeySuper) description = "ctrl + " + description;
                if (io.KeyCtrl) description = "command + " + description;
               #else
                if (io.KeyCtrl) description = "ctrl + " + description;
                if (io.KeySuper) description = "command + " + description;
               #endif
                shellKey (std::move (description));
            }
        }
        if (tooltip)
        {
            const bool help = hovered && ImGui::IsItemHovered (ImGuiHoveredFlags_DelayNormal | ImGuiHoveredFlags_Stationary)
                              && ! ImGui::IsMouseDown (ImGuiMouseButton_Left);
            const auto text = ! help ? std::string() : hit >= 0
                ? state.markers[static_cast<size_t> (hit)].name + " - click to jump, double-click to rename"
                : "Mini timeline - click to seek; double-click a marker to rename.";
            tooltip (text, mouse.x / scale, mouse.y / scale);
        }
    }

private:
    ImVec2 origin {}, size {};
    float scale = 1.0f;
    GlyphTally glyphs;

    static std::uint32_t markerColour (std::uint32_t c) { return (c >> 24) != 0 ? c : 0xffe0a050; }
    static std::uint32_t alpha (std::uint32_t c, std::uint32_t a) { return (c & 0x00ffffff) | (a << 24); }
    static std::uint32_t brighter (std::uint32_t c, float amount)
    {
        auto result = c & 0xff000000;
        for (unsigned shift : { 0u, 8u, 16u })
            result |= static_cast<std::uint32_t> (255.0f - (255 - ((c >> shift) & 255)) / (1.0f + amount)) << shift;
        return result;
    }
    static ImU32 rgba (std::uint32_t c) { return IM_COL32 ((c >> 16) & 255, (c >> 8) & 255, c & 255, c >> 24); }
    float xForSample (std::int64_t sample) const
    {
        const double fraction = state.end > 0
            ? std::clamp (static_cast<double> (sample) / static_cast<double> (state.end), 0.0, 1.0) : 0.0;
        return origin.x + scale * 6.0f + static_cast<float> (std::lround (fraction * std::max (1.0f, size.x - scale * 12.0f)));
    }
    std::int64_t sampleForX (float x) const
    {
        const double fraction = std::clamp (static_cast<double> (x - origin.x - scale * 6.0f)
                                           / std::max (1.0f, size.x - scale * 12.0f), 0.0, 1.0);
        return static_cast<std::int64_t> (fraction * static_cast<double> (state.end));
    }
    int markerAtX (float x) const
    {
        for (size_t i = 0; i < markerPaint.size(); ++i)
        {
            const auto& row = markerPaint[i];
            if (row.labelWidth > 0 && x >= static_cast<float> (row.labelX) - scale * 2.0f
                && x <= static_cast<float> (row.labelX + row.labelWidth) + scale * 2.0f)
                return static_cast<int> (i);
        }
        int best = -1;
        float distance = scale * 4.0f;
        for (size_t i = 0; i < state.markers.size(); ++i)
        {
            const float candidate = std::abs (xForSample (state.markers[i].sample) - x);
            if (candidate <= scale * 4.0f && (best < 0 || candidate < distance))
            { distance = candidate; best = static_cast<int> (i); }
        }
        return best;
    }
};
} // namespace duskstudio::imgui
