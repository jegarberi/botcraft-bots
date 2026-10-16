#pragma once

#include <cstdint>

namespace Construction
{
    constexpr bool CanPursuePlayer(const bool building, const int food, const double health)
    {
        return !building && food >= 8 && health >= 12.0;
    }

    constexpr bool RetryReady(const int failures, const std::int64_t elapsed_seconds)
    {
        return failures == 0 || elapsed_seconds >= 60;
    }
}
