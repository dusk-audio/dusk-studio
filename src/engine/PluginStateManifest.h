#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <vector>

namespace duskstudio
{
// Which plug-in state an insert slot holds: the owner (a load, unload or
// restore moves the generation) and how far that owner's persistent state has
// moved since (the revision only grows for one owner).
struct PluginStateToken
{
    std::uint64_t generation = 0;
    std::uint64_t revision = 0;
    bool occupied = false;
};

// A slot empty in both reads holds no state to lose, however often it was
// filled and emptied in between; the session JSON still names what it holds.
inline bool pluginStateMoved (const PluginStateToken& baseline, const PluginStateToken& live) noexcept
{
    if (! baseline.occupied && ! live.occupied) return false;
    return baseline.generation != live.generation || baseline.revision != live.revision;
}

// A deferred restore that completed after its baseline was taken: the slot was
// empty at generationBefore and now holds the restored state at token.
struct PluginStateReceipt
{
    std::size_t slot = 0;
    std::uint64_t generationBefore = 0;
    PluginStateToken token;
};

template <std::size_t Slots>
struct PluginStateManifest
{
    std::uint64_t epoch = 0;
    std::size_t receipts = 0;
    std::array<PluginStateToken, Slots> tokens {};
    // Slots whose live state the record could not capture, so the session
    // saved with it holds an earlier state for them.
    std::array<bool, Slots> uncaptured {};

    // Adopting a record keeps the previous baseline's token for every slot it
    // could not capture: a change since that baseline stays unsaved.
    void keepUncapturedFrom (const PluginStateManifest& previous) noexcept
    {
        for (std::size_t slot = 0; slot < Slots; ++slot)
            if (uncaptured[slot]) tokens[slot] = previous.tokens[slot];
        uncaptured = {};
    }

    // Receipts from another session's epoch never apply, and a slot that has
    // changed hands since the baseline keeps its own token.
    void adopt (std::uint64_t currentEpoch, const std::vector<PluginStateReceipt>& all) noexcept
    {
        if (epoch != currentEpoch) return;
        for (; receipts < all.size(); ++receipts)
        {
            const auto& receipt = all[receipts];
            if (receipt.slot >= Slots) continue;
            auto& token = tokens[receipt.slot];
            if (! token.occupied && token.generation == receipt.generationBefore)
                token = receipt.token;
        }
    }

    bool differsFrom (const std::array<PluginStateToken, Slots>& live) const noexcept
    {
        for (std::size_t slot = 0; slot < Slots; ++slot)
            if (pluginStateMoved (tokens[slot], live[slot])) return true;
        return false;
    }
};
} // namespace duskstudio
