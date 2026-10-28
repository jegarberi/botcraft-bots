#include "creative_build.hpp"
#include <cassert>
#include <chrono>
#include <iostream>

int main()
{
    // Rails go by command one at a time (their shape), and are not mistaken for a material to fill boxes with
    assert(CreativeBuild::Single("minecraft:powered_rail") && CreativeBuild::Batchable("minecraft:rail") && !CreativeBuild::Single("minecraft:stone"));
    assert(CreativeBuild::Batchable("minecraft:stone_bricks"));
    assert(CreativeBuild::Batchable("minecraft:stone_brick_stairs"));
    assert(!CreativeBuild::Batchable("minecraft:chest"));
    assert(!CreativeBuild::Batchable("minecraft:oak_door"));
    assert(CreativeBuild::Natural("minecraft:stone"));
    assert(!CreativeBuild::Natural("minecraft:chest"));
    assert(!CreativeBuild::Natural("minecraft:bedrock"));
    assert(!CreativeBuild::Natural("minecraft:stone_bricks"));
    std::set<CreativeBuild::Cell> floor;
    for (int x = 0; x < 70; ++x) for (int z = 0; z < 50; ++z) floor.insert({x, 64, z});
    const auto baseline = floor;
    int commands = 0, checked = 0;
    const auto start = std::chrono::steady_clock::now();
    while (!floor.empty())
    {
        const auto batch = CreativeBuild::Rectangle(floor, *floor.begin());
        assert(batch.count > 0 && batch.count <= 4096);
        for (int x = batch.low[0]; x <= batch.high[0]; ++x)
            for (int z = batch.low[2]; z <= batch.high[2]; ++z)
            {
                assert(floor.erase({x, 64, z}) == 1);
                ++checked;
            }
        ++commands;
    }
    assert(checked == 3500 && commands == 1);
    auto holes = baseline;
    holes.erase({10, 64, 5}); // A protected cell, another material, or unseen terrain must never be filled.
    holes.insert({0, 65, 0});
    const auto safe = CreativeBuild::Rectangle(holes, {0, 64, 0});
    assert(safe.high[2] < 5 && safe.low[1] == 64 && safe.high[1] == 64);
    assert(CreativeBuild::Rectangle(holes, {10, 64, 5}).count == 0);
    std::set<CreativeBuild::Cell> large;
    for (int x = 0; x < 128; ++x) for (int z = 0; z < 64; ++z) large.insert({x, 64, z});
    assert(CreativeBuild::Rectangle(large, {0, 64, 0}).count == 4096);
    std::set<CreativeBuild::Cell> hill; // levelling: a 40x40 column of 12 layers is one command
    for (int x = 0; x < 40; ++x) for (int y = 70; y < 82; ++y) for (int z = 0; z < 40; ++z) hill.insert({x, y, z});
    const auto cut = CreativeBuild::Rectangle(hill, {5, 75, 5});
    assert(cut.count == 40 * 40 * 12 && cut.low[1] == 70 && cut.high[1] == 81);
    std::set<CreativeBuild::Cell> slope, sky; // a ring of a slope to cut, the air around it already right
    for (int x = 0; x < 30; ++x) for (int z = 0; z < 30; ++z) (x < 2 || z < 2 ? slope : sky).insert({x, 70, z});
    const auto ring = CreativeBuild::Rectangle([&](const CreativeBuild::Cell& c) { return slope.count(c) || sky.count(c); }, {0, 70, 0});
    assert(ring.count == 900 && CreativeBuild::Rectangle(slope, {0, 70, 0}).count < 900);
    std::cout << "3500-cell floor: single placement " << checked << " actions; batched " << commands
              << " commands (" << static_cast<double>(checked) / commands << "x fewer), planner "
              << std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - start).count() << " us\n";
}
