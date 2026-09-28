#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <string>
#include <string_view>
#include <unordered_map>

namespace duskstudio::lv2
{
// The URID map/unmap behind one plugin instance. Plugins call map from their
// worker thread, the UI and the message thread at once, and some from run()
// too, so looking up a URI that is already mapped - nearly every call after
// instantiate - takes no lock and allocates nothing: entries are append-only,
// published into a linear-probe table with release stores and never moved or
// removed. Only a first-time URI takes the lock to append itself. Past
// kLockFreeIds URIs the rest live in a locked overflow map.
class Lv2UridMap
{
public:
    static constexpr uint32_t kLockFreeIds = 2048;

    uint32_t map (const char* uri)
    {
        if (uri == nullptr) return 0;
        const std::string_view key (uri);
        const size_t hash = std::hash<std::string_view> {} (key);
        if (const Entry* e = find (key, hash)) return e->id;

        const std::lock_guard<std::mutex> lock (writeLock);
        if (const Entry* e = find (key, hash)) return e->id;
        if (auto it = overflow.find (key); it != overflow.end()) return it->second->id;

        const auto id = (uint32_t) entries.size() + 1;
        entries.push_back ({ std::string (key), id });
        const Entry* added = &entries.back();
        if (id <= kLockFreeIds)
        {
            for (size_t probe = 0;; ++probe)
            {
                auto& slot = slots[(hash + probe) & kSlotMask];
                if (slot.load (std::memory_order_relaxed) == nullptr)
                {
                    slot.store (added, std::memory_order_release);
                    break;
                }
            }
            byId[id].store (added, std::memory_order_release);
        }
        else
        {
            overflow.emplace (added->uri, added);
        }
        return id;
    }

    // The returned string stays valid for this map's lifetime.
    const char* unmap (uint32_t id)
    {
        if (id == 0) return nullptr;
        if (id <= kLockFreeIds)
        {
            const Entry* e = byId[id].load (std::memory_order_acquire);
            return e != nullptr ? e->uri.c_str() : nullptr;
        }
        const std::lock_guard<std::mutex> lock (writeLock);
        return id <= entries.size() ? entries[id - 1].uri.c_str() : nullptr;
    }

private:
    struct Entry { std::string uri; uint32_t id; };

    // Twice the lock-free id count, so the table never runs past half full.
    static constexpr size_t kSlots = 2 * (size_t) kLockFreeIds;
    static constexpr size_t kSlotMask = kSlots - 1;

    const Entry* find (std::string_view key, size_t hash) const noexcept
    {
        for (size_t probe = 0; probe < kSlots; ++probe)
        {
            const Entry* e = slots[(hash + probe) & kSlotMask].load (std::memory_order_acquire);
            if (e == nullptr) return nullptr;
            if (e->uri == key) return e;
        }
        return nullptr;
    }

    std::array<std::atomic<const Entry*>, kSlots> slots {};
    std::array<std::atomic<const Entry*>, kLockFreeIds + 1> byId {};
    std::mutex writeLock;
    std::deque<Entry> entries;   // writeLock; element addresses never change
    std::unordered_map<std::string_view, const Entry*> overflow;   // writeLock
};
} // namespace duskstudio::lv2
