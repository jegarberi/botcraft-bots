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
            "minecraft:deepslate", "minecraft:snow", "minecraft:short_grass", "minecraft:tall_grass",
            "minecraft:podzol", "minecraft:coarse_dirt", "minecraft:rooted_dirt", "minecraft:mud", "minecraft:clay",
            "minecraft:moss_block", "minecraft:moss_carpet", "minecraft:mossy_cobblestone", "minecraft:tuff", "minecraft:calcite",
            "minecraft:sandstone", "minecraft:red_sand", "minecraft:terracotta", "minecraft:snow_block", "minecraft:fern",
            "minecraft:large_fern", "minecraft:sweet_berry_bush", "minecraft:dead_bush", "minecraft:bush", "minecraft:leaf_litter",
            "minecraft:firefly_bush", "minecraft:dandelion", "minecraft:poppy", "minecraft:cornflower", "minecraft:oxeye_daisy",
            "minecraft:brown_mushroom", "minecraft:red_mushroom", "minecraft:pumpkin", "minecraft:vine"}) if (item == name) return true;
        return Suffix(item, "_log") || Suffix(item, "_leaves") || Suffix(item, "_terracotta");
    }
    using Cell = std::array<int, 3>;
    struct Batch { Cell low, high; int count; };

    // A box containing only eligible blueprint cells, never holes or another material: a rectangle of up to
    // 4096 cells on the seed's layer, then grown up and down by whole layers up to /fill's 32768 blocks.
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
        const int area = width * (batch.high[2] - batch.low[2] + 1);
        const auto layer = [&](int y)
        {
            for (int x = batch.low[0]; x <= batch.high[0]; ++x)
                for (int z = batch.low[2]; z <= batch.high[2]; ++z) if (!cells.count({x, y, z})) return false;
            return true;
        };
        while (area * (batch.high[1] - batch.low[1] + 2) <= 32768 && layer(batch.high[1] + 1)) ++batch.high[1];
        while (area * (batch.high[1] - batch.low[1] + 2) <= 32768 && layer(batch.low[1] - 1)) --batch.low[1];
        batch.count = area * (batch.high[1] - batch.low[1] + 1);
        return batch;
    }
}
