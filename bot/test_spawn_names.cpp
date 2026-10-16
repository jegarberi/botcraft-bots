#include "spawn_names.hpp"
#include <cassert>

int main()
{
    std::set<std::string> used = { "Bot", "Bot1", "Bot3" };
    const auto first = NewBotNames(3, "Bot", used);
    assert((first == std::vector<std::string>{ "Bot2", "Bot4", "Bot5" }));
    used.insert(first.begin(), first.end());
    const auto second = NewBotNames(2, "Bot", used);
    assert((second == std::vector<std::string>{ "Bot6", "Bot7" }));
    assert(NewBotNames(100, "Bot", {}).size() == 100);
    for (const int count : { -1, 0, 101 })
    {
        bool rejected = false;
        try { NewBotNames(count, "Bot", used); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    assert((NewBotNames(2, "Worker", { "Worker1" }) == std::vector<std::string>{ "Worker2", "Worker3" }));
    assert(NewBotNames(1, "ABCDEFGHIJKLMNO", {})[0] == "ABCDEFGHIJKLMNO1");
    for (const std::string base : { "", "bad name", "bad-name", "ABCDEFGHIJKLMNOP", "bot$" })
    {
        bool rejected = false;
        try { NewBotNames(1, base, {}); } catch (const std::runtime_error&) { rejected = true; }
        assert(rejected);
    }
    bool overflow = false;
    try { NewBotNames(10, "ABCDEFGHIJKLMNO", {}); } catch (const std::runtime_error&) { overflow = true; }
    assert(overflow);

}
