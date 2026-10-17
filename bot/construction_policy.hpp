#pragma once

#include <cstdint>
#include <string_view>

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

    constexpr bool ShouldFinishGatherFirst(const bool building, const int goal_remaining)
    {
        return building && goal_remaining > 0;
    }

    constexpr int PlacementStage(const bool clearing, const std::string_view item)
    {
        if (clearing)
        {
            return 0;
        }
        const auto ends_with = [&](const std::string_view suffix)
        {
            return item.size() >= suffix.size() && item.substr(item.size() - suffix.size()) == suffix;
        };
        return ends_with("_bed") ? 2 : 1;
    }
}

