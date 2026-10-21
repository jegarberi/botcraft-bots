// The stores: every operation, the same checks on each. The store is BOTCRAFT_DB's (see store.hpp); test_store.sh
// runs them on SQLite, on a throwaway PostgreSQL and on a throwaway Valkey/Redis
#include "store.hpp"

#include <cassert>
#include <chrono>
#include <cstdio>
#include <string>
#include <thread>
#include <vector>

using store::Position;

int main()
{
    const std::string run = std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
    const std::string bot = "TestBot" + run;     // a fresh bot each run: the server stores can hold earlier runs
    const std::string dim = "test:dim" + run;
    std::unique_ptr<store::Memory> m = store::OpenMemory(bot);
    assert(m && "the memory opens");

    // What it is doing
    m->SaveDoing("state", "farming");
    m->SaveDoing("goal", "wheat 3");
    m->SaveDoing("goal", "");
    assert(m->LoadDoing().size() == 1 && m->LoadDoing()["state"] == "farming");

    // Its own places
    m->SetOwn("bed", Position(1, 64, 2));
    m->SetOwn("bed", Position(3, 64, 4));
    assert(m->GetOwn("bed") == Position(3, 64, 4));
    m->ForgetOwn("bed");
    assert(!m->GetOwn("bed"));

    // A scan, the nearest of a kind (a suffix matches several kinds), what a cell holds
    const store::Box box{ Position(-24, 40, -24), Position(24, 88, 24) };
    m->ReplaceBox(dim, box, { { "iron_ore", Position(10, 50, 10) }, { "deepslate_iron_ore", Position(2, 41, 2) }, { "oak_log", Position(-5, 70, 3) },
        { "mob:zombie", Position(0, 64, 0) } });
    std::vector<store::Place> near = m->Nearest(dim, "iron_ore", Position(0, 40, 0), 5);
    assert(near.size() == 2 && near[0].kind == "deepslate_iron_ore" && near[1].kind == "iron_ore");
    assert(m->Nearest(dim, "_log", Position(0, 64, 0), 5).size() == 1);
    assert(m->Nearest(dim, "_log", Position(0, 64, 0), 0).empty());
    assert(m->KnownInCell(dim, "iron_ore", Position(0, 48, 0)) && !m->KnownInCell(dim, "iron_ore", Position(16, 48, 0)) && !m->KnownInCell(dim, "coal_ore", Position(0, 48, 0)));
    // A second scan of the box replaces the first: the log was cut
    m->ReplaceBox(dim, box, { { "iron_ore", Position(10, 50, 10) }, { "deepslate_iron_ore", Position(2, 41, 2) } });
    assert(m->Nearest(dim, "_log", Position(0, 64, 0), 5).empty());
    // A scan elsewhere leaves this box alone
    m->ReplaceBox(dim, store::Box{ Position(100, 40, 100), Position(148, 88, 148) }, { { "coal_ore", Position(120, 60, 120) } });
    assert(m->Nearest(dim, "_ore", Position(0, 40, 0), 10).size() == 3);
    // Told by a teammate: one place per position, the kind replaced
    m->RememberPlace(dim, "gold_ore", Position(-3, 45, -3));
    m->RememberPlace(dim, "copper_ore", Position(-3, 45, -3));
    assert(m->Nearest(dim, "gold_ore", Position(0, 40, 0), 5).empty() && m->Nearest(dim, "copper_ore", Position(0, 40, 0), 5).size() == 1);
    m->Forget(Position(-3, 45, -3));
    m->Forget(Position(10, 50, 10));
    assert(m->Nearest(dim, "iron_ore", Position(0, 40, 0), 5).size() == 1);
    const auto summary = m->Summary(40);
    assert(summary.size() == 2);

    // Begin/Commit group writes
    m->Begin();
    m->SaveDoing("make", "bread 4");
    m->Commit();
    assert(m->LoadDoing()["make"] == "bread 4");

    store::Shared* s = store::SharedStore();
    assert(s && "the shared store opens");

    // Finds: one per kind and cell, a rescan takes back what is gone
    s->ReportFinds(dim, box, { { "iron_ore", Position(10, 50, 10) }, { "iron_ore", Position(11, 50, 10) }, { "oak_log", Position(-5, 70, 3) } }, bot);
    near = s->NearestFinds(dim, "iron_ore", Position(0, 40, 0), 5);
    assert(near.size() == 1 && near[0].p == Position(10, 50, 10));
    s->ReportFinds(dim, box, { { "iron_ore", Position(10, 50, 10) } }, bot);
    assert(s->NearestFinds(dim, "_log", Position(0, 64, 0), 5).empty());
    s->ForgetFind(Position(10, 50, 10));
    assert(s->NearestFinds(dim, "iron_ore", Position(0, 40, 0), 5).empty());

    // Chests and what is in them
    s->RememberChest(dim, Position(5, 64, 5), { { "minecraft:bread", 12 }, { "minecraft:wheat", 3 } }, bot);
    s->RememberChest(dim, Position(9, 64, 9), { { "minecraft:bread", 40 } }, bot);
    s->RememberChest(dim, Position(30, 64, 30), {}, bot);
    assert(s->ChestItems(dim, Position(5, 64, 5)).size() == 2 && s->ChestItems(dim, Position(5, 64, 5))["minecraft:bread"] == 12);
    assert(s->ChestHolding(dim, "minecraft:bread", Position(0, 64, 0)) == Position(5, 64, 5));
    assert(s->ChestHolding(dim, "minecraft:bread", Position(10, 64, 10)) == Position(9, 64, 9));
    assert(!s->ChestHolding(dim, "minecraft:carrot", Position(0, 64, 0)));
    assert(s->ChestsIn(dim, box).size() == 2);
    const auto with = s->ChestsWith("minecraft:bread", 10);
    size_t mine = 0;
    for (const auto& c : with) if (c.dimension == dim) ++mine;
    assert(mine == 2);
    assert(s->Stored(dim)["minecraft:bread"] == 52 && s->Stored(dim).count("") == 0);
    bool listed = false;
    for (const auto& c : s->Chests(1000)) if (c.dimension == dim && c.p == Position(30, 64, 30) && c.by == bot) listed = true;
    assert(listed);
    // Opened again: what it holds now replaces what it held
    s->RememberChest(dim, Position(5, 64, 5), { { "minecraft:wheat", 1 } }, bot);
    assert(s->ChestItems(dim, Position(5, 64, 5)).size() == 1 && s->Stored(dim)["minecraft:bread"] == 40);
    s->ForgetChest(dim, Position(5, 64, 5));
    assert(s->ChestItems(dim, Position(5, 64, 5)).empty() && s->ChestsIn(dim, box).size() == 1);

    // Sites in order (roads left out), removed by index
    const size_t sites_before = s->Sites().size();
    s->AddSite({ Position(0, 0, 0), Position(10, 10, 10), "house" + run });
    s->AddSite({ Position(20, 0, 0), Position(30, 10, 10), "main_road" });
    s->AddSite({ Position(40, 0, 0), Position(50, 10, 10), "tower" + run });
    std::vector<store::Site> sites = s->Sites();
    assert(sites.size() == sites_before + 2 && sites.back().name == "tower" + run);
    s->RemoveSite(sites.size() - 2);
    sites = s->Sites();
    assert(sites.size() == sites_before + 1 && sites.back().name == "tower" + run);
    s->RemoveSite(sites.size() - 1);

    // The chest for each item, mines, roles
    std::map<std::string, Position> before = s->ChestFor();
    std::map<std::string, Position> chests = before;
    chests["test:item" + run] = Position(1, 2, 3);
    s->SaveChestFor(chests);
    assert(s->ChestFor()["test:item" + run] == Position(1, 2, 3));
    s->SaveChestFor(before);
    const size_t mines_before = s->Mines().size();
    s->AddMine({ Position(7, 60, 7), "iron_ore" + run });
    assert(s->Mines().size() == mines_before + 1 && s->Mines().back().resources == "iron_ore" + run);
    const std::string role = "trade:test" + run;
    s->SetRole(bot + "a", role, true);
    s->SetRole(bot + "b", role, true);
    s->SetRole(bot + "a", role, true);
    assert(s->WithRole(role) == std::vector<std::string>({ bot + "a", bot + "b" }));
    assert(s->RoleStartingWith(bot + "b", "trade:") == role && s->RoleStartingWith(bot + "c", "trade:").empty());
    s->SetRole(bot + "a", role, false);
    assert(s->WithRole(role) == std::vector<std::string>({ bot + "b" }));
    s->SetRole(bot + "b", role, false);

    // From many threads at once
    std::vector<std::thread> threads;
    for (int t = 0; t < 8; ++t)
    {
        threads.emplace_back([&, t]()
            {
                store::Shared* mine_s = store::SharedStore();
                for (int k = 0; k < 20; ++k) mine_s->SetRole(bot + "t" + std::to_string(t), role + "x", (k % 2) == 0);
            });
    }
    for (std::thread& t : threads) t.join();
    assert(s->WithRole(role + "x").size() == 0);
    assert(store::Error().empty() || !(std::printf("store error: %s\n", store::Error().c_str())));
    std::printf("store OK (%s): memory, finds, chests, sites, chests for items, mines, roles, 8 threads\n", store::Backend().c_str());
}
