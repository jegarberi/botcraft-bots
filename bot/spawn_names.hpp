#pragma once

#include <algorithm>
#include <set>
#include <stdexcept>
#include <string>
#include <vector>

inline std::vector<std::string> NewBotNames(const int count, const std::string& base, const std::set<std::string>& used)
{
    if (count < 1 || count > 100) { throw std::runtime_error("count must be an integer from 1 to 100"); }
    if (base.empty() || base.size() > 15 || !std::all_of(base.begin(), base.end(), [](const char c)
        { return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_'; }))
    {
        throw std::runtime_error("base name must contain 1-15 letters, digits or underscores");
    }
    std::vector<std::string> names;
    names.reserve(count);
    for (int i = 1; names.size() < static_cast<std::size_t>(count); ++i)
    {
        const std::string name = base + std::to_string(i);
        if (name.size() > 16) { throw std::runtime_error("numbered bot names exceed 16 characters; use a shorter base name"); }
        if (used.count(name) == 0) { names.push_back(name); }
    }
    return names;
}
