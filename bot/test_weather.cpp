#include "weather.hpp"

#include <cassert>
#include <limits>

int main()
{
    Weather weather;
    assert(weather.rain == 0.0f && weather.thunder == 0.0f);
    weather.Update(1, 0.0f);
    assert(weather.rain == 1.0f);
    weather.Update(2, 0.0f);
    assert(weather.rain == 0.0f);
    weather.Update(7, 0.4f);
    weather.Update(8, 0.8f);
    assert(weather.rain == 0.4f && weather.thunder == 0.8f);
    weather.Update(7, std::numeric_limits<float>::quiet_NaN());
    weather.Update(8, std::numeric_limits<float>::infinity());
    assert(weather.rain == 0.4f && weather.thunder == 0.8f);
    weather.Update(7, -1.0f);
    weather.Update(8, 2.0f);
    assert(weather.rain == 0.0f && weather.thunder == 1.0f);
    weather.Update(3, 0.5f);
    assert(weather.rain == 0.0f && weather.thunder == 1.0f);
    weather.Reset();
    assert(weather.rain == 0.0f && weather.thunder == 0.0f);
}
