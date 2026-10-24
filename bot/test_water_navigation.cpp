#include "botcraft/AI/Tasks/PathfindingTask.hpp"

#include <cassert>
#include <cmath>
#include <iostream>

// Deterministic water motion model: inputs reset each physics tick, drag and a steady sideways current.
// The old upward waypoint wait supplied one jump and no steering until it reached the bank's Y.
static int CrossCurrent(const bool controlled, const double current, const double yaw)
{
    double x = 0.0, y = 0.0, z = 0.0, vx = 0.0, vy = 0.0, vz = 0.0;
    for (int tick = 0; tick < 200; ++tick)
    {
        const auto input = Botcraft::WaterNavigation::Towards(1.0 - x, 0.5 - y, -z, vx, vz, yaw);
        if (input.reached) return tick;
        const double radians = yaw * 0.017453292519943295;
        const double forward = controlled ? input.forward : 0.0;
        const double left = controlled ? input.left : 0.0;
        vx = (vx + (left * std::cos(radians) - forward * std::sin(radians)) * 0.02) * 0.8 + current;
        vz = (vz + (forward * std::cos(radians) + left * std::sin(radians)) * 0.02) * 0.8;
        vy = (vy + ((controlled ? input.jump : tick == 0) ? 0.04 : 0.0)) * 0.8 - 0.02;
        x += vx; y += vy; z += vz;
    }
    return -1;
}
int main()
{
    assert(CrossCurrent(false, 0.014, 0.0) == -1);
    int longest = 0;
    for (const double current : {-0.014, 0.0, 0.014})
    {
        for (const double yaw : {0.0, 90.0, 180.0, 270.0})
        {
            const int ticks = CrossCurrent(true, current, yaw);
            assert(ticks > 0 && ticks < 200);
            longest = std::max(longest, ticks);
        }
    }
    const auto drift = Botcraft::WaterNavigation::Towards(0.05, 0.0, 0.05, 0.08, 0.0, 0.0);
    assert(drift.reached); // A current does not need to stop before proceeding to the next waypoint.
    const auto down = Botcraft::WaterNavigation::Towards(0.0, -1.0, 0.0, 0.0, 0.0, 0.0);
    assert(!down.jump && !down.reached);
    std::cout << "water navigation OK: old ascent stalls for 200 ticks; current controller reaches all 12 simulated crossings in <= " << longest << " ticks\n";
}
