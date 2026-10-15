#include "AtlasImage.h"
#include <OpenGL.hpp>

#include <cstddef>

namespace duskstudio::imgui
{
void AtlasImage::reserve (ImFontAtlas& atlas, const unsigned char* rgba, int width,
                          int height)
{
    source = nullptr;
    rect = -1;
    pixels = rgba;
    imageWidth = width;
    imageHeight = height;
    if (rgba == nullptr || width < 1 || height < 1)
        return;
    rect = atlas.AddCustomRectRegular (width, height);
}

void AtlasImage::rasterise (ImFontAtlas& atlas)
{
    if (rect < 0 || pixels == nullptr)
        return;
    const ImFontAtlasCustomRect* const packed = atlas.GetCustomRectByIndex (rect);
    if (packed == nullptr || ! packed->IsPacked())
        return;

    unsigned char* texturePixels = nullptr;
    int textureWidth = 0, textureHeight = 0;
    atlas.GetTexDataAsRGBA32 (&texturePixels, &textureWidth, &textureHeight);
    if (texturePixels == nullptr)
        return;

    for (int row = 0; row < imageHeight; ++row)
    {
        auto* const destination = reinterpret_cast<unsigned int*> (texturePixels)
                                + (packed->Y + row) * textureWidth + packed->X;
        const unsigned char* sourceRow = pixels
                                       + static_cast<std::size_t> (row) * static_cast<std::size_t> (imageWidth) * 4u;
        for (int column = 0; column < imageWidth; ++column, sourceRow += 4)
            destination[column] = IM_COL32 (sourceRow[0], sourceRow[1], sourceRow[2],
                                            sourceRow[3]);
    }

    atlas.CalcCustomRectUV (packed, &uv[0], &uv[1]);
    source = &atlas;
}

bool AtlasImage::ready() const noexcept
{
    return source != nullptr && source->TexID != ImTextureID();
}

void AtlasImage::update (const unsigned char* rgba)
{
    if (! ready() || rgba == nullptr) return;
    pixels = rgba;
    rasterise (*source);
    const auto* packed = source->GetCustomRectByIndex (rect);
    unsigned char* data = nullptr;
    int width = 0, height = 0;
    source->GetTexDataAsRGBA32 (&data, &width, &height);
    if (packed == nullptr || data == nullptr) return;
    GLint binding = 0, alignment = 4, rowLength = 0;
    glGetIntegerv (GL_TEXTURE_BINDING_2D, &binding);
    glGetIntegerv (GL_UNPACK_ALIGNMENT, &alignment);
    glGetIntegerv (GL_UNPACK_ROW_LENGTH, &rowLength);
    glBindTexture (GL_TEXTURE_2D, static_cast<GLuint> (source->TexID));
    glPixelStorei (GL_UNPACK_ALIGNMENT, 1);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, width);
    glTexSubImage2D (GL_TEXTURE_2D, 0, packed->X, packed->Y, imageWidth, imageHeight,
                     GL_RGBA, GL_UNSIGNED_BYTE, data + (static_cast<std::size_t> (packed->Y) * width + packed->X) * 4);
    glPixelStorei (GL_UNPACK_ROW_LENGTH, rowLength);
    glPixelStorei (GL_UNPACK_ALIGNMENT, alignment);
    glBindTexture (GL_TEXTURE_2D, static_cast<GLuint> (binding));
}

ImTextureID AtlasImage::textureId() const noexcept
{
    return source != nullptr ? source->TexID : ImTextureID();
}

void AtlasImage::draw (ImDrawList& dl, ImVec2 tl, ImVec2 br) const
{
    if (! ready() || imageWidth < 1 || imageHeight < 1)
        return;

    const float boxW = br.x - tl.x;
    const float boxH = br.y - tl.y;
    const float aspect = static_cast<float> (imageWidth) / static_cast<float> (imageHeight);
    float drawW = boxW;
    float drawH = boxW / aspect;
    if (drawH > boxH)
    {
        drawH = boxH;
        drawW = boxH * aspect;
    }
    const ImVec2 at (tl.x + (boxW - drawW) * 0.5f, tl.y + (boxH - drawH) * 0.5f);
    dl.AddImage (textureId(), at, ImVec2 (at.x + drawW, at.y + drawH), uv[0], uv[1]);
}
} // namespace duskstudio::imgui
