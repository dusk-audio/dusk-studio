#include "SfzArtwork.h"
#include <BinaryData.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <cstring>
#include <new>

namespace
{
constexpr std::size_t kInitialDecoderBytes = 16 * 1024 * 1024;
constexpr std::size_t kMaxDecoderBytes = 512 * 1024 * 1024;
struct ArtworkDecodeBudget;
thread_local ArtworkDecodeBudget* artworkBudget = nullptr;

struct ArtworkDecodeBudget
{
    ArtworkDecodeBudget() noexcept : previous (artworkBudget) { artworkBudget = this; }
    ~ArtworkDecodeBudget() { artworkBudget = previous; }
    ArtworkDecodeBudget (const ArtworkDecodeBudget&) = delete;
    ArtworkDecodeBudget& operator= (const ArtworkDecodeBudget&) = delete;

    std::size_t limit = kInitialDecoderBytes, used = 0;
    ArtworkDecodeBudget* previous;
};

struct alignas (std::max_align_t) ArtworkAllocation
{
    std::size_t bytes;
    ArtworkDecodeBudget* owner;
};
constexpr auto kAllocationAlignment = std::max<std::size_t> (16, alignof (std::max_align_t));
constexpr auto kAllocationHeaderBytes = (sizeof (ArtworkAllocation) + kAllocationAlignment - 1)
                                     / kAllocationAlignment * kAllocationAlignment;

void* allocateArtwork (std::size_t bytes) noexcept
{
    auto* budget = artworkBudget;
    if (budget == nullptr) return nullptr;
    const auto available = budget->limit - budget->used;
    if (bytes > available || kAllocationHeaderBytes > available - bytes) return nullptr;
    const auto total = bytes + kAllocationHeaderBytes;
    auto* storage = std::malloc (total);
    if (storage == nullptr) return nullptr;
    new (storage) ArtworkAllocation { total, budget };
    budget->used += total;
    return static_cast<char*> (storage) + kAllocationHeaderBytes;
}

void freeArtwork (void* memory) noexcept
{
    if (memory == nullptr) return;
    auto* allocation = reinterpret_cast<ArtworkAllocation*> (static_cast<char*> (memory) - kAllocationHeaderBytes);
    allocation->owner->used -= allocation->bytes;
    std::free (allocation);
}

void* reallocateArtwork (void* memory, std::size_t bytes) noexcept
{
    auto* replacement = allocateArtwork (bytes);
    if (replacement == nullptr) return nullptr;
    if (memory != nullptr)
    {
        const auto* allocation = reinterpret_cast<ArtworkAllocation*> (static_cast<char*> (memory) - kAllocationHeaderBytes);
        std::memcpy (replacement, memory, std::min (bytes, allocation->bytes - kAllocationHeaderBytes));
        freeArtwork (memory);
    }
    return replacement;
}
} // namespace

#define STB_IMAGE_IMPLEMENTATION
#define STB_IMAGE_STATIC
#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_NO_STDIO
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC(bytes) allocateArtwork (bytes)
#define STBI_REALLOC_SIZED(memory, oldBytes, newBytes) reallocateArtwork (memory, newBytes)
#define STBI_FREE(memory) freeArtwork (memory)
#if defined (__GNUC__) || defined (__clang__)
 #pragma GCC diagnostic push
 #pragma GCC diagnostic ignored "-Wunused-function"
#endif
#include "../../../external/stb/stb_image.h"
#if defined (__GNUC__) || defined (__clang__)
 #pragma GCC diagnostic pop
#endif
#undef STBI_MALLOC
#undef STBI_REALLOC_SIZED
#undef STBI_FREE

namespace duskstudio::imgui
{
namespace
{
struct ImageResource { const char* resource; const char* credit; };
constexpr ImageResource kImages[] {
    { "sfz_piano_png", "TheresaKnott; CC0-1.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_electric_piano_png", "Docrobbie; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_organ_png", "Dongui; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_drums_png", "Mfaiiazi; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_percussion_png", "Nicole Gordine; CC BY 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_acoustic_guitar_png", "Jim Hutcheson; CC BY 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_electric_guitar_png", "Patrick Mueller; CC BY 2.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_bass_png", "Patrick Scales; Djhé; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_strings_png", "Cornergraf; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_brass_png", "BenP; CC BY 2.5. Sources and adaptations: LICENSES.txt." },
    { "sfz_woodwinds_png", "Yamaha Corporation; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_mallets_png", "The Metropolitan Museum of Art; CC0-1.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_synth_png", "Leo-setä; Clusternote; CC BY 2.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_voice_png", "Zzubnik; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_world_png", "Sathyadeep; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_collection_png", "TheresaKnott; Cornergraf; Mfaiiazi; Dusk Audio; CC0-1.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_collection_png", "TheresaKnott; Cornergraf; Mfaiiazi; Dusk Audio; CC0-1.0. Sources and adaptations: LICENSES.txt." }
};
static_assert (sizeof (kImages) / sizeof (kImages[0]) == sfz::kInstrumentCategories.size());

constexpr ImageResource kDetailImages[] {
    { "sfz_upright_bass_png", "papapishu; CC0-1.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_harp_png", "Museum f\u00fcr Kunst und Gewerbe Hamburg; CC0-1.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_cello_png", "Georg Feitscher; CC BY 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_banjo_png", "Zwitt95; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_mandolin_png", "Arik Kerman; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_ukulele_png", "Kollektives Schreiben; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_accordion_png", "ArtFavor; CC0-1.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_harmonica_png", "Sarah A Morrigan / missiridia; CC0-1.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_saxophone_png", "File:Yamaha Saxophone YAS-62.tif: Yamaha Corporation; remove backround: Habitator terrae; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_clarinet_png", "File:Yamaha Clarinet YCL-457II-22.tiff: Yamaha Corporation; remove backround: Habitator terrae; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_bassoon_png", "Yamaha Corporation; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_oboe_png", "man ichihara ; Photographed and edited by fi:K\u00e4ytt\u00e4j\u00e4:Aquazer.; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_trumpet_png", "PJ, background cropped by EWikist; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_trombone_png", "Debbie Rawson (photograph); Jonathanischoice (uploader); funded by Wikimedia Aotearoa New Zealand; CC BY 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_tuba_png", "Buffet Crampon; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_harpsichord_png", "FREYmanagement; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_vibraphone_png", "Xylosmygame at English Wikipedia; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_glockenspiel_png", "flamurai; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_steelpan_png", "J dahlia; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_handpan_png", "Pensierarte; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_koto_png", "Got\u014d Teij\u014d / Got\u014d Y\u016bj\u014d; CC0-1.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_tubular_bells_png", "Xylosmygame at English Wikipedia; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_handbell_png", "M0tty; CC BY-SA 4.0. Sources and adaptations: LICENSES.txt." },
};

const ImageResource& artworkEntry (sfz::InstrumentCategory category, std::string_view name, std::string_view context)
{
    using C = sfz::InstrumentCategory;
    const auto match = [&] (std::string_view value) -> const ImageResource*
    {
        const auto words = sfz::instrumentWords (value);
        const auto has = [&] (std::initializer_list<const char*> terms) { return sfz::instrumentWordsContain (words, terms); };
        switch (category)
        {
            case C::Bass:
                if (has ({ "upright bass", "double bass", "contrabass" })) return &kDetailImages[0];
                break;
            case C::Strings:
                if (has ({ "harp" })) return &kDetailImages[1];
                if (has ({ "cello", "violoncello" })) return &kDetailImages[2];
                break;
            case C::AcousticGuitar:
                if (has ({ "banjo", "ganjo" })) return &kDetailImages[3];
                if (has ({ "mandolin" })) return &kDetailImages[4];
                if (has ({ "ukulele", "uke" })) return &kDetailImages[5];
                break;
            case C::World:
                if (has ({ "accordion", "bandoneon", "concertina" })) return &kDetailImages[6];
                if (has ({ "harmonica", "mouth organ" })) return &kDetailImages[7];
                if (has ({ "koto" })) return &kDetailImages[20];
                break;
            case C::Woodwinds:
                if (has ({ "saxophone", "sax" })) return &kDetailImages[8];
                if (has ({ "clarinet" })) return &kDetailImages[9];
                if (has ({ "bassoon" })) return &kDetailImages[10];
                if (has ({ "oboe", "english horn", "cor anglais" })) return &kDetailImages[11];
                break;
            case C::Brass:
                if (has ({ "trumpet", "cornet", "flugelhorn" })) return &kDetailImages[12];
                if (has ({ "trombone" })) return &kDetailImages[13];
                if (has ({ "tuba", "euphonium" })) return &kDetailImages[14];
                break;
            case C::Piano:
                if (has ({ "harpsichord", "spinet" })) return &kDetailImages[15];
                break;
            case C::Mallets:
                if (has ({ "vibraphone", "vibes" })) return &kDetailImages[16];
                if (has ({ "glockenspiel", "glock" })) return &kDetailImages[17];
                if (has ({ "tubular bell", "chime", "carillon" })) return &kDetailImages[21];
                if (has ({ "bell", "handbell" })) return &kDetailImages[22];
                break;
            case C::Percussion:
                if (has ({ "handpan", "hang drum" })) return &kDetailImages[19];
                if (has ({ "steelpan", "steel pan", "steel drum" })) return &kDetailImages[18];
                break;
            case C::ElectricPiano: case C::Organ: case C::Drums: case C::ElectricGuitar:
            case C::Synth: case C::Voice: case C::Collection: case C::Other: break;
        }
        return nullptr;
    };
    if (const auto* image = match (name)) return *image;
    if (sfz::instrumentCategory (name) == C::Other)
        if (const auto* image = match (context)) return *image;
    if (category == C::World && ! name.empty() && ! sfz::instrumentWordsContain (sfz::instrumentWords (name), { "sitar" }))
        return kImages[static_cast<std::size_t> (C::Collection)];
    return kImages[static_cast<std::size_t> (category)];
}
} // namespace

const char* instrumentArtworkResource (sfz::InstrumentCategory category, std::string_view name, std::string_view context)
{
    return artworkEntry (category, name, context).resource;
}

std::string_view instrumentArtwork (sfz::InstrumentCategory category, std::string_view name, std::string_view context)
{
    int bytes = 0;
    const auto* data = BinaryData::getNamedResource (artworkEntry (category, name, context).resource, bytes);
    return data != nullptr && bytes > 0 ? std::string_view (data, static_cast<std::size_t> (bytes)) : std::string_view {};
}

const char* instrumentArtworkCredit (sfz::InstrumentCategory category, std::string_view name, std::string_view context)
{
    return artworkEntry (category, name, context).credit;
}

std::shared_ptr<const sfz::ArtworkPixels> decodeInstrumentArtwork (std::string_view bytes)
{
    if (bytes.empty() || bytes.size() > 4 * 1024 * 1024) return {};
    ArtworkDecodeBudget budget;
    const auto* data = reinterpret_cast<const stbi_uc*> (bytes.data());
    int width = 0, height = 0, channels = 0;
    if (! stbi_info_from_memory (data, static_cast<int> (bytes.size()), &width, &height, &channels)
        || width < 1 || height < 1 || width > 8192 || height > 8192
        || static_cast<std::uint64_t> (width) * static_cast<std::uint64_t> (height) > 16 * 1024 * 1024) return {};
    // PNG inflation can grow beyond the dimensions declared by its header.
    budget.limit = std::min (kMaxDecoderBytes, kInitialDecoderBytes + static_cast<std::size_t> (width) * height * 32);
    const std::unique_ptr<stbi_uc, decltype (&stbi_image_free)> image (
        stbi_load_from_memory (data, static_cast<int> (bytes.size()), &width, &height, &channels, 4), stbi_image_free);
    if (! image) return {};
    auto output = std::make_shared<sfz::ArtworkPixels>();
    output->width = 512; output->height = 320;
    output->rgba.resize (512 * 320 * 4);
    for (std::size_t pixel = 0; pixel < output->rgba.size(); pixel += 4)
    {
        output->rgba[pixel] = 24; output->rgba[pixel + 1] = 27;
        output->rgba[pixel + 2] = 32; output->rgba[pixel + 3] = 255;
    }
    const double scale = std::min (500.0 / width, 308.0 / height);
    const int drawWidth = std::max (1, static_cast<int> (std::lround (width * scale)));
    const int drawHeight = std::max (1, static_cast<int> (std::lround (height * scale)));
    const int left = (512 - drawWidth) / 2, top = (320 - drawHeight) / 2;
    for (int y = 0; y < drawHeight; ++y)
        for (int x = 0; x < drawWidth; ++x)
        {
            const double sourceX = std::clamp ((x + 0.5) / scale - 0.5, 0.0, static_cast<double> (width - 1));
            const double sourceY = std::clamp ((y + 0.5) / scale - 0.5, 0.0, static_cast<double> (height - 1));
            const int x0 = static_cast<int> (sourceX), y0 = static_cast<int> (sourceY);
            const int x1 = std::min (x0 + 1, width - 1), y1 = std::min (y0 + 1, height - 1);
            const auto component = [&] (int channel)
            {
                const auto sample = [&] (int sx, int sy)
                {
                    const auto* pixel = image.get() + (static_cast<std::size_t> (sy) * static_cast<std::size_t> (width) + static_cast<std::size_t> (sx)) * 4;
                    return channel == 3 ? static_cast<double> (pixel[3]) : pixel[channel] * pixel[3] / 255.0;
                };
                const double a = sample (x0, y0) * (1 - sourceX + x0) + sample (x1, y0) * (sourceX - x0);
                const double b = sample (x0, y1) * (1 - sourceX + x0) + sample (x1, y1) * (sourceX - x0);
                return a * (1 - sourceY + y0) + b * (sourceY - y0);
            };
            auto* target = output->rgba.data() + (static_cast<std::size_t> (top + y) * 512 + static_cast<std::size_t> (left + x)) * 4;
            const double alpha = component (3) / 255.0;
            for (int channel = 0; channel < 3; ++channel)
                target[channel] = static_cast<unsigned char> (std::lround (component (channel) + target[channel] * (1 - alpha)));
        }
    return output;
}
} // namespace duskstudio::imgui
