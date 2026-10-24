#pragma once

#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace CreativeFlight
{
    using Point = std::array<double, 3>;

    template <typename Open>
    bool Clear(const Point& from, const Point& to, const Open& open)
    {
        const double distance = std::hypot(std::hypot(to[0] - from[0], to[2] - from[2]), to[1] - from[1]);
        if (distance > 96.0) return false; // Plan only nearby loaded terrain; long journeys use hops.
        const int steps = std::max(1, static_cast<int>(std::ceil(distance * 4.0)));
        for (int i = 0; i <= steps; ++i)
        {
            Point p;
            for (int axis = 0; axis < 3; ++axis) p[axis] = from[axis] + (to[axis] - from[axis]) * i / steps;
            for (int x = static_cast<int>(std::floor(p[0] - 0.35)); x <= static_cast<int>(std::floor(p[0] + 0.35)); ++x)
                for (int y = static_cast<int>(std::floor(p[1] + 0.05)); y <= static_cast<int>(std::floor(p[1] + 1.8)); ++y)
                    for (int z = static_cast<int>(std::floor(p[2] - 0.35)); z <= static_cast<int>(std::floor(p[2] + 0.35)); ++z)
                        if (!open(x, y, z)) return false;
        }
        return true;
    }

    template <typename Open>
    std::vector<Point> Route(const Point& from, const Point& to, const Open& open)
    {
        if (Clear(from, to, open)) return {to};
        for (const double rise : {8.0, 16.0, 32.0})
        {
            const double y = std::max(from[1], to[1]) + rise;
            const Point up = {from[0], y, from[2]}, across = {to[0], y, to[2]};
            if (Clear(from, up, open) && Clear(up, across, open) && Clear(across, to, open)) return {up, across, to};
        }
        return {};
    }
}
