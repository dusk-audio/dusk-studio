#pragma once

#include <DuskWidgets.hpp>

namespace duskstudio::imgui
{
// One RGBA bitmap living in a view's slice of the font atlas. The pixels are the
// caller's and are only read during rasterise.
class AtlasImage
{
public:
    // `rgba` is width * height * 4 bytes, straight RGBA. Nothing is copied.
    void reserve (ImFontAtlas& atlas, const unsigned char* rgba, int width, int height);
    void rasterise (ImFontAtlas& atlas);

    bool ready() const noexcept;
    ImTextureID textureId() const noexcept;
    ImVec2 uvMin() const noexcept { return uv[0]; }
    ImVec2 uvMax() const noexcept { return uv[1]; }

    // Draw it into `tl`..`br`, letterboxed so the source aspect is kept.
    void draw (ImDrawList& dl, ImVec2 tl, ImVec2 br) const;

private:
    const ImFontAtlas* source = nullptr;
    const unsigned char* pixels = nullptr;
    ImVec2 uv[2] = {};
    int rect = -1;
    int imageWidth = 0;
    int imageHeight = 0;
};
} // namespace duskstudio::imgui
