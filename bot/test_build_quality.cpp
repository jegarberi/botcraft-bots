#include "build_quality.hpp"

#include <cassert>

using Construction::Cell;
using Construction::Space;
static_assert(Construction::CrampedMine(true, false, false, 3));
static_assert(!Construction::CrampedMine(false, false, false, 0));
static_assert(Construction::CrampedMine(true, true, false, 3));
static_assert(Construction::CrampedMine(true, true, true, 2));
static_assert(!Construction::CrampedMine(true, true, true, 3));

int main()
{
    for (const auto facing : { "north", "south", "east", "west" })
        for (const auto neighbor : { "north", "south", "east", "west" })
            assert(Construction::MisalignedChests(facing, neighbor) == (std::string_view(facing) != neighbor));
    assert(!Construction::MisalignedChests("", "north"));
    assert(!Construction::MisalignedChests("south", ""));
    std::set<Cell> road;
    for (int x = 0; x < 8; ++x) road.insert({ x, 0, 0 });
    assert(Construction::NarrowRoadCells(road) == 8);
    for (int x = 0; x < 8; ++x) road.insert({ x, 0, 1 });
    assert(Construction::NarrowRoadCells(road) == 0);
    road.insert({ 8, 0, 0 });
    assert(Construction::NarrowRoadCells(road) == 1);
    road.clear();
    for (int x = 0; x < 8; ++x)
        for (int z = 0; z < 3; ++z) road.insert({ x, x, z });
    assert(Construction::NarrowRoadCells(road) == 0);

    std::set<Cell> house;
    for (int x = 0; x < 5; ++x)
        for (int z = 0; z < 5; ++z)
            for (int y = 0; y < 4; ++y)
                if (x == 0 || x == 4 || z == 0 || z == 4 || y == 0 || y == 3) house.insert({ x, y, z });
    const auto read = [&](const Cell& p)
    {
        return p[1] <= 0 || house.count(p) ? Space::Solid : Space::Open;
    };
    assert(Construction::SealedBuilding(house, read));
    house.erase({ 2, 2, 4 }); // A window is not an entrance.
    assert(Construction::SealedBuilding(house, read));
    house.erase({ 2, 1, 4 });
    assert(!Construction::SealedBuilding(house, read));
    house.insert({ 2, 1, 4 });
    assert(!Construction::SealedBuilding(house, [&](const Cell& p)
    {
        return p[0] < 0 ? Space::Unknown : read(p);
    }));
    std::set<Cell> raised;
    for (int x = 0; x < 5; ++x)
        for (int z = 0; z < 5; ++z)
            for (int y = 0; y < 5; ++y)
                if (x == 0 || x == 4 || z == 0 || z == 4 || y == 0 || y == 4) raised.insert({ x, y, z });
    raised.erase({ 2, 2, 4 });
    raised.erase({ 2, 3, 4 });
    raised.insert({ 2, 1, 4 });
    raised.insert({ 2, 1, 5 });
    const auto raised_read = [&](const Cell& p) { return p[1] <= 0 || raised.count(p) ? Space::Solid : Space::Open; };
    assert(!Construction::SealedBuilding(raised, raised_read));
    for (int x = 1; x < 4; ++x)
        for (int z = 1; z < 4; ++z) house.erase({ x, 3, z });
    assert(!Construction::SealedBuilding(house, read)); // Open pens are not sealed buildings.
}
