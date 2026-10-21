// Copy the SQLite stores into the one BOTCRAFT_DB names (PostgreSQL or Valkey): every bot's memory (memory/*.sqlite)
// and what the bots share (botcraft.sqlite), then count what arrived. Run from bot/bin with the program stopped:
//   BOTCRAFT_DB=postgresql://... bin/bot_import [memory dir (../memory)] [shared database (../botcraft.sqlite)]
// Places keep their kind and position (seen: the import time). Sites and mines are only copied into a store that
// has none yet, so running it twice adds no copies; each bot's memory is replaced, the rest overwritten.
#include "store.hpp"

#include <sqlite3.h>

#include <algorithm>
#include <cstdio>
#include <memory>
#include <set>
#include <tuple>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace
{
    using store::Position;

    /// @brief Each row of a query, read-only
    template<class F>
    int Rows(sqlite3* db, const char* sql, F&& row)
    {
        sqlite3_stmt* s = nullptr;
        int n = 0;
        if (sqlite3_prepare_v2(db, sql, -1, &s, nullptr) != SQLITE_OK)
        {
            std::fprintf(stderr, "  skipped (%s): %s\n", sqlite3_errmsg(db), sql);
            return 0;
        }
        while (sqlite3_step(s) == SQLITE_ROW)
        {
            row(s);
            ++n;
        }
        sqlite3_finalize(s);
        return n;
    }
    std::string Text(sqlite3_stmt* s, const int col) { const unsigned char* t = sqlite3_column_text(s, col); return t ? reinterpret_cast<const char*>(t) : ""; }
    int Int(sqlite3_stmt* s, const int col) { return sqlite3_column_int(s, col); }
    Position At(sqlite3_stmt* s, const int col) { return Position(Int(s, col), Int(s, col + 1), Int(s, col + 2)); }

    sqlite3* OpenReadOnly(const std::string& path)
    {
        sqlite3* db = nullptr;
        if (sqlite3_open_v2(path.c_str(), &db, SQLITE_OPEN_READONLY, nullptr) != SQLITE_OK)
        {
            std::fprintf(stderr, "can't open %s: %s\n", path.c_str(), db ? sqlite3_errmsg(db) : "out of memory");
            sqlite3_close(db);
            return nullptr;
        }
        return db;
    }

    // An empty box: ReplaceBox then takes nothing back and only writes
    const store::Box nothing{ Position(1, 1, 1), Position(0, 0, 0) };
}

int main(int argc, char* argv[])
{
    const std::string memory_dir = argc > 1 ? argv[1] : "../memory";
    const std::string shared_path = argc > 2 ? argv[2] : "../botcraft.sqlite";
    if (store::Backend() == "sqlite")
    {
        std::fprintf(stderr, "BOTCRAFT_DB names no other store (postgresql://... or valkey://...): nothing to import into\n");
        return 2;
    }
    store::Shared* target = store::SharedStore();
    if (target == nullptr)
    {
        std::fprintf(stderr, "can't open the %s store: %s\n", store::Backend().c_str(), store::Error().c_str());
        return 1;
    }
    bool ok = true;

    // Every bot's memory
    std::vector<std::filesystem::path> files;
    if (std::filesystem::is_directory(memory_dir))
    {
        for (const auto& entry : std::filesystem::directory_iterator(memory_dir))
        {
            if (entry.path().extension() == ".sqlite") files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    size_t total_places = 0;
    for (const auto& file : files)
    {
        const std::string bot = file.stem().string();
        sqlite3* db = OpenReadOnly(file.string());
        std::unique_ptr<store::Memory> memory = db ? store::OpenMemory(bot) : nullptr;
        if (!memory)
        {
            sqlite3_close(db);
            ok = false;
            continue;
        }
        // A copy, not a merge: what the store held for the bot (an earlier import) goes first
        memory->Clear();
        const int doing = Rows(db, "SELECT key, value FROM doing", [&](sqlite3_stmt* s) { memory->SaveDoing(Text(s, 0), Text(s, 1)); });
        const int own = Rows(db, "SELECT what, x, y, z FROM own", [&](sqlite3_stmt* s) { memory->SetOwn(Text(s, 0), At(s, 1)); });
        // Places by dimension, a few thousand at a time
        std::map<std::string, std::vector<store::Place>> places;
        const int count = Rows(db, "SELECT kind, dimension, x, y, z FROM places", [&](sqlite3_stmt* s) { places[Text(s, 1)].push_back({ Text(s, 0), At(s, 2) }); });
        for (auto& [dimension, list] : places)
        {
            for (size_t i = 0; i < list.size(); i += 5000)
            {
                memory->ReplaceBox(dimension, nothing, std::vector<store::Place>(list.begin() + i, list.begin() + std::min(list.size(), i + 5000)));
            }
        }
        // What arrived
        int arrived = 0;
        for (const auto& [kind, n] : memory->Summary(1000000)) arrived += n;
        const bool same = arrived == count && memory->LoadDoing().size() == static_cast<size_t>(doing);
        ok = ok && same;
        total_places += static_cast<size_t>(count);
        std::printf("%-12s %6d places, %d own, %d doing%s\n", bot.c_str(), count, own, doing, same ? "" : "  MISMATCH: places or doing in the store differ");
        sqlite3_close(db);
    }

    // What they share
    sqlite3* db = OpenReadOnly(shared_path);
    if (db == nullptr)
    {
        return 1;
    }
    std::map<std::pair<std::string, std::string>, std::vector<store::Place>> finds;   // (dimension, by) -> finds
    const int find_count = Rows(db, "SELECT kind, dimension, x, y, z, by FROM finds", [&](sqlite3_stmt* s) { finds[{ Text(s, 1), Text(s, 5) }].push_back({ Text(s, 0), At(s, 2) }); });
    for (const auto& [key, list] : finds)
    {
        for (size_t i = 0; i < list.size(); i += 5000)
        {
            target->ReportFinds(key.first, nothing, std::vector<store::Place>(list.begin() + i, list.begin() + std::min(list.size(), i + 5000)), key.second);
        }
    }
    // Chests: one call per chest, with what it held and who saw it last
    struct Chest { std::map<std::string, int> contents; std::string by; };
    std::map<std::tuple<std::string, int, int, int>, Chest> chests;
    Rows(db, "SELECT dimension, x, y, z, item, count, by FROM chest_contents", [&](sqlite3_stmt* s)
        {
            Chest& chest = chests[{ Text(s, 0), Int(s, 1), Int(s, 2), Int(s, 3) }];
            if (!Text(s, 4).empty()) chest.contents[Text(s, 4)] = Int(s, 5);
            chest.by = Text(s, 6);
        });
    for (const auto& [at, chest] : chests)
    {
        target->RememberChest(std::get<0>(at), Position(std::get<1>(at), std::get<2>(at), std::get<3>(at)), chest.contents, chest.by);
    }
    std::map<std::string, Position> chest_for;
    Rows(db, "SELECT item, x, y, z FROM chests", [&](sqlite3_stmt* s) { chest_for[Text(s, 0)] = At(s, 1); });
    target->SaveChestFor(chest_for);
    int sites = 0, mines = 0;
    if (target->Sites().empty())
    {
        // All of them, roads too, in their order
        sites = Rows(db, "SELECT x1, y1, z1, x2, y2, z2, name FROM sites ORDER BY rowid", [&](sqlite3_stmt* s) { target->AddSite({ At(s, 0), At(s, 3), Text(s, 6) }); });
    }
    else
    {
        std::printf("sites: the store has some already, not copied\n");
    }
    if (target->Mines().empty())
    {
        mines = Rows(db, "SELECT x, y, z, resources FROM mines ORDER BY rowid", [&](sqlite3_stmt* s) { target->AddMine({ At(s, 0), Text(s, 3) }); });
    }
    else
    {
        std::printf("mines: the store has some already, not copied\n");
    }
    const int roles = Rows(db, "SELECT bot, role FROM roles ORDER BY rowid", [&](sqlite3_stmt* s) { target->SetRole(Text(s, 0), Text(s, 1), true); });
    sqlite3_close(db);

    // What arrived
    size_t finds_arrived = 0;
    std::set<std::string> dimensions;
    for (const auto& [key, list] : finds) dimensions.insert(key.first);
    for (const std::string& dimension : dimensions) finds_arrived += target->NearestFinds(dimension, "", Position(0, 0, 0), 100000000).size();
    const bool shared_same = finds_arrived == static_cast<size_t>(find_count) && target->ChestFor().size() == chest_for.size() &&
        target->Chests(100000000).size() >= chests.size();
    ok = ok && shared_same;
    std::printf("shared: %d finds, %zu chests, %zu chest items, %d sites, %d mines, %d roles%s\n", find_count, chests.size(), chest_for.size(),
        sites, mines, roles, shared_same ? "" : "  MISMATCH: finds or chests in the store differ");
    std::printf("%s: %zu bots, %zu places into %s\n", ok ? "imported" : "imported with mismatches", files.size(), total_places, store::Backend().c_str());
    if (!store::Error().empty()) std::printf("store error: %s\n", store::Error().c_str());
    return ok && store::Error().empty() ? 0 : 1;
}
