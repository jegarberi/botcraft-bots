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
        // Last, once the solid blocks of the plan stand: what goes on or against them. A sloped rail placed before the
        // block on its high side was taken off by the next update (gaps in a railway at each slope); likewise a
        // lantern, button, ladder or wall torch with nothing to hang on. (Not chains: a lantern hangs from one)
        return (ends_with("_bed") || ends_with("_door") || ends_with("_fence_gate") || ends_with(":campfire") || ends_with("_torch") ||
            ends_with(":torch") || ends_with(":water") || ends_with(":rail") || ends_with("_rail") || ends_with("lantern") ||
            ends_with("_button") || ends_with(":ladder")) ? 2 : 1;
    }
}

