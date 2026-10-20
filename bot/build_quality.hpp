#pragma once

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <queue>
#include <set>
#include <string_view>

namespace Construction
{
    using Cell = std::array<int, 3>;
    enum class Space { Unknown, Open, Solid, Hazard };

    constexpr bool MisalignedChests(const std::string_view facing, const std::string_view neighbor)
    {
        return !facing.empty() && !neighbor.empty() && facing != neighbor;
    }

    // A mine slice the bot walks through: open at the feet, but less than 3 wide or 3 high
    constexpr bool CrampedMine(const bool center, const bool left, const bool right, const int headroom)
    {
        return center && (!left || !right || headroom < 3);
    }

    // A road cell needs a second lane, including on bends and one-block slopes.
    inline int NarrowRoadCells(const std::set<Cell>& deck)
    {
        const auto has = [&](const int x, const int y, const int z)
        {
            for (int dy = -1; dy <= 1; ++dy)
                if (deck.count({ x, y + dy, z })) return true;
            return false;
        };
        int narrow = 0;
        for (const auto& p : deck)
        {
            bool wide = false;
            for (const int dx : { -1, 1 })
                for (const int dz : { -1, 1 })
                    wide = wide || (has(p[0] + dx, p[1], p[2]) && has(p[0], p[1], p[2] + dz) && has(p[0] + dx, p[1], p[2] + dz));
            narrow += !wide;
        }
        return narrow;
    }

    template<class Read>
    bool SealedBuilding(const std::set<Cell>& blocks, Read read)
    {
        if (blocks.empty()) return false;
        Cell low = *blocks.begin(), high = low;
        for (const auto& p : blocks)
            for (int i = 0; i < 3; ++i) { low[i] = std::min(low[i], p[i]); high[i] = std::max(high[i], p[i]); }
        const auto width = static_cast<long long>(high[0]) - low[0] + 3;
        const auto depth = static_cast<long long>(high[2]) - low[2] + 3;
        const auto height = static_cast<long long>(high[1]) - low[1];
        // shortcut: access is checked inside the plan plus one block, extend the audit for sites larger than 262144 cells.
        if (width > 128 || depth > 128 || height < 3 || height > 64 || width * depth * height > 262144 ||
            low[0] <= std::numeric_limits<int>::min() + 1 || low[2] <= std::numeric_limits<int>::min() + 1 ||
            high[0] >= std::numeric_limits<int>::max() - 1 || high[2] >= std::numeric_limits<int>::max() - 1) return false;
        std::set<Cell> walkable, roofed;
        std::queue<Cell> open;
        std::set<Cell> reached;
        std::map<std::array<int, 2>, int> roof;
        for (const auto& p : blocks)
        {
            const auto entry = roof.emplace(std::array<int, 2>{ p[0], p[2] }, p[1]);
            entry.first->second = std::max(entry.first->second, p[1]);
        }
        for (int x = low[0] - 1; x <= high[0] + 1; ++x)
            for (int z = low[2] - 1; z <= high[2] + 1; ++z)
                for (int y = low[1] + 1; y < high[1]; ++y)
                {
                    const auto floor = read(Cell{ x, y - 1, z });
                    const auto feet = read(Cell{ x, y, z });
                    const auto head = read(Cell{ x, y + 1, z });
                    // Unloaded surroundings cannot establish that a building is sealed.
                    if (floor == Space::Unknown || feet == Space::Unknown || head == Space::Unknown) return false;
                    if (floor != Space::Solid || feet != Space::Open || head != Space::Open) continue;
                    const Cell p{ x, y, z };
                    walkable.insert(p);
                    if (x == low[0] - 1 || x == high[0] + 1 || z == low[2] - 1 || z == high[2] + 1)
                    { open.push(p); reached.insert(p); }
                    if (x > low[0] && x < high[0] && z > low[2] && z < high[2] && roof.count({ x, z }) && roof.at({ x, z }) >= y + 2)
                        roofed.insert(p);
                }
        while (!open.empty())
        {
            const Cell p = open.front(); open.pop();
            for (const Cell d : { Cell{ 1, 0, 0 }, Cell{ -1, 0, 0 }, Cell{ 0, 0, 1 }, Cell{ 0, 0, -1 } })
            {
                for (int dy = -1; dy <= 1; ++dy)
                {
                    const Cell next{ p[0] + d[0], p[1] + dy, p[2] + d[2] };
                    if (!walkable.count(next)) continue;
                    // A one-block step also needs room above the lower landing.
                    const Cell clearance = dy > 0 ? Cell{ p[0], p[1] + 2, p[2] } : Cell{ next[0], next[1] + 2, next[2] };
                    if (dy != 0 && read(clearance) != Space::Open) continue;
                    if (reached.insert(next).second) open.push(next);
                }
            }
        }
        return !roofed.empty() && std::none_of(roofed.begin(), roofed.end(), [&](const Cell& p) { return reached.count(p); });
    }
}
