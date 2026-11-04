#include "CjkFont.h"

#if defined (__linux__)
 #include <fontconfig/fontconfig.h>
 #include <ft2build.h>
 #include FT_FREETYPE_H
#elif defined (__APPLE__)
 #include <CoreText/CoreText.h>
#elif defined (_WIN32)
 #include <dwrite.h>
 #include <wrl/client.h>
#endif

#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace duskstudio::imgui
{
bool isCjkCodepoint (unsigned int c)
{
    return (c >= 0x2e80 && c <= 0x9fff) || (c >= 0xac00 && c <= 0xd7af)
        || (c >= 0xf900 && c <= 0xfaff) || (c >= 0xff00 && c <= 0xffef)
        || (c >= 0x1100 && c <= 0x11ff);
}

#if defined (__linux__)
struct CjkFont::Impl
{
    FT_Library library = nullptr;
    FcConfig* config = nullptr;
    std::vector<FT_Face> faces;
    std::set<std::pair<std::string, int>> opened;
    bool system = false;

    explicit Impl (const std::filesystem::path& file) : system (file.empty())
    {
        if (FT_Init_FreeType (&library) != 0) return;
        if (system) config = FcInitLoadConfigAndFonts();
        else open (file.u8string(), 0);
    }

    ~Impl()
    {
        for (auto face : faces) FT_Done_Face (face);
        if (config != nullptr) FcConfigDestroy (config);
        if (library != nullptr) FT_Done_FreeType (library);
    }

    void open (const std::string& file, int index)
    {
        if (! opened.emplace (file, index).second) return;
        FT_Face face = nullptr;
        if (library != nullptr && FT_New_Face (library, file.c_str(), index, &face) == 0)
            faces.push_back (face);
    }

    FT_Face find (unsigned int codepoint)
    {
        for (auto face : faces)
            if (FT_Get_Char_Index (face, codepoint) != 0) return face;
        if (config == nullptr) return nullptr;
        auto* pattern = FcPatternCreate();
        auto* charset = FcCharSetCreate();
        FcCharSetAddChar (charset, codepoint);
        FcPatternAddCharSet (pattern, FC_CHARSET, charset);
        FcPatternAddString (pattern, FC_FAMILY, reinterpret_cast<const FcChar8*> ("sans-serif"));
        FcPatternAddBool (pattern, FC_SCALABLE, FcTrue);
        FcConfigSubstitute (config, pattern, FcMatchPattern);
        FcDefaultSubstitute (pattern);
        FcResult result;
        auto* match = FcFontMatch (config, pattern, &result);
        FcChar8* file = nullptr;
        int index = 0;
        if (match != nullptr && FcPatternGetString (match, FC_FILE, 0, &file) == FcResultMatch)
        {
            FcPatternGetInteger (match, FC_INDEX, 0, &index);
            open (reinterpret_cast<const char*> (file), index);
        }
        if (match != nullptr) FcPatternDestroy (match);
        FcCharSetDestroy (charset);
        FcPatternDestroy (pattern);
        for (auto face : faces)
            if (FT_Get_Char_Index (face, codepoint) != 0) return face;
        return nullptr;
    }

    bool rasterize (unsigned int codepoint, float pixels, CjkGlyph& out)
    {
        auto face = find (codepoint);
        if (face == nullptr || FT_Set_Pixel_Sizes (face, 0, static_cast<FT_UInt> (std::ceil (pixels))) != 0
            || FT_Load_Char (face, codepoint, FT_LOAD_RENDER | FT_LOAD_TARGET_NORMAL) != 0)
            return false;
        const auto& bitmap = face->glyph->bitmap;
        if (bitmap.pixel_mode != FT_PIXEL_MODE_GRAY && bitmap.pixel_mode != FT_PIXEL_MODE_MONO)
            return false;
        out.width = static_cast<int> (bitmap.width);
        out.height = static_cast<int> (bitmap.rows);
        out.left = face->glyph->bitmap_left;
        out.top = -face->glyph->bitmap_top;
        out.advance = static_cast<float> (face->glyph->advance.x) / 64.0f;
        out.alpha.resize (static_cast<std::size_t> (out.width * out.height));
        for (int y = 0; y < out.height; ++y)
        {
            const auto* row = bitmap.buffer + (bitmap.pitch >= 0 ? y : out.height - 1 - y) * std::abs (bitmap.pitch);
            for (int x = 0; x < out.width; ++x)
                out.alpha[static_cast<std::size_t> (y * out.width + x)] = bitmap.pixel_mode == FT_PIXEL_MODE_GRAY
                    ? row[x] : static_cast<unsigned char> ((row[x / 8] & (0x80 >> (x % 8))) != 0 ? 255 : 0);
        }
        return true;
    }
};
#elif defined (__APPLE__)
struct CjkFont::Impl
{
    CTFontRef base = nullptr;
    bool system = false;
    explicit Impl (const std::filesystem::path& file) : system (file.empty())
    {
        if (system) base = CTFontCreateWithName (CFSTR("Helvetica"), 13, nullptr);
        else if (auto data = CGDataProviderCreateWithFilename (file.c_str()))
        {
            if (auto font = CGFontCreateWithDataProvider (data))
            {
                base = CTFontCreateWithGraphicsFont (font, 13, nullptr, nullptr);
                CGFontRelease (font);
            }
            CGDataProviderRelease (data);
        }
    }
    ~Impl() { if (base != nullptr) CFRelease (base); }

    bool rasterize (unsigned int codepoint, float pixels, CjkGlyph& out)
    {
        if (base == nullptr) return false;
        const UniChar character = static_cast<UniChar> (codepoint);
        auto text = CFStringCreateWithCharacters (nullptr, &character, 1);
        auto fallback = system ? CTFontCreateForString (base, text, CFRangeMake (0, 1))
                               : static_cast<CTFontRef> (CFRetain (base));
        CFRelease (text);
        if (fallback == nullptr) return false;
        auto font = CTFontCreateCopyWithAttributes (fallback, pixels, nullptr, nullptr);
        CFRelease (fallback);
        if (font == nullptr) return false;
        CGGlyph glyph = 0;
        if (! CTFontGetGlyphsForCharacters (font, &character, &glyph, 1) || glyph == 0)
        {
            CFRelease (font);
            return false;
        }
        const auto bounds = CTFontGetBoundingRectsForGlyphs (font, kCTFontOrientationDefault, &glyph, nullptr, 1);
        CGSize advance {};
        CTFontGetAdvancesForGlyphs (font, kCTFontOrientationDefault, &glyph, &advance, 1);
        out.left = static_cast<int> (std::floor (bounds.origin.x));
        out.top = -static_cast<int> (std::ceil (bounds.origin.y + bounds.size.height));
        out.width = static_cast<int> (std::ceil (bounds.origin.x + bounds.size.width)) - out.left;
        out.height = -out.top - static_cast<int> (std::floor (bounds.origin.y));
        out.advance = static_cast<float> (advance.width);
        out.alpha.resize (static_cast<std::size_t> (out.width * out.height));
        if (out.width > 0 && out.height > 0)
        {
            auto space = CGColorSpaceCreateDeviceGray();
            auto context = CGBitmapContextCreate (out.alpha.data(), out.width, out.height, 8, out.width,
                                                   space, kCGImageAlphaNone);
            CGColorSpaceRelease (space);
            if (context == nullptr) { CFRelease (font); return false; }
            CGContextSetGrayFillColor (context, 1, 1);
            const CGPoint position { static_cast<CGFloat> (-out.left), static_cast<CGFloat> (out.height + out.top) };
            CTFontDrawGlyphs (font, &glyph, &position, 1, context);
            CGContextRelease (context);
        }
        CFRelease (font);
        return true;
    }
};
#elif defined (_WIN32)
using Microsoft::WRL::ComPtr;
struct CjkFont::Impl
{
    ComPtr<IDWriteFactory> factory;
    ComPtr<IDWriteFontCollection> collection;
    std::vector<ComPtr<IDWriteFontFace>> faces;
    explicit Impl (const std::filesystem::path& file)
    {
        if (FAILED (DWriteCreateFactory (DWRITE_FACTORY_TYPE_SHARED, __uuidof (IDWriteFactory),
                                         reinterpret_cast<IUnknown**> (factory.GetAddressOf())))) return;
        if (file.empty()) factory->GetSystemFontCollection (&collection);
        else
        {
            ComPtr<IDWriteFontFile> fontFile;
            if (FAILED (factory->CreateFontFileReference (file.c_str(), nullptr, &fontFile))) return;
            BOOL supported = FALSE;
            DWRITE_FONT_FILE_TYPE fileType;
            DWRITE_FONT_FACE_TYPE faceType;
            UINT32 count = 0;
            if (FAILED (fontFile->Analyze (&supported, &fileType, &faceType, &count)) || ! supported || count == 0) return;
            auto* raw = fontFile.Get();
            ComPtr<IDWriteFontFace> face;
            if (SUCCEEDED (factory->CreateFontFace (faceType, 1, &raw, 0, DWRITE_FONT_SIMULATIONS_NONE, &face)))
                faces.push_back (std::move (face));
        }
    }

    IDWriteFontFace* find (unsigned int codepoint, UINT16& glyph)
    {
        const UINT32 character = codepoint;
        for (auto& face : faces)
            if (SUCCEEDED (face->GetGlyphIndices (&character, 1, &glyph)) && glyph != 0) return face.Get();
        if (collection == nullptr) return nullptr;
        for (UINT32 i = 0; i < collection->GetFontFamilyCount(); ++i)
        {
            ComPtr<IDWriteFontFamily> family;
            ComPtr<IDWriteFont> font;
            BOOL has = FALSE;
            if (FAILED (collection->GetFontFamily (i, &family))
                || FAILED (family->GetFirstMatchingFont (DWRITE_FONT_WEIGHT_NORMAL, DWRITE_FONT_STRETCH_NORMAL,
                                                          DWRITE_FONT_STYLE_NORMAL, &font))
                || FAILED (font->HasCharacter (character, &has)) || ! has) continue;
            ComPtr<IDWriteFontFace> face;
            if (SUCCEEDED (font->CreateFontFace (&face))
                && SUCCEEDED (face->GetGlyphIndices (&character, 1, &glyph)) && glyph != 0)
            {
                faces.push_back (std::move (face));
                return faces.back().Get();
            }
        }
        return nullptr;
    }

    bool rasterize (unsigned int codepoint, float pixels, CjkGlyph& out)
    {
        UINT16 glyph = 0;
        auto* face = find (codepoint, glyph);
        if (face == nullptr) return false;
        DWRITE_FONT_METRICS metrics {};
        DWRITE_GLYPH_METRICS glyphMetrics {};
        face->GetMetrics (&metrics);
        if (FAILED (face->GetDesignGlyphMetrics (&glyph, 1, &glyphMetrics)) || metrics.designUnitsPerEm == 0) return false;
        out.advance = pixels * static_cast<float> (glyphMetrics.advanceWidth) / metrics.designUnitsPerEm;
        DWRITE_GLYPH_RUN run {};
        run.fontFace = face;
        run.fontEmSize = pixels;
        run.glyphCount = 1;
        run.glyphIndices = &glyph;
        run.glyphAdvances = &out.advance;
        ComPtr<IDWriteGlyphRunAnalysis> analysis;
        if (FAILED (factory->CreateGlyphRunAnalysis (&run, 1, nullptr, DWRITE_RENDERING_MODE_NATURAL_SYMMETRIC,
                                                     DWRITE_MEASURING_MODE_NATURAL, 0, 0, &analysis))) return false;
        RECT bounds {};
        if (FAILED (analysis->GetAlphaTextureBounds (DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds))) return false;
        out.left = bounds.left;
        out.top = bounds.top;
        out.width = bounds.right - bounds.left;
        out.height = bounds.bottom - bounds.top;
        out.alpha.resize (static_cast<std::size_t> (out.width * out.height));
        if (! out.alpha.empty())
        {
            std::vector<unsigned char> rgb (out.alpha.size() * 3);
            if (FAILED (analysis->CreateAlphaTexture (DWRITE_TEXTURE_CLEARTYPE_3x1, &bounds, rgb.data(),
                                                      static_cast<UINT32> (rgb.size())))) return false;
            for (std::size_t i = 0; i < out.alpha.size(); ++i)
                out.alpha[i] = static_cast<unsigned char> ((unsigned (rgb[i * 3]) + rgb[i * 3 + 1] + rgb[i * 3 + 2]) / 3);
        }
        return true;
    }
};
#else
struct CjkFont::Impl
{
    explicit Impl (const std::filesystem::path&) {}
    bool rasterize (unsigned int, float, CjkGlyph&) { return false; }
};
#endif

CjkFont::CjkFont (const std::filesystem::path& file) : impl (std::make_unique<Impl> (file)) {}
CjkFont::~CjkFont() = default;

bool CjkFont::rasterize (unsigned int codepoint, float pixels, CjkGlyph& glyph)
{
    glyph = {};
    glyph.codepoint = codepoint;
    return isCjkCodepoint (codepoint) && std::isfinite (pixels) && pixels >= 1 && pixels <= 128
        && impl->rasterize (codepoint, pixels, glyph);
}
} // namespace duskstudio::imgui
