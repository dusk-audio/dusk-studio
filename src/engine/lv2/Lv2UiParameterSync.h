#pragma once

#include "Lv2Instance.h"

#include <cstring>
#include <vector>

namespace duskstudio::lv2
{
class Lv2UiParameterSync
{
public:
    template <typename Sender>
    void sendCurrentValues (Lv2Instance& instance, bool force, Sender&& sender)
    {
        instance.drainPatchFeedback();

        const int count = instance.uiParameterEventCount();
        if (sentValues.size() != static_cast<size_t> (count))
        {
            sentValues.assign (static_cast<size_t> (count), 0.0f);
            valueSent.assign (static_cast<size_t> (count), 0);
            force = true;
        }

        Lv2Instance::UiParameterEvent event;
        for (int i = 0; i < count; ++i)
        {
            if (! instance.currentUiParameterEvent (i, event)) continue;
            const auto index = static_cast<size_t> (i);
            if (! force && valueSent[index] != 0
                && std::memcmp (&sentValues[index], &event.value, sizeof (event.value)) == 0)
                continue;

            sender (event);
            sentValues[index] = event.value;
            valueSent[index] = 1;
        }
    }

    void reset()
    {
        sentValues.clear();
        valueSent.clear();
    }

private:
    std::vector<float> sentValues;
    std::vector<uint8_t> valueSent;
};
} // namespace duskstudio::lv2
