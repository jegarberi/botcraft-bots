#include "site_index.hpp"

#include <algorithm>
#include <cassert>
#include <chrono>
#include <fstream>
#include <iostream>
#include <limits>
#include <random>

using Botcraft::Position;

static bool LinearContains(const std::vector<Site>& sites, const Position& p)
{
    return std::any_of(sites.begin(), sites.end(), [&](const Site& s) { return s.Contains(p); });
}

int main(int argc, char** argv)
{
    std::vector<Site> sites = {
        { Position(-17, -65, -33), Position(-1, -1, 15), "negative" },
        { Position(0, 0, 0), Position(16, 16, 16), "positive" },
        { Position(-1, -1, -1), Position(1, 1, 1), "overlap" },
        { Position(-1000000, 100, -1000000), Position(1000000, 200, 1000000), "large" },
    };
    SiteIndex index;
    index.Reset(sites);
    for (const int x : { -33, -32, -17, -16, -1, 0, 1, 15, 16, 17, 31, 32 })
        for (const int y : { -66, -65, -64, -17, -16, -1, 0, 1, 15, 16, 17, 100, 200, 201 })
            for (const int z : { -34, -33, -32, -17, -16, -1, 0, 1, 15, 16, 17 })
                assert(index.Contains(Position(x, y, z)) == LinearContains(sites, Position(x, y, z)));
    for (const int n : { std::numeric_limits<int>::min(), std::numeric_limits<int>::max() })
        assert(!index.Contains(Position(n, n, n)));
    index.Reset({});
    assert(!index.Contains(Position(0, 0, 0)));
    index.Reset({ { Position(-2, -2, -2), Position(2, 2, 2), "new" } });
    assert(index.Contains(Position(-1, 0, 1)));

    std::mt19937 random(42);
    std::uniform_int_distribution<int> at(-300, 300), size(0, 100);
    sites.clear();
    for (int i = 0; i < 264; ++i)
    {
        const Position low(at(random), at(random), at(random));
        sites.push_back({ low, low + Position(size(random), size(random), size(random)), "test" });
    }
    index.Reset(sites);
    for (int i = 0; i < 50000; ++i)
    {
        const Position p(at(random), at(random), at(random));
        assert(index.Contains(p) == LinearContains(sites, p));
    }
    std::cout << "site index OK: boundaries, negative coordinates, overlaps, large boxes, refresh, 50000 random queries\n";
    if (argc != 2) { return 0; }

    std::ifstream file(argv[1]);
    assert(file);
    sites.clear();
    Site s;
    while (file >> s.min.x >> s.min.y >> s.min.z >> s.max.x >> s.max.y >> s.max.z) { sites.push_back(s); }
    assert(!sites.empty());
    std::vector<Position> points;
    for (int x = 8; x <= 72; ++x)
        for (int y = 31; y <= 95; ++y)
            for (int z = 425; z <= 489; ++z) { points.emplace_back(x, y, z); }
    std::size_t expected = 0;
    const auto bench = [&](auto contains)
        {
            std::size_t hits = 0;
            const auto start = std::chrono::steady_clock::now();
            for (int repeat = 0; repeat < 5; ++repeat)
                for (const Position& p : points) { hits += contains(p); }
            const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
            if (expected == 0) { expected = hits; } else { assert(hits == expected); }
            return ms;
        };
    const double before = bench([&](const Position& p) { return LinearContains(sites, p); });
    index.Reset(sites);
    const double after = bench([&](const Position& p) { return index.Contains(p); });
    std::cout << sites.size() << " sites, " << points.size() * 5 << " queries: linear " << before
              << " ms, indexed " << after << " ms, " << before / after << "x faster\n";
}
