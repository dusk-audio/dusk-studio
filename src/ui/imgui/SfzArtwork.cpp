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
    { "sfz_piano_png", "Photo: © Copyright Steinway & Sons; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
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
    { "sfz_mallets_png", "Marimbaone; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_synth_png", "Leo-setä; Clusternote; CC BY 2.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_voice_png", "Lucasbosch; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_world_png", "Sathyadeep; Public domain. Sources and adaptations: LICENSES.txt." },
    { "sfz_collection_png", "Photo: © Copyright Steinway & Sons; Cornergraf; Mfaiiazi; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." },
    { "sfz_collection_png", "Photo: © Copyright Steinway & Sons; Cornergraf; Mfaiiazi; CC BY-SA 3.0. Sources and adaptations: LICENSES.txt." }
};
static_assert (sizeof (kImages) / sizeof (kImages[0]) == sfz::kInstrumentCategories.size());
} // namespace

std::string_view instrumentArtwork (sfz::InstrumentCategory category)
{
    int bytes = 0;
    const auto* data = BinaryData::getNamedResource (kImages[static_cast<std::size_t> (category)].resource, bytes);
    return data != nullptr && bytes > 0 ? std::string_view (data, static_cast<std::size_t> (bytes)) : std::string_view {};
}

const char* instrumentArtworkCredit (sfz::InstrumentCategory category)
{
    return kImages[static_cast<std::size_t> (category)].credit;
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
