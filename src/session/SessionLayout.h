#pragma once

namespace duskstudio
{
struct SessionLayout
{
    static constexpr int kNumTracks = 24;
    static constexpr int kBankSize  = 8;
    static constexpr int kNumBanks  = kNumTracks / kBankSize;
};
} // namespace duskstudio
