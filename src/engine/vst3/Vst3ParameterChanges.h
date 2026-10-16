#pragma once

#include "../MidiPanic.h"

#include <pluginterfaces/vst/ivstparameterchanges.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace duskstudio::vst3::detail
{
constexpr std::size_t kEditorParameterRingSize = 512;
constexpr std::size_t kParameterPointCapacity = dusk::kMidiRoutingBlockBytes / dusk::MidiBuffer::recordBytes (2)
    + kEditorParameterRingSize - 1 + 2 * midi::kTransportStopResetMessageCount;
enum class ParameterPointPriority : std::uint8_t { Modulation, Chase, Reset };

template <std::size_t Capacity>
class FixedParameterChanges final : public Steinberg::Vst::IParameterChanges
{
    static_assert (Capacity > 0);
    struct Point
    {
        Steinberg::Vst::ParamID id;
        Steinberg::int32 offset;
        double value;
        std::uint64_t order;
        ParameterPointPriority priority;
    };
    struct Value
    {
        Steinberg::int32 offset;
        double value;
    };
    static constexpr std::size_t kTableCapacity = []
    {
        std::size_t size = 1;
        while (size < Capacity * 2) size *= 2;
        return size;
    }();
    static constexpr std::uint32_t kDeleted = UINT32_MAX;
    struct Lookup { std::size_t point, slot; };
    struct Queue final : Steinberg::Vst::IParamValueQueue
    {
        Steinberg::Vst::ParamID id = Steinberg::Vst::kNoParamId;
        const Value* values = nullptr;
        Steinberg::int32 count = 0;

        Steinberg::Vst::ParamID PLUGIN_API getParameterId() override { return id; }
        Steinberg::int32 PLUGIN_API getPointCount() override { return count; }
        Steinberg::tresult PLUGIN_API getPoint (Steinberg::int32 index, Steinberg::int32& offset, double& value) override
        {
            if (index < 0 || index >= count) return Steinberg::kResultFalse;
            offset = values[index].offset;
            value = values[index].value;
            return Steinberg::kResultTrue;
        }
        Steinberg::tresult PLUGIN_API addPoint (Steinberg::int32, double, Steinberg::int32& index) override
        { index = -1; return Steinberg::kResultFalse; }
        Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID iid, void** object) override
        {
            if (object == nullptr) return Steinberg::kInvalidArgument;
            *object = nullptr;
            if (! Steinberg::FUnknownPrivate::iidEqual (iid, Steinberg::Vst::IParamValueQueue::iid)
                && ! Steinberg::FUnknownPrivate::iidEqual (iid, Steinberg::FUnknown::iid)) return Steinberg::kNoInterface;
            *object = static_cast<Steinberg::Vst::IParamValueQueue*> (this);
            addRef();
            return Steinberg::kResultOk;
        }
        // Process-data views share their owner's lifetime; reference calls cannot free them.
        Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
        Steinberg::uint32 PLUGIN_API release() override { return 1; }
    };

public:
    FixedParameterChanges() = default;
    FixedParameterChanges (const FixedParameterChanges&) = delete;
    FixedParameterChanges& operator= (const FixedParameterChanges&) = delete;

    void beginBlock (int frames) noexcept
    {
        for (std::size_t i = 0; i < touchedCount; ++i) slots[touchedSlots[i]] = 0;
        touchedCount = 0;
        frameCount = frames;
        pointCount = queueCount = 0;
        finished = false;
    }

    bool append (Steinberg::Vst::ParamID id, int offset, double value,
                 std::uint64_t sourceOrder, ParameterPointPriority priority) noexcept
    {
        if (finished || offset < 0 || offset >= frameCount || id == Steinberg::Vst::kNoParamId) return false;
        const auto found = locate (id, offset);
        if (found.point != Capacity)
        {
            auto& existing = points[found.point];
            existing.priority = std::max (existing.priority, priority);
            if (sourceOrder >= existing.order)
            {
                existing.value = value;
                existing.order = sourceOrder;
            }
            return true;
        }
        const Point point { id, offset, value, sourceOrder, priority };
        auto destination = pointCount;
        if (pointCount == Capacity)
        {
            destination = Capacity;
            for (std::size_t i = 0; i < pointCount; ++i)
                if (points[i].priority < priority && (destination == Capacity
                    || points[i].priority < points[destination].priority
                    || (points[i].priority == points[destination].priority && points[i].order > points[destination].order)))
                    destination = i;
            if (destination == Capacity) return false;
            const auto& displaced = points[destination];
            slots[locate (displaced.id, displaced.offset).slot] = kDeleted;
        }
        else ++pointCount;
        points[destination] = point;
        if (slots[found.slot] == 0) touchedSlots[touchedCount++] = static_cast<std::uint32_t> (found.slot);
        slots[found.slot] = static_cast<std::uint32_t> (destination + 1);
        return true;
    }

    void finalize() noexcept
    {
        if (finished) return;
        for (std::size_t i = 0; i < pointCount; ++i) order[i] = i;
        std::sort (order.begin(), order.begin() + pointCount, [this] (std::size_t a, std::size_t b)
        {
            const auto& lhs = points[a];
            const auto& rhs = points[b];
            if (lhs.id != rhs.id) return lhs.id < rhs.id;
            return lhs.offset < rhs.offset;
        });
        std::size_t written = 0;
        for (std::size_t i = 0; i < pointCount; ++i)
        {
            const auto& point = points[order[i]];
            if (queueCount == 0 || queues[queueCount - 1].id != point.id)
            {
                auto& queue = queues[queueCount++];
                queue.id = point.id;
                queue.values = values.data() + written;
                queue.count = 0;
            }
            values[written++] = { point.offset, point.value };
            ++queues[queueCount - 1].count;
        }
        finished = true;
    }

    Steinberg::int32 PLUGIN_API getParameterCount() override { return static_cast<Steinberg::int32> (queueCount); }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API getParameterData (Steinberg::int32 index) override
    { return index >= 0 && static_cast<std::size_t> (index) < queueCount ? &queues[index] : nullptr; }
    Steinberg::Vst::IParamValueQueue* PLUGIN_API addParameterData (const Steinberg::Vst::ParamID&, Steinberg::int32& index) override
    { index = -1; return nullptr; }
    Steinberg::tresult PLUGIN_API queryInterface (const Steinberg::TUID iid, void** object) override
    {
        if (object == nullptr) return Steinberg::kInvalidArgument;
        *object = nullptr;
        if (! Steinberg::FUnknownPrivate::iidEqual (iid, Steinberg::Vst::IParameterChanges::iid)
            && ! Steinberg::FUnknownPrivate::iidEqual (iid, Steinberg::FUnknown::iid)) return Steinberg::kNoInterface;
        *object = static_cast<Steinberg::Vst::IParameterChanges*> (this);
        addRef();
        return Steinberg::kResultOk;
    }
    Steinberg::uint32 PLUGIN_API addRef() override { return 1; }
    Steinberg::uint32 PLUGIN_API release() override { return 1; }

private:
    Lookup locate (Steinberg::Vst::ParamID id, int offset) const noexcept
    {
        auto hash = (static_cast<std::uint64_t> (id) << 32) | static_cast<std::uint32_t> (offset);
        hash = (hash ^ (hash >> 30)) * UINT64_C(0xbf58476d1ce4e5b9);
        hash = (hash ^ (hash >> 27)) * UINT64_C(0x94d049bb133111eb);
        hash ^= hash >> 31;
        auto available = kTableCapacity;
        for (std::size_t i = 0; i < kTableCapacity; ++i)
        {
            const auto slot = static_cast<std::size_t> (hash + i) & (kTableCapacity - 1);
            const auto entry = slots[slot];
            if (entry == 0) return { Capacity, available == kTableCapacity ? slot : available };
            if (entry == kDeleted)
            {
                if (available == kTableCapacity) available = slot;
            }
            else if (points[entry - 1].id == id && points[entry - 1].offset == offset)
                return { entry - 1, slot };
        }
        return { Capacity, available };
    }

    std::array<Point, Capacity> points;
    std::array<Value, Capacity> values;
    std::array<std::size_t, Capacity> order;
    std::array<Queue, Capacity> queues;
    std::array<std::uint32_t, kTableCapacity> slots {};
    std::array<std::uint32_t, kTableCapacity> touchedSlots;
    std::size_t touchedCount = 0;
    std::size_t pointCount = 0, queueCount = 0;
    int frameCount = 0;
    bool finished = false;
};

using BoundedParameterChanges = FixedParameterChanges<kParameterPointCapacity>;
static_assert (sizeof (BoundedParameterChanges) < 1048576);
} // namespace duskstudio::vst3::detail
