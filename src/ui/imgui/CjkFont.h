#pragma once

#include <filesystem>
#include <memory>
#include <vector>

namespace duskstudio::imgui
{
struct CjkGlyph
{
    unsigned int codepoint = 0;
    int width = 0, height = 0, left = 0, top = 0;
    float advance = 0;
    std::vector<unsigned char> alpha;
};

bool isCjkCodepoint (unsigned int codepoint);

class CjkFont
{
public:
    explicit CjkFont (const std::filesystem::path& file = {});
    ~CjkFont();
    bool rasterize (unsigned int codepoint, float pixels, CjkGlyph& glyph);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};
} // namespace duskstudio::imgui
