#pragma once

#include "../../engine/sfz/SfzArtworkCache.h"
#include "../../engine/sfz/SfzInstrumentCategory.h"

namespace duskstudio::imgui
{
std::string_view instrumentArtwork (sfz::InstrumentCategory);
const char* instrumentArtworkCredit (sfz::InstrumentCategory);
std::shared_ptr<const sfz::ArtworkPixels> decodeInstrumentArtwork (std::string_view);
} // namespace duskstudio::imgui
