#pragma once

#include <array>
#include <set>
#include <string_view>

namespace CreativeBuild
{
    inline bool Suffix(const std::string_view name, const std::string_view suffix)
    {
        return name.size() >= suffix.size() && name.substr(name.size() - suffix.size()) == suffix;
    }
    inline bool Batchable(const std::string_view item)
    {
        for (const auto name : {"minecraft:air", "minecraft:stone", "minecraft:stone_bricks", "minecraft:cobblestone",
            "minecraft:dirt", "minecraft:grass_block", "minecraft:sand", "minecraft:gravel", "minecraft:glass",
            "minecraft:glass_pane", "minecraft:farmland", "minecraft:wheat", "minecraft:water"}) if (item == name) return true;
        return Suffix(item, "_stairs") || Suffix(item, "_slab") || Suffix(item, "_planks") || Suffix(item, "_log") || Suffix(item, "_fence");
    }
    inline bool Natural(const std::string_view item)
    {
        for (const auto name : {"minecraft:stone", "minecraft:cobblestone", "minecraft:dirt", "minecraft:grass_block",
            "minecraft:sand", "minecraft:gravel", "minecraft:granite", "minecraft:andesite", "minecraft:diorite",
            "minecraft:deepslate", "minecraft:snow", "minecraft:short_grass", "minecraft:tall_grass"}) if (item == name) return true;
        return Suffix(item, "_log") || Suffix(item, "_leaves");
    }
    using Cell = std::array<int, 3>;
    struct Batch { Cell low, high; int count; };

    // A horizontal rectangle containing only eligible blueprint cells, never holes or another material.
    inline Batch Rectangle(const std::set<Cell>& cells, const Cell& seed)
    {
        Batch batch{seed, seed, 0};
        if (!cells.count(seed)) return batch;
        while (batch.high[0] - batch.low[0] < 127 && cells.count({batch.high[0] + 1, seed[1], seed[2]})) ++batch.high[0];
        while (batch.high[0] - batch.low[0] < 127 && cells.count({batch.low[0] - 1, seed[1], seed[2]})) --batch.low[0];
        const int width = batch.high[0] - batch.low[0] + 1;
        const auto row = [&](int z)
        {
            for (int x = batch.low[0]; x <= batch.high[0]; ++x) if (!cells.count({x, seed[1], z})) return false;
            return true;
        };
        while (width * (batch.high[2] - batch.low[2] + 2) <= 4096 && row(batch.high[2] + 1)) ++batch.high[2];
        while (width * (batch.high[2] - batch.low[2] + 2) <= 4096 && row(batch.low[2] - 1)) --batch.low[2];
        batch.count = width * (batch.high[2] - batch.low[2] + 1);
        return batch;
    }
}
