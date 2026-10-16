#pragma once

#include <set>
#include <stdexcept>
#include <string>
#include <vector>

inline std::vector<std::string> NewBotNames(const int count, const std::set<std::string>& used)
{
    if (count < 1 || count > 100) { throw std::runtime_error("count must be an integer from 1 to 100"); }
    std::vector<std::string> names;
    names.reserve(count);
    for (int i = 1; names.size() < static_cast<std::size_t>(count); ++i)
    {
        const std::string name = "Bot" + std::to_string(i);
        if (used.count(name) == 0) { names.push_back(name); }
    }
    return names;
}
