#include "creative_flight.hpp"
#include <cassert>

int main()
{
    const CreativeFlight::Point from{0.5, 64.0, 0.5}, to{12.5, 64.0, 0.5};
    const auto open = [](int, int y, int) { return y >= 64 && y < 319; };
    assert(CreativeFlight::Route(from, to, open).size() == 1);
    const auto wall = [](int x, int y, int) { return y >= 64 && !(x == 6 && y < 70); };
    const auto route = CreativeFlight::Route(from, to, wall);
    assert(route.size() == 3 && route[0][1] == 72.0);
    const auto roof = [](int x, int y, int) { return y >= 64 && y < 67 && x != 6; };
    assert(CreativeFlight::Route(from, to, roof).empty());
    const auto unloaded = [](int x, int, int) { return x < 6; };
    assert(CreativeFlight::Route(from, to, unloaded).empty());
    assert(!CreativeFlight::Clear(from, {200.5, 64.0, 0.5}, open));
    // The flight volume includes shoulders and head, not just the feet.
    assert(!CreativeFlight::Clear(from, to, [](int x, int y, int) { return !(x == 6 && y == 65); }));
}
