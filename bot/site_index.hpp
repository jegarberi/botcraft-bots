#pragma once

#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "botcraft/Game/Vector3.hpp"

struct Site
{
    Botcraft::Position min, max;
    std::string name;
    bool Contains(const Botcraft::Position& p) const
    {
        return p.x >= min.x && p.x <= max.x && p.y >= min.y && p.y <= max.y && p.z >= min.z && p.z <= max.z;
    }
};

// Index only the sections queried during the existing five-second cache lifetime.
class SiteIndex
{
    std::vector<Site> sites;
    std::unordered_map<Botcraft::Position, std::vector<std::size_t>> sections;

public:
    void Reset(std::vector<Site> updated)
    {
        sites = std::move(updated);
        sections.clear();
    }

    bool Contains(const Botcraft::Position& p)
    {
        const auto section = [](const int n) { return n / 16 - (n % 16 < 0); };
        const Botcraft::Position key(section(p.x), section(p.y), section(p.z));
        auto [it, inserted] = sections.try_emplace(key);
        if (inserted)
        {
            const Botcraft::Position low(key.x * 16, key.y * 16, key.z * 16);
            const Botcraft::Position high = low + Botcraft::Position(15, 15, 15);
            for (std::size_t i = 0; i < sites.size(); ++i)
            {
                const Site& s = sites[i];
                if (s.min.x <= high.x && s.max.x >= low.x && s.min.y <= high.y && s.max.y >= low.y && s.min.z <= high.z && s.max.z >= low.z)
                {
                    it->second.push_back(i);
                }
            }
        }
        for (const std::size_t i : it->second)
        {
            if (sites[i].Contains(p)) { return true; }
        }
        return false;
    }
};
