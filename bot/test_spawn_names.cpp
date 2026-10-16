#include "spawn_names.hpp"
#include <cassert>

int main()
{
    std::set<std::string> used = { "Bot", "Bot1", "Bot3" };
    const auto first = NewBotNames(3, used);
    assert((first == std::vector<std::string>{ "Bot2", "Bot4", "Bot5" }));
    used.insert(first.begin(), first.end());
    const auto second = NewBotNames(2, used);
    assert((second == std::vector<std::string>{ "Bot6", "Bot7" }));
    assert(NewBotNames(100, {}).size() == 100);
    for (const int count : { -1, 0, 101 })
    {
        bool rejected = false;
        try { NewBotNames(count, used); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
}
