#pragma once

#include "../../engine/sfz/SfzArtworkCache.h"
#include "../../engine/sfz/SfzInstrumentCategory.h"

namespace duskstudio::imgui
{
const char* instrumentArtworkResource (sfz::InstrumentCategory, std::string_view name = {}, std::string_view context = {});
std::string_view instrumentArtwork (sfz::InstrumentCategory, std::string_view name = {}, std::string_view context = {});
const char* instrumentArtworkCredit (sfz::InstrumentCategory, std::string_view name = {}, std::string_view context = {});
std::shared_ptr<const sfz::ArtworkPixels> decodeInstrumentArtwork (std::string_view);
} // namespace duskstudio::imgui
