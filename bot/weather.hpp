#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>

struct Weather
{
    std::atomic<float> rain = 0.0f;
    std::atomic<float> thunder = 0.0f;

    void Reset() { rain = 0.0f; thunder = 0.0f; }

    void Update(const unsigned char event, const float value)
    {
        // Event constants from Java 26.3 ClientboundGameEventPacket.
        switch (event)
        {
        case 1: rain = 1.0f; break; // START_RAINING
        case 2: rain = 0.0f; break; // STOP_RAINING
        case 7: if (std::isfinite(value)) rain = std::clamp(value, 0.0f, 1.0f); break;
        case 8: if (std::isfinite(value)) thunder = std::clamp(value, 0.0f, 1.0f); break;
        default: break;
        }
    }
};
