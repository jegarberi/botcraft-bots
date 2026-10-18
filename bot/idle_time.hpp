#pragma once

#include <chrono>

class IdleTime
{
public:
    int Record(const std::chrono::steady_clock::duration elapsed, const bool enabled)
    {
        if (!enabled) return 0;
        remainder += elapsed;
        const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(remainder);
        remainder -= seconds;
        return static_cast<int>(seconds.count());
    }

private:
    std::chrono::steady_clock::duration remainder{};
};
