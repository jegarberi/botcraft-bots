// The bots' stores: SQLite (default), PostgreSQL and Valkey. See store.hpp
#include "store.hpp"

#include <sqlite3.h>
#ifdef BOTCRAFT_WITH_POSTGRES
#include <libpq-fe.h>
#endif
#ifdef BOTCRAFT_WITH_VALKEY
#include <hiredis/hiredis.h>
#endif

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <ctime>
#include <filesystem>
#include <functional>
#include <iostream>
#include <mutex>
#include <set>
#include <sstream>
#include <tuple>

namespace store
{
    bool Box::Contains(const Position& p) const
    {
        return p.x >= low.x && p.x <= high.x && p.y >= low.y && p.y <= high.y && p.z >= low.z && p.z <= high.z;
    }

    namespace
    {
        constexpr long long mob_memory_seconds = 1800;

        long long Now() { return static_cast<long long>(std::time(nullptr)); }

        bool EndsWith(const std::string& s, const std::string& end)
        {
            return s.size() >= end.size() && s.compare(s.size() - end.size(), end.size(), end) == 0;
        }

        long long Distance(const Position& a, const Position& b)
        {
            const long long dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
            return dx * dx + dy * dy + dz * dz;
        }

        /// @brief The nearest max of places to me (places not already in out), added to out, out sorted nearest first
        void MergeNearest(std::vector<Place>& out, std::vector<Place> places, const Position& me, const int max)
        {
            for (Place& place : places)
            {
                if (std::none_of(out.begin(), out.end(), [&](const Place& known) { return known.p == place.p; })) out.push_back(std::move(place));
            }
            std::sort(out.begin(), out.end(), [&](const Place& a, const Place& b) { return Distance(a.p, me) < Distance(b.p, me); });
            if (out.size() > static_cast<size_t>(std::max(0, max))) out.resize(static_cast<size_t>(std::max(0, max)));
        }

        std::string Key(const Position& p) { return std::to_string(p.x) + "," + std::to_string(p.y) + "," + std::to_string(p.z); }

        std::optional<Position> ParsePosition(const std::string& s)
        {
            Position p;
            char c1 = 0, c2 = 0;
            std::istringstream in(s);
            if (in >> p.x >> c1 >> p.y >> c2 >> p.z && c1 == ',' && c2 == ',') return p;
            return std::nullopt;
        }

        Position CellOf(const Position& p) { return Position(p.x >> 4, p.y >> 4, p.z >> 4); }

        /// @brief The kinds of a table ending in suffix, from the list of its kinds, which is read again once a minute.
        /// Tables hold tens of thousands of places of a few dozen kinds: looking kinds up exactly uses their index,
        /// where LIKE '%suffix' read every row (59% of the program's CPU)
        class KindCache
        {
        public:
            std::vector<std::string> Ending(const std::string& key, const std::string& suffix, const std::function<std::vector<std::string>()>& list)
            {
                std::vector<std::string> all;
                {
                    std::lock_guard<std::mutex> lock(mutex);
                    const auto found = cache.find(key);
                    if (found != cache.end() && std::chrono::steady_clock::now() - found->second.first < std::chrono::minutes(1)) all = found->second.second;
                }
                if (all.empty())
                {
                    all = list();
                    std::lock_guard<std::mutex> lock(mutex);
                    cache[key] = { std::chrono::steady_clock::now(), all };
                }
                std::vector<std::string> matching;
                for (const std::string& kind : all) if (EndsWith(kind, suffix)) matching.push_back(kind);
                return matching;
            }
            void Add(const std::string& key, const std::string& kind)
            {
                std::lock_guard<std::mutex> lock(mutex);
                const auto found = cache.find(key);
                if (found != cache.end() && std::find(found->second.second.begin(), found->second.second.end(), kind) == found->second.second.end())
                {
                    found->second.second.push_back(kind);
                }
            }
        private:
            std::mutex mutex;
            std::map<std::string, std::pair<std::chrono::steady_clock::time_point, std::vector<std::string>>> cache;
        };
        KindCache kind_cache;

        std::string backend_error;
        std::mutex backend_mutex;
        void Fail(const std::string& what)
        {
            std::lock_guard<std::mutex> lock(backend_mutex);
            if (backend_error.empty()) std::cerr << "store: " << what << std::endl;
            backend_error = what;
        }

        std::string Url()
        {
            const char* url = std::getenv("BOTCRAFT_DB");
            return url == nullptr ? "" : url;
        }
    }

    std::string Backend()
    {
        const std::string url = Url();
        if (url.rfind("postgres", 0) == 0) return "postgresql";
        if (url.rfind("valkey", 0) == 0 || url.rfind("redis", 0) == 0) return "valkey";
        return "sqlite";
    }

    std::string Error()
    {
        std::lock_guard<std::mutex> lock(backend_mutex);
        return backend_error;
    }

    // ------------------------------------------------------------------------------------------------ SQLite

    namespace
    {
        /// @brief A prepared statement: bound with Bind(index, value) (1-based, like SQLite), stepped with Next()
        class Stmt
        {
        public:
            Stmt(sqlite3* db, const std::string& sql) { if (db == nullptr || sqlite3_prepare_v2(db, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) { sqlite3_finalize(statement); statement = nullptr; } }
            ~Stmt() { sqlite3_finalize(statement); }
            Stmt(const Stmt&) = delete;
            Stmt& operator=(const Stmt&) = delete;
            explicit operator bool() const { return statement != nullptr; }
            Stmt& Bind(const int i, const std::string& v) { sqlite3_bind_text(statement, i, v.c_str(), -1, SQLITE_TRANSIENT); return *this; }
            Stmt& Bind(const int i, const int v) { sqlite3_bind_int(statement, i, v); return *this; }
            Stmt& Bind(const int i, const long long v) { sqlite3_bind_int64(statement, i, v); return *this; }
            bool Next() { return statement != nullptr && sqlite3_step(statement) == SQLITE_ROW; }
            void Run() { Next(); }
            void Reset() { sqlite3_reset(statement); }
            int Int(const int col) { return sqlite3_column_int(statement, col); }
            long long Int64(const int col) { return sqlite3_column_int64(statement, col); }
            std::string Text(const int col) { const unsigned char* t = sqlite3_column_text(statement, col); return t == nullptr ? "" : reinterpret_cast<const char*>(t); }
        private:
            sqlite3_stmt* statement = nullptr;
        };

        void Exec(sqlite3* db, const char* sql) { sqlite3_exec(db, sql, nullptr, nullptr, nullptr); }

        /// @brief End a write transaction: rolled back if the commit fails (left open it would hold the write lock for good)
        void CommitOrRollback(sqlite3* db)
        {
            if (sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr) != SQLITE_OK) Exec(db, "ROLLBACK");
        }

        /// @brief Places of a places-like table (kind, dimension, x, y, z), kinds ending in suffix, nearest first
        std::vector<Place> SqliteNearest(sqlite3* db, const std::string& table, const std::string& cache_key, const std::string& dimension,
            const std::string& suffix, const Position& me, const int max)
        {
            std::vector<Place> places;
            const auto list = [&]()
                {
                    std::vector<std::string> kinds;
                    Stmt s(db, "SELECT DISTINCT kind FROM " + table);
                    while (s.Next()) kinds.push_back(s.Text(0));
                    return kinds;
                };
            for (const std::string& kind : kind_cache.Ending(cache_key, suffix, list))
            {
                Stmt s(db, "SELECT kind, x, y, z FROM " + table + " WHERE kind = ?2 AND dimension = ?1"
                    " ORDER BY (x - ?3) * (x - ?3) + (y - ?4) * (y - ?4) + (z - ?5) * (z - ?5) LIMIT ?6");
                s.Bind(1, dimension).Bind(2, kind).Bind(3, me.x).Bind(4, me.y).Bind(5, me.z).Bind(6, max);
                std::vector<Place> some;
                while (s.Next()) some.push_back({ s.Text(0), Position(s.Int(1), s.Int(2), s.Int(3)) });
                MergeNearest(places, std::move(some), me, max);
            }
            return places;
        }

        class SqliteMemory : public Memory
        {
        public:
            explicit SqliteMemory(sqlite3* db_, std::string path_) : db(db_), path(std::move(path_)) {}
            ~SqliteMemory() override { sqlite3_close(db); }

            void SaveDoing(const std::string& key, const std::string& value) override
            {
                if (value.empty()) Stmt(db, "DELETE FROM doing WHERE key = ?1").Bind(1, key).Run();
                else Stmt(db, "INSERT OR REPLACE INTO doing (key, value) VALUES (?1, ?2)").Bind(1, key).Bind(2, value).Run();
            }
            std::map<std::string, std::string> LoadDoing() override
            {
                std::map<std::string, std::string> doing;
                Stmt s(db, "SELECT key, value FROM doing");
                while (s.Next()) doing[s.Text(0)] = s.Text(1);
                return doing;
            }
            void SetOwn(const std::string& what, const Position& p) override
            {
                Stmt(db, "INSERT OR REPLACE INTO own (what, x, y, z) VALUES (?1, ?2, ?3, ?4)").Bind(1, what).Bind(2, p.x).Bind(3, p.y).Bind(4, p.z).Run();
            }
            void ForgetOwn(const std::string& what) override { Stmt(db, "DELETE FROM own WHERE what = ?1").Bind(1, what).Run(); }
            std::optional<Position> GetOwn(const std::string& what) override
            {
                Stmt s(db, "SELECT x, y, z FROM own WHERE what = ?1");
                s.Bind(1, what);
                if (s.Next()) return Position(s.Int(0), s.Int(1), s.Int(2));
                return std::nullopt;
            }
            void RememberPlace(const std::string& dimension, const std::string& kind, const Position& p) override
            {
                Stmt(db, "INSERT OR REPLACE INTO places (kind, dimension, x, y, z, seen) VALUES (?1, ?2, ?3, ?4, ?5, ?6)")
                    .Bind(1, kind).Bind(2, dimension).Bind(3, p.x).Bind(4, p.y).Bind(5, p.z).Bind(6, Now()).Run();
                kind_cache.Add(path, kind);
            }
            bool KnownInCell(const std::string& dimension, const std::string& kind, const Position& o) override
            {
                Stmt s(db, "SELECT 1 FROM places WHERE dimension = ?1 AND kind = ?2 AND x BETWEEN ?3 AND ?3 + 15 AND y BETWEEN ?4 AND ?4 + 15"
                    " AND z BETWEEN ?5 AND ?5 + 15 LIMIT 1");
                s.Bind(1, dimension).Bind(2, kind).Bind(3, o.x).Bind(4, o.y).Bind(5, o.z);
                return s.Next();
            }
            void ReplaceBox(const std::string& dimension, const Box& box, const std::vector<Place>& places) override
            {
                Exec(db, "BEGIN");
                Stmt(db, "DELETE FROM places WHERE dimension = ?1 AND x BETWEEN ?2 AND ?3 AND y BETWEEN ?4 AND ?5 AND z BETWEEN ?6 AND ?7")
                    .Bind(1, dimension).Bind(2, box.low.x).Bind(3, box.high.x).Bind(4, box.low.y).Bind(5, box.high.y).Bind(6, box.low.z).Bind(7, box.high.z).Run();
                const long long now = Now();
                Stmt insert(db, "INSERT OR REPLACE INTO places (kind, dimension, x, y, z, seen) VALUES (?1, ?2, ?3, ?4, ?5, ?6)");
                for (const Place& place : places)
                {
                    insert.Bind(1, place.kind).Bind(2, dimension).Bind(3, place.p.x).Bind(4, place.p.y).Bind(5, place.p.z).Bind(6, now).Run();
                    insert.Reset();
                    kind_cache.Add(path, place.kind);
                }
                Stmt(db, "DELETE FROM places WHERE kind LIKE 'mob:%' AND seen < ?1").Bind(1, now - mob_memory_seconds).Run();
                Exec(db, "COMMIT");
            }
            std::vector<Place> Nearest(const std::string& dimension, const std::string& suffix, const Position& me, const int max) override
            {
                return SqliteNearest(db, "places", path, dimension, suffix, me, max);
            }
            void Forget(const Position& p) override { Stmt(db, "DELETE FROM places WHERE x = ?1 AND y = ?2 AND z = ?3").Bind(1, p.x).Bind(2, p.y).Bind(3, p.z).Run(); }
            std::vector<std::pair<std::string, int>> Summary(const int max) override
            {
                std::vector<std::pair<std::string, int>> summary;
                Stmt s(db, "SELECT kind, COUNT(*) FROM places GROUP BY kind ORDER BY 2 DESC LIMIT ?1");
                s.Bind(1, max);
                while (s.Next()) summary.push_back({ s.Text(0), s.Int(1) });
                return summary;
            }
            void Clear() override { Exec(db, "DELETE FROM places; DELETE FROM own; DELETE FROM doing;"); }
            void Begin() override { Exec(db, "BEGIN"); }
            void Commit() override { Exec(db, "COMMIT"); }

        private:
            sqlite3* db;
            std::string path;
        };

        std::unique_ptr<Memory> OpenSqliteMemory(const std::string& bot)
        {
            const char* dir_env = std::getenv("BOTCRAFT_MEMORY_DIR");
            const std::string dir = dir_env != nullptr ? dir_env : "../memory";
            std::filesystem::create_directories(dir);
            const std::string path = dir + "/" + bot + ".sqlite";
            sqlite3* db = nullptr;
            if (sqlite3_open(path.c_str(), &db) != SQLITE_OK ||
                sqlite3_exec(db,
                    // Write-ahead log, never synced: the OS writes it out by itself. Synced at checkpoints, 100 bots writing
                    // every 10 seconds had the disk at 85% wait. A crash loses nothing; a power cut, the last seconds
                    "PRAGMA journal_mode = WAL; PRAGMA synchronous = OFF;"
                    "CREATE TABLE IF NOT EXISTS places ("
                    "  kind TEXT NOT NULL, dimension TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL,"
                    "  seen INTEGER NOT NULL, PRIMARY KEY (dimension, x, y, z));"
                    "CREATE INDEX IF NOT EXISTS places_kind ON places (kind);"
                    "CREATE TABLE IF NOT EXISTS own (what TEXT PRIMARY KEY, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS doing (key TEXT PRIMARY KEY, value TEXT NOT NULL);", nullptr, nullptr, nullptr) != SQLITE_OK)
            {
                Fail("can't open " + path + ": " + (db ? sqlite3_errmsg(db) : "out of memory"));
                sqlite3_close(db);
                return nullptr;
            }
            return std::make_unique<SqliteMemory>(db, path);
        }

        class SqliteShared : public Shared
        {
        public:
            explicit SqliteShared(sqlite3* db_) : db(db_) {}
            ~SqliteShared() override { sqlite3_close(db); }

            void ReportFinds(const std::string& dimension, const Box& box, const std::vector<Place>& finds, const std::string& by) override
            {
                if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return;   // busy: next time
                const long long now = Now();
                Stmt(db, "DELETE FROM finds WHERE dimension = ?1 AND x BETWEEN ?2 AND ?3 AND y BETWEEN ?4 AND ?5 AND z BETWEEN ?6 AND ?7"
                    " OR (kind LIKE 'mob:%' AND seen < ?8)")
                    .Bind(1, dimension).Bind(2, box.low.x).Bind(3, box.high.x).Bind(4, box.low.y).Bind(5, box.high.y).Bind(6, box.low.z).Bind(7, box.high.z)
                    .Bind(8, now - mob_memory_seconds).Run();
                Stmt insert(db, "INSERT OR REPLACE INTO finds (kind, dimension, cx, cy, cz, x, y, z, seen, by) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8, ?9, ?10)");
                std::set<std::tuple<std::string, int, int, int>> cells;
                for (const Place& f : finds)
                {
                    const Position cell = CellOf(f.p);
                    if (!cells.insert({ f.kind, cell.x, cell.y, cell.z }).second) continue;
                    insert.Bind(1, f.kind).Bind(2, dimension).Bind(3, cell.x).Bind(4, cell.y).Bind(5, cell.z).Bind(6, f.p.x).Bind(7, f.p.y).Bind(8, f.p.z)
                        .Bind(9, now).Bind(10, by).Run();
                    insert.Reset();
                    kind_cache.Add("finds", f.kind);
                }
                CommitOrRollback(db);
            }
            std::vector<Place> NearestFinds(const std::string& dimension, const std::string& suffix, const Position& me, const int max) override
            {
                return SqliteNearest(db, "finds", "finds", dimension, suffix, me, max);
            }
            void ForgetFind(const Position& p) override { Stmt(db, "DELETE FROM finds WHERE x = ?1 AND y = ?2 AND z = ?3").Bind(1, p.x).Bind(2, p.y).Bind(3, p.z).Run(); }

            std::vector<Position> ChestsIn(const std::string& dimension, const Box& box) override
            {
                std::vector<Position> chests;
                Stmt s(db, "SELECT DISTINCT x, y, z FROM chest_contents WHERE dimension = ?1 AND x BETWEEN ?2 AND ?3 AND y BETWEEN ?4 AND ?5 AND z BETWEEN ?6 AND ?7");
                s.Bind(1, dimension).Bind(2, box.low.x).Bind(3, box.high.x).Bind(4, box.low.y).Bind(5, box.high.y).Bind(6, box.low.z).Bind(7, box.high.z);
                while (s.Next()) chests.push_back(Position(s.Int(0), s.Int(1), s.Int(2)));
                return chests;
            }
            void ForgetChest(const std::string& dimension, const Position& p) override
            {
                Stmt(db, "DELETE FROM chest_contents WHERE dimension = ?1 AND x = ?2 AND y = ?3 AND z = ?4").Bind(1, dimension).Bind(2, p.x).Bind(3, p.y).Bind(4, p.z).Run();
            }
            void RememberChest(const std::string& dimension, const Position& p, const std::map<std::string, int>& contents, const std::string& by) override
            {
                if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return;   // busy: next time
                ForgetChest(dimension, p);
                std::map<std::string, int> rows = contents;
                rows[""] += 0;      // the chest itself, empty or not
                Stmt insert(db, "INSERT INTO chest_contents (dimension, x, y, z, item, count, seen, by) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7, ?8)");
                for (const auto& [item, count] : rows)
                {
                    insert.Bind(1, dimension).Bind(2, p.x).Bind(3, p.y).Bind(4, p.z).Bind(5, item).Bind(6, count).Bind(7, Now()).Bind(8, by).Run();
                    insert.Reset();
                }
                CommitOrRollback(db);
            }
            std::optional<Position> ChestHolding(const std::string& dimension, const std::string& item, const Position& me) override
            {
                Stmt s(db, "SELECT x, y, z FROM chest_contents WHERE dimension = ?1 AND item = ?2 AND count > 0"
                    " ORDER BY (x - ?3) * (x - ?3) + (y - ?4) * (y - ?4) + (z - ?5) * (z - ?5) LIMIT 1");
                s.Bind(1, dimension).Bind(2, item).Bind(3, me.x).Bind(4, me.y).Bind(5, me.z);
                if (s.Next()) return Position(s.Int(0), s.Int(1), s.Int(2));
                return std::nullopt;
            }
            std::map<std::string, int> ChestItems(const std::string& dimension, const Position& p) override
            {
                std::map<std::string, int> items;
                Stmt s(db, "SELECT item, count FROM chest_contents WHERE dimension = ?1 AND x = ?2 AND y = ?3 AND z = ?4 AND item <> ''");
                s.Bind(1, dimension).Bind(2, p.x).Bind(3, p.y).Bind(4, p.z);
                while (s.Next()) items[s.Text(0)] = s.Int(1);
                return items;
            }
            std::vector<ChestSeen> Chests(const int max) override
            {
                std::vector<ChestSeen> list;
                Stmt s(db, "SELECT x, y, z, dimension, MAX(seen), MAX(by) FROM chest_contents GROUP BY dimension, x, y, z ORDER BY MAX(seen) DESC LIMIT ?1");
                s.Bind(1, max);
                while (s.Next()) list.push_back({ Position(s.Int(0), s.Int(1), s.Int(2)), s.Text(3), s.Int64(4), s.Text(5), 0 });
                return list;
            }
            std::vector<ChestSeen> ChestsWith(const std::string& item, const int max) override
            {
                std::vector<ChestSeen> list;
                Stmt s(db, "SELECT x, y, z, dimension, seen, by, count FROM chest_contents WHERE item = ?1 AND count > 0 ORDER BY count DESC LIMIT ?2");
                s.Bind(1, item).Bind(2, max);
                while (s.Next()) list.push_back({ Position(s.Int(0), s.Int(1), s.Int(2)), s.Text(3), s.Int64(4), s.Text(5), s.Int(6) });
                return list;
            }
            std::map<std::string, int> Stored(const std::string& dimension) override
            {
                std::map<std::string, int> stored;
                Stmt s(db, "SELECT item, SUM(count) FROM chest_contents WHERE dimension = ?1 AND item != '' AND count > 0 GROUP BY item");
                s.Bind(1, dimension);
                while (s.Next()) stored[s.Text(0)] = s.Int(1);
                return stored;
            }

            std::vector<Site> Sites() override
            {
                std::vector<Site> sites;
                Stmt s(db, "SELECT x1, y1, z1, x2, y2, z2, name FROM sites WHERE name NOT LIKE '%_road' ORDER BY rowid");
                while (s.Next()) sites.push_back({ Position(s.Int(0), s.Int(1), s.Int(2)), Position(s.Int(3), s.Int(4), s.Int(5)), s.Text(6) });
                return sites;
            }
            void AddSite(const Site& site) override
            {
                Stmt(db, "INSERT INTO sites (x1, y1, z1, x2, y2, z2, name) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)")
                    .Bind(1, site.low.x).Bind(2, site.low.y).Bind(3, site.low.z).Bind(4, site.high.x).Bind(5, site.high.y).Bind(6, site.high.z).Bind(7, site.name).Run();
            }
            void RemoveSite(const size_t index) override
            {
                Stmt(db, "DELETE FROM sites WHERE rowid = (SELECT rowid FROM sites WHERE name NOT LIKE '%_road' ORDER BY rowid LIMIT 1 OFFSET ?1)")
                    .Bind(1, static_cast<int>(index)).Run();
            }
            std::map<std::string, Position> ChestFor() override
            {
                std::map<std::string, Position> chests;
                Stmt s(db, "SELECT item, x, y, z FROM chests");
                while (s.Next()) chests[s.Text(0)] = Position(s.Int(1), s.Int(2), s.Int(3));
                return chests;
            }
            void SaveChestFor(const std::map<std::string, Position>& chests) override
            {
                if (sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr) != SQLITE_OK) return;   // busy: with the next change
                Exec(db, "DELETE FROM chests");
                Stmt insert(db, "INSERT INTO chests (item, x, y, z) VALUES (?1, ?2, ?3, ?4)");
                for (const auto& [item, p] : chests)
                {
                    insert.Bind(1, item).Bind(2, p.x).Bind(3, p.y).Bind(4, p.z).Run();
                    insert.Reset();
                }
                CommitOrRollback(db);
            }
            std::vector<Mine> Mines() override
            {
                std::vector<Mine> mines;
                Stmt s(db, "SELECT x, y, z, resources FROM mines ORDER BY rowid");
                while (s.Next()) mines.push_back({ Position(s.Int(0), s.Int(1), s.Int(2)), s.Text(3) });
                return mines;
            }
            void AddMine(const Mine& mine) override
            {
                Stmt(db, "INSERT INTO mines (x, y, z, resources) VALUES (?1, ?2, ?3, ?4)").Bind(1, mine.entrance.x).Bind(2, mine.entrance.y).Bind(3, mine.entrance.z)
                    .Bind(4, mine.resources).Run();
            }
            void SetRole(const std::string& bot, const std::string& role, const bool on) override
            {
                Stmt(db, on ? "INSERT OR IGNORE INTO roles (bot, role) VALUES (?1, ?2)" : "DELETE FROM roles WHERE bot = ?1 AND role = ?2").Bind(1, bot).Bind(2, role).Run();
            }
            std::vector<std::string> WithRole(const std::string& role) override
            {
                std::vector<std::string> bots;
                Stmt s(db, "SELECT bot FROM roles WHERE role = ?1 ORDER BY rowid");
                s.Bind(1, role);
                while (s.Next()) bots.push_back(s.Text(0));
                return bots;
            }
            std::string RoleStartingWith(const std::string& bot, const std::string& prefix) override
            {
                Stmt s(db, "SELECT role FROM roles WHERE bot = ?1 AND substr(role, 1, length(?2)) = ?2 ORDER BY rowid");
                s.Bind(1, bot).Bind(2, prefix);
                return s.Next() ? s.Text(0) : "";
            }

        private:
            sqlite3* db;
        };

        // One connection per thread: with every bot in one process, one connection made them all queue and mixed one
        // thread's statements into another's transaction. In WAL mode the connections read side by side; the busy
        // timeout makes writers wait for each other
        SqliteShared* OpenSqliteShared()
        {
            const char* path_env = std::getenv("BOTCRAFT_SHARED_DB");
            const std::string path = path_env != nullptr ? path_env : "../botcraft.sqlite";
            sqlite3* db = nullptr;
            if (sqlite3_open(path.c_str(), &db) != SQLITE_OK || sqlite3_busy_timeout(db, 5000) != SQLITE_OK ||
                sqlite3_exec(db,
                    // A small page cache per connection: there is one per thread. Never synced, like the memories
                    "PRAGMA journal_mode = WAL; PRAGMA synchronous = OFF; PRAGMA cache_size = -512;"
                    // The log back to 64 MB at most after each checkpoint (it had grown to 1.6 GB)
                    "PRAGMA journal_size_limit = 67108864;"
                    "CREATE TABLE IF NOT EXISTS sites (x1 INTEGER NOT NULL, y1 INTEGER NOT NULL, z1 INTEGER NOT NULL,"
                    "  x2 INTEGER NOT NULL, y2 INTEGER NOT NULL, z2 INTEGER NOT NULL, name TEXT NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS chests (item TEXT PRIMARY KEY, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS mines (x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL, resources TEXT NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS roles (bot TEXT NOT NULL, role TEXT NOT NULL, PRIMARY KEY (bot, role));"
                    "CREATE TABLE IF NOT EXISTS finds (kind TEXT NOT NULL, dimension TEXT NOT NULL, cx INTEGER NOT NULL, cy INTEGER NOT NULL,"
                    "  cz INTEGER NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL, seen INTEGER NOT NULL, by TEXT NOT NULL,"
                    "  PRIMARY KEY (kind, dimension, cx, cy, cz));"
                    "CREATE INDEX IF NOT EXISTS finds_kind ON finds (kind);"
                    // A row with item '' marks the chest itself, empty or not
                    "CREATE TABLE IF NOT EXISTS chest_contents (dimension TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL,"
                    "  z INTEGER NOT NULL, item TEXT NOT NULL, count INTEGER NOT NULL, seen INTEGER NOT NULL, by TEXT NOT NULL,"
                    "  PRIMARY KEY (dimension, x, y, z, item));"
                    "CREATE INDEX IF NOT EXISTS chest_contents_item ON chest_contents (item);",
                    nullptr, nullptr, nullptr) != SQLITE_OK)
            {
                Fail("can't open " + path + ": " + (db ? sqlite3_errmsg(db) : "out of memory"));
                sqlite3_close(db);
                return nullptr;
            }
            return new SqliteShared(db);
        }
    }

    // ------------------------------------------------------------------------------------------------ connection pool

    namespace
    {
        /// @brief A few connections to a server, lent to one thread at a time (a connection is not thread-safe, and one
        /// per thread would be ~600 of them). BOTCRAFT_DB_POOL sets how many (default 8)
        template<class C>
        class Pool
        {
        public:
            Pool(std::function<C*()> open_, std::function<void(C*)> close_, std::function<bool(C*)> ok_) : open(std::move(open_)), close(std::move(close_)), ok(std::move(ok_))
            {
                const char* size_env = std::getenv("BOTCRAFT_DB_POOL");
                size = size_env != nullptr ? std::max(1, std::atoi(size_env)) : 8;
            }
            class Lease
            {
            public:
                Lease(Pool* pool_, C* c_) : pool(pool_), c(c_) {}
                ~Lease() { if (pool != nullptr) pool->Give(c); }
                Lease(Lease&& o) noexcept : pool(o.pool), c(o.c) { o.pool = nullptr; o.c = nullptr; }
                Lease(const Lease&) = delete;
                C* operator->() const { return c; }
                C* get() const { return c; }
                explicit operator bool() const { return c != nullptr; }
            private:
                Pool* pool;
                C* c;
            };
            Lease Take()
            {
                std::unique_lock<std::mutex> lock(mutex);
                available.wait(lock, [&]() { return !idle.empty() || open_count < size; });
                C* c = nullptr;
                if (!idle.empty())
                {
                    c = idle.back();
                    idle.pop_back();
                }
                else
                {
                    ++open_count;
                    lock.unlock();
                    c = open();
                    lock.lock();
                    if (c == nullptr) { --open_count; available.notify_one(); }
                }
                return Lease(this, c);
            }
        private:
            void Give(C* c)
            {
                if (c == nullptr) return;
                std::lock_guard<std::mutex> lock(mutex);
                if (ok(c)) idle.push_back(c);
                else { close(c); --open_count; }       // broken (the server went away): a new one next time
                available.notify_one();
            }
            std::function<C*()> open;
            std::function<void(C*)> close;
            std::function<bool(C*)> ok;
            std::mutex mutex;
            std::condition_variable available;
            std::vector<C*> idle;
            int open_count = 0;
            int size = 8;
        };
    }

    // ------------------------------------------------------------------------------------------------ PostgreSQL

#ifdef BOTCRAFT_WITH_POSTGRES
    namespace
    {
        /// @brief A query's result: rows of text values
        class PgResult
        {
        public:
            explicit PgResult(PGresult* r_) : r(r_) {}
            ~PgResult() { PQclear(r); }
            PgResult(const PgResult&) = delete;
            PgResult(PgResult&& o) noexcept : r(o.r) { o.r = nullptr; }
            bool Ok() const { const ExecStatusType s = PQresultStatus(r); return s == PGRES_COMMAND_OK || s == PGRES_TUPLES_OK; }
            int Rows() const { return Ok() ? PQntuples(r) : 0; }
            std::string Text(const int row, const int col) const { return PQgetvalue(r, row, col); }
            int Int(const int row, const int col) const { return std::atoi(PQgetvalue(r, row, col)); }
            long long Int64(const int row, const int col) const { return std::atoll(PQgetvalue(r, row, col)); }
            std::string ErrorText() const { return PQresultErrorMessage(r); }
        private:
            PGresult* r;
        };

        PGconn* PgOpen();

        Pool<PGconn>& PgPool()
        {
            static Pool<PGconn> pool(PgOpen, PQfinish, [](PGconn* c) { return PQstatus(c) == CONNECTION_OK; });
            return pool;
        }

        std::string ToText(const std::string& v) { return v; }
        std::string ToText(const char* v) { return v; }
        std::string ToText(const int v) { return std::to_string(v); }
        std::string ToText(const long long v) { return std::to_string(v); }
        std::string ToText(const size_t v) { return std::to_string(v); }

        template<class... A>
        PgResult Pq(PGconn* c, const std::string& sql, const A&... args)
        {
            const std::vector<std::string> values = { ToText(args)... };
            std::vector<const char*> pointers;
            for (const std::string& v : values) pointers.push_back(v.c_str());
            PgResult result(PQexecParams(c, sql.c_str(), static_cast<int>(pointers.size()), nullptr, pointers.empty() ? nullptr : pointers.data(), nullptr, nullptr, 0));
            if (!result.Ok()) Fail("postgresql: " + result.ErrorText());
            return result;
        }

        PGconn* PgOpen()
        {
            PGconn* c = PQconnectdb(Url().c_str());
            if (PQstatus(c) != CONNECTION_OK)
            {
                Fail(std::string("postgresql: can't connect: ") + PQerrorMessage(c));
                PQfinish(c);
                return nullptr;
            }
            // Notices ("relation already exists, skipping") stay out of the log
            PQsetNoticeProcessor(c, [](void*, const char*) {}, nullptr);
            // The tables, once per connection (cheap when they exist). The memories of all the bots share a table
            // set, keyed by bot; "seen_by" for SQLite's "by" (a reserved word here)
            static std::once_flag schema;
            std::call_once(schema, [&]()
                {
                    PgResult r(PQexec(c,
                        "CREATE TABLE IF NOT EXISTS places (bot TEXT NOT NULL, kind TEXT NOT NULL, dimension TEXT NOT NULL, x INTEGER NOT NULL,"
                        "  y INTEGER NOT NULL, z INTEGER NOT NULL, seen BIGINT NOT NULL, PRIMARY KEY (bot, dimension, x, y, z));"
                        "CREATE INDEX IF NOT EXISTS places_kind ON places (bot, kind);"
                        "CREATE TABLE IF NOT EXISTS own (bot TEXT NOT NULL, what TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL,"
                        "  PRIMARY KEY (bot, what));"
                        "CREATE TABLE IF NOT EXISTS doing (bot TEXT NOT NULL, key TEXT NOT NULL, value TEXT NOT NULL, PRIMARY KEY (bot, key));"
                        "CREATE TABLE IF NOT EXISTS sites (id BIGSERIAL PRIMARY KEY, x1 INTEGER NOT NULL, y1 INTEGER NOT NULL, z1 INTEGER NOT NULL,"
                        "  x2 INTEGER NOT NULL, y2 INTEGER NOT NULL, z2 INTEGER NOT NULL, name TEXT NOT NULL);"
                        "CREATE TABLE IF NOT EXISTS chests (item TEXT PRIMARY KEY, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL);"
                        "CREATE TABLE IF NOT EXISTS mines (id BIGSERIAL PRIMARY KEY, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL, resources TEXT NOT NULL);"
                        "CREATE TABLE IF NOT EXISTS roles (id BIGSERIAL, bot TEXT NOT NULL, role TEXT NOT NULL, PRIMARY KEY (bot, role));"
                        "CREATE TABLE IF NOT EXISTS finds (kind TEXT NOT NULL, dimension TEXT NOT NULL, cx INTEGER NOT NULL, cy INTEGER NOT NULL,"
                        "  cz INTEGER NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL, seen BIGINT NOT NULL, seen_by TEXT NOT NULL,"
                        "  PRIMARY KEY (kind, dimension, cx, cy, cz));"
                        "CREATE INDEX IF NOT EXISTS finds_kind ON finds (kind);"
                        "CREATE TABLE IF NOT EXISTS chest_contents (dimension TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL,"
                        "  item TEXT NOT NULL, count INTEGER NOT NULL, seen BIGINT NOT NULL, seen_by TEXT NOT NULL, PRIMARY KEY (dimension, x, y, z, item));"
                        "CREATE INDEX IF NOT EXISTS chest_contents_item ON chest_contents (item);"));
                    if (!r.Ok()) Fail("postgresql: can't create the tables: " + r.ErrorText());
                });
            return c;
        }

        /// @brief A Postgres array literal of text values
        std::string TextArray(const std::vector<std::string>& values)
        {
            std::string out = "{";
            for (size_t i = 0; i < values.size(); ++i)
            {
                if (i > 0) out += ",";
                out += "\"";
                for (const char ch : values[i]) { if (ch == '"' || ch == '\\') out += '\\'; out += ch; }
                out += "\"";
            }
            return out + "}";
        }
        std::string IntArray(const std::vector<int>& values)
        {
            std::string out = "{";
            for (size_t i = 0; i < values.size(); ++i) out += (i > 0 ? "," : "") + std::to_string(values[i]);
            return out + "}";
        }

        std::vector<Place> PgNearest(PGconn* c, const std::string& from, const std::string& where_bot, const std::string& bot, const std::string& cache_key,
            const std::string& dimension, const std::string& suffix, const Position& me, const int max)
        {
            std::vector<Place> places;
            const auto list = [&]()
                {
                    std::vector<std::string> kinds;
                    PgResult r = where_bot.empty() ? Pq(c, "SELECT DISTINCT kind FROM " + from) : Pq(c, "SELECT DISTINCT kind FROM " + from + " WHERE bot = $1", bot);
                    for (int i = 0; i < r.Rows(); ++i) kinds.push_back(r.Text(i, 0));
                    return kinds;
                };
            for (const std::string& kind : kind_cache.Ending(cache_key, suffix, list))
            {
                const std::string order = " ORDER BY (x - $3) * (x - $3) + (y - $4) * (y - $4) + (z - $5) * (z - $5) LIMIT $6";
                PgResult r = where_bot.empty()
                    ? Pq(c, "SELECT kind, x, y, z FROM " + from + " WHERE kind = $2 AND dimension = $1" + order, dimension, kind, me.x, me.y, me.z, max)
                    : Pq(c, "SELECT kind, x, y, z FROM " + from + " WHERE bot = $7 AND kind = $2 AND dimension = $1" + order, dimension, kind, me.x, me.y, me.z, max, bot);
                std::vector<Place> some;
                for (int i = 0; i < r.Rows(); ++i) some.push_back({ r.Text(i, 0), Position(r.Int(i, 1), r.Int(i, 2), r.Int(i, 3)) });
                MergeNearest(places, std::move(some), me, max);
            }
            return places;
        }

        class PgMemory : public Memory
        {
        public:
            explicit PgMemory(std::string bot_) : bot(std::move(bot_)) {}
            void SaveDoing(const std::string& key, const std::string& value) override
            {
                auto c = PgPool().Take();
                if (!c) return;
                if (value.empty()) Pq(c.get(), "DELETE FROM doing WHERE bot = $1 AND key = $2", bot, key);
                else Pq(c.get(), "INSERT INTO doing (bot, key, value) VALUES ($1, $2, $3) ON CONFLICT (bot, key) DO UPDATE SET value = EXCLUDED.value", bot, key, value);
            }
            std::map<std::string, std::string> LoadDoing() override
            {
                std::map<std::string, std::string> doing;
                auto c = PgPool().Take();
                if (!c) return doing;
                PgResult r = Pq(c.get(), "SELECT key, value FROM doing WHERE bot = $1", bot);
                for (int i = 0; i < r.Rows(); ++i) doing[r.Text(i, 0)] = r.Text(i, 1);
                return doing;
            }
            void SetOwn(const std::string& what, const Position& p) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "INSERT INTO own (bot, what, x, y, z) VALUES ($1, $2, $3, $4, $5) ON CONFLICT (bot, what) DO UPDATE SET x = EXCLUDED.x, y = EXCLUDED.y, z = EXCLUDED.z",
                    bot, what, p.x, p.y, p.z);
            }
            void ForgetOwn(const std::string& what) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "DELETE FROM own WHERE bot = $1 AND what = $2", bot, what);
            }
            std::optional<Position> GetOwn(const std::string& what) override
            {
                auto c = PgPool().Take();
                if (!c) return std::nullopt;
                PgResult r = Pq(c.get(), "SELECT x, y, z FROM own WHERE bot = $1 AND what = $2", bot, what);
                if (r.Rows() > 0) return Position(r.Int(0, 0), r.Int(0, 1), r.Int(0, 2));
                return std::nullopt;
            }
            void RememberPlace(const std::string& dimension, const std::string& kind, const Position& p) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "INSERT INTO places (bot, kind, dimension, x, y, z, seen) VALUES ($1, $2, $3, $4, $5, $6, $7)"
                    " ON CONFLICT (bot, dimension, x, y, z) DO UPDATE SET kind = EXCLUDED.kind, seen = EXCLUDED.seen", bot, kind, dimension, p.x, p.y, p.z, Now());
                kind_cache.Add("pg:" + bot, kind);
            }
            bool KnownInCell(const std::string& dimension, const std::string& kind, const Position& o) override
            {
                auto c = PgPool().Take();
                if (!c) return false;
                return Pq(c.get(), "SELECT 1 FROM places WHERE bot = $6 AND dimension = $1 AND kind = $2 AND x BETWEEN $3 AND $3 + 15 AND y BETWEEN $4 AND $4 + 15"
                    " AND z BETWEEN $5 AND $5 + 15 LIMIT 1", dimension, kind, o.x, o.y, o.z, bot).Rows() > 0;
            }
            void ReplaceBox(const std::string& dimension, const Box& box, const std::vector<Place>& places) override
            {
                auto c = PgPool().Take();
                if (!c) return;
                // The rows go as arrays, one statement for all (thousands of them a scan)
                std::vector<std::string> kinds;
                std::vector<int> xs, ys, zs;
                std::set<std::tuple<int, int, int>> at;
                for (auto it = places.rbegin(); it != places.rend(); ++it)
                {
                    // One row per position (the last written wins, as with INSERT OR REPLACE)
                    if (!at.insert({ it->p.x, it->p.y, it->p.z }).second) continue;
                    kinds.push_back(it->kind);
                    xs.push_back(it->p.x);
                    ys.push_back(it->p.y);
                    zs.push_back(it->p.z);
                    kind_cache.Add("pg:" + bot, it->kind);
                }
                const long long now = Now();
                Pq(c.get(), "BEGIN");
                Pq(c.get(), "DELETE FROM places WHERE bot = $8 AND dimension = $1 AND x BETWEEN $2 AND $3 AND y BETWEEN $4 AND $5 AND z BETWEEN $6 AND $7",
                    dimension, box.low.x, box.high.x, box.low.y, box.high.y, box.low.z, box.high.z, bot);
                if (!kinds.empty())
                {
                    Pq(c.get(), "INSERT INTO places (bot, kind, dimension, x, y, z, seen) SELECT $1, k, $2, x, y, z, $7"
                        " FROM unnest($3::text[], $4::int[], $5::int[], $6::int[]) AS t(k, x, y, z)"
                        " ON CONFLICT (bot, dimension, x, y, z) DO UPDATE SET kind = EXCLUDED.kind, seen = EXCLUDED.seen",
                        bot, dimension, TextArray(kinds), IntArray(xs), IntArray(ys), IntArray(zs), now);
                }
                Pq(c.get(), "DELETE FROM places WHERE bot = $1 AND kind LIKE 'mob:%' AND seen < $2", bot, now - mob_memory_seconds);
                if (!Pq(c.get(), "COMMIT").Ok()) Pq(c.get(), "ROLLBACK");
            }
            std::vector<Place> Nearest(const std::string& dimension, const std::string& suffix, const Position& me, const int max) override
            {
                auto c = PgPool().Take();
                if (!c) return {};
                return PgNearest(c.get(), "places", "bot", bot, "pg:" + bot, dimension, suffix, me, max);
            }
            void Forget(const Position& p) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "DELETE FROM places WHERE bot = $1 AND x = $2 AND y = $3 AND z = $4", bot, p.x, p.y, p.z);
            }
            void Clear() override
            {
                auto c = PgPool().Take();
                if (!c) return;
                Pq(c.get(), "DELETE FROM places WHERE bot = $1", bot);
                Pq(c.get(), "DELETE FROM own WHERE bot = $1", bot);
                Pq(c.get(), "DELETE FROM doing WHERE bot = $1", bot);
            }
            std::vector<std::pair<std::string, int>> Summary(const int max) override
            {
                std::vector<std::pair<std::string, int>> summary;
                auto c = PgPool().Take();
                if (!c) return summary;
                PgResult r = Pq(c.get(), "SELECT kind, COUNT(*) FROM places WHERE bot = $1 GROUP BY kind ORDER BY 2 DESC LIMIT $2", bot, max);
                for (int i = 0; i < r.Rows(); ++i) summary.push_back({ r.Text(i, 0), r.Int(i, 1) });
                return summary;
            }
        private:
            std::string bot;
        };

        class PgShared : public Shared
        {
        public:
            void ReportFinds(const std::string& dimension, const Box& box, const std::vector<Place>& finds, const std::string& by) override
            {
                auto c = PgPool().Take();
                if (!c) return;
                std::vector<std::string> kinds;
                std::vector<int> cx, cy, cz, xs, ys, zs;
                std::set<std::tuple<std::string, int, int, int>> cells;
                for (const Place& f : finds)
                {
                    const Position cell = CellOf(f.p);
                    if (!cells.insert({ f.kind, cell.x, cell.y, cell.z }).second) continue;
                    kinds.push_back(f.kind);
                    cx.push_back(cell.x); cy.push_back(cell.y); cz.push_back(cell.z);
                    xs.push_back(f.p.x); ys.push_back(f.p.y); zs.push_back(f.p.z);
                    kind_cache.Add("pg:finds", f.kind);
                }
                const long long now = Now();
                Pq(c.get(), "BEGIN");
                Pq(c.get(), "DELETE FROM finds WHERE (dimension = $1 AND x BETWEEN $2 AND $3 AND y BETWEEN $4 AND $5 AND z BETWEEN $6 AND $7)"
                    " OR (kind LIKE 'mob:%' AND seen < $8)", dimension, box.low.x, box.high.x, box.low.y, box.high.y, box.low.z, box.high.z, now - mob_memory_seconds);
                if (!kinds.empty())
                {
                    Pq(c.get(), "INSERT INTO finds (kind, dimension, cx, cy, cz, x, y, z, seen, seen_by) SELECT k, $1, a, b, d, x, y, z, $2, $3"
                        " FROM unnest($4::text[], $5::int[], $6::int[], $7::int[], $8::int[], $9::int[], $10::int[]) AS t(k, a, b, d, x, y, z)"
                        " ON CONFLICT (kind, dimension, cx, cy, cz) DO UPDATE SET x = EXCLUDED.x, y = EXCLUDED.y, z = EXCLUDED.z, seen = EXCLUDED.seen, seen_by = EXCLUDED.seen_by",
                        dimension, now, by, TextArray(kinds), IntArray(cx), IntArray(cy), IntArray(cz), IntArray(xs), IntArray(ys), IntArray(zs));
                }
                if (!Pq(c.get(), "COMMIT").Ok()) Pq(c.get(), "ROLLBACK");
            }
            std::vector<Place> NearestFinds(const std::string& dimension, const std::string& suffix, const Position& me, const int max) override
            {
                auto c = PgPool().Take();
                if (!c) return {};
                return PgNearest(c.get(), "finds", "", "", "pg:finds", dimension, suffix, me, max);
            }
            void ForgetFind(const Position& p) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "DELETE FROM finds WHERE x = $1 AND y = $2 AND z = $3", p.x, p.y, p.z);
            }
            std::vector<Position> ChestsIn(const std::string& dimension, const Box& box) override
            {
                std::vector<Position> chests;
                auto c = PgPool().Take();
                if (!c) return chests;
                PgResult r = Pq(c.get(), "SELECT DISTINCT x, y, z FROM chest_contents WHERE dimension = $1 AND x BETWEEN $2 AND $3 AND y BETWEEN $4 AND $5 AND z BETWEEN $6 AND $7",
                    dimension, box.low.x, box.high.x, box.low.y, box.high.y, box.low.z, box.high.z);
                for (int i = 0; i < r.Rows(); ++i) chests.push_back(Position(r.Int(i, 0), r.Int(i, 1), r.Int(i, 2)));
                return chests;
            }
            void ForgetChest(const std::string& dimension, const Position& p) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "DELETE FROM chest_contents WHERE dimension = $1 AND x = $2 AND y = $3 AND z = $4", dimension, p.x, p.y, p.z);
            }
            void RememberChest(const std::string& dimension, const Position& p, const std::map<std::string, int>& contents, const std::string& by) override
            {
                auto c = PgPool().Take();
                if (!c) return;
                std::vector<std::string> items = { "" };
                std::vector<int> counts = { 0 };
                for (const auto& [item, count] : contents)
                {
                    if (item.empty()) continue;
                    items.push_back(item);
                    counts.push_back(count);
                }
                Pq(c.get(), "BEGIN");
                Pq(c.get(), "DELETE FROM chest_contents WHERE dimension = $1 AND x = $2 AND y = $3 AND z = $4", dimension, p.x, p.y, p.z);
                Pq(c.get(), "INSERT INTO chest_contents (dimension, x, y, z, item, count, seen, seen_by) SELECT $1, $2, $3, $4, i, n, $5, $6"
                    " FROM unnest($7::text[], $8::int[]) AS t(i, n)", dimension, p.x, p.y, p.z, Now(), by, TextArray(items), IntArray(counts));
                if (!Pq(c.get(), "COMMIT").Ok()) Pq(c.get(), "ROLLBACK");
            }
            std::optional<Position> ChestHolding(const std::string& dimension, const std::string& item, const Position& me) override
            {
                auto c = PgPool().Take();
                if (!c) return std::nullopt;
                PgResult r = Pq(c.get(), "SELECT x, y, z FROM chest_contents WHERE dimension = $1 AND item = $2 AND count > 0"
                    " ORDER BY (x - $3) * (x - $3) + (y - $4) * (y - $4) + (z - $5) * (z - $5) LIMIT 1", dimension, item, me.x, me.y, me.z);
                if (r.Rows() > 0) return Position(r.Int(0, 0), r.Int(0, 1), r.Int(0, 2));
                return std::nullopt;
            }
            std::map<std::string, int> ChestItems(const std::string& dimension, const Position& p) override
            {
                std::map<std::string, int> items;
                auto c = PgPool().Take();
                if (!c) return items;
                PgResult r = Pq(c.get(), "SELECT item, count FROM chest_contents WHERE dimension = $1 AND x = $2 AND y = $3 AND z = $4 AND item <> ''", dimension, p.x, p.y, p.z);
                for (int i = 0; i < r.Rows(); ++i) items[r.Text(i, 0)] = r.Int(i, 1);
                return items;
            }
            std::vector<ChestSeen> Chests(const int max) override
            {
                std::vector<ChestSeen> list;
                auto c = PgPool().Take();
                if (!c) return list;
                PgResult r = Pq(c.get(), "SELECT x, y, z, dimension, MAX(seen), MAX(seen_by) FROM chest_contents GROUP BY dimension, x, y, z ORDER BY MAX(seen) DESC LIMIT $1", max);
                for (int i = 0; i < r.Rows(); ++i) list.push_back({ Position(r.Int(i, 0), r.Int(i, 1), r.Int(i, 2)), r.Text(i, 3), r.Int64(i, 4), r.Text(i, 5), 0 });
                return list;
            }
            std::vector<ChestSeen> ChestsWith(const std::string& item, const int max) override
            {
                std::vector<ChestSeen> list;
                auto c = PgPool().Take();
                if (!c) return list;
                PgResult r = Pq(c.get(), "SELECT x, y, z, dimension, seen, seen_by, count FROM chest_contents WHERE item = $1 AND count > 0 ORDER BY count DESC LIMIT $2", item, max);
                for (int i = 0; i < r.Rows(); ++i) list.push_back({ Position(r.Int(i, 0), r.Int(i, 1), r.Int(i, 2)), r.Text(i, 3), r.Int64(i, 4), r.Text(i, 5), r.Int(i, 6) });
                return list;
            }
            std::map<std::string, int> Stored(const std::string& dimension) override
            {
                std::map<std::string, int> stored;
                auto c = PgPool().Take();
                if (!c) return stored;
                PgResult r = Pq(c.get(), "SELECT item, SUM(count) FROM chest_contents WHERE dimension = $1 AND item <> '' AND count > 0 GROUP BY item", dimension);
                for (int i = 0; i < r.Rows(); ++i) stored[r.Text(i, 0)] = r.Int(i, 1);
                return stored;
            }
            std::vector<Site> Sites() override
            {
                std::vector<Site> sites;
                auto c = PgPool().Take();
                if (!c) return sites;
                PgResult r = Pq(c.get(), "SELECT x1, y1, z1, x2, y2, z2, name FROM sites WHERE name NOT LIKE '%\\_road' ORDER BY id");
                for (int i = 0; i < r.Rows(); ++i) sites.push_back({ Position(r.Int(i, 0), r.Int(i, 1), r.Int(i, 2)), Position(r.Int(i, 3), r.Int(i, 4), r.Int(i, 5)), r.Text(i, 6) });
                return sites;
            }
            void AddSite(const Site& site) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "INSERT INTO sites (x1, y1, z1, x2, y2, z2, name) VALUES ($1, $2, $3, $4, $5, $6, $7)",
                    site.low.x, site.low.y, site.low.z, site.high.x, site.high.y, site.high.z, site.name);
            }
            void RemoveSite(const size_t index) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "DELETE FROM sites WHERE id = (SELECT id FROM sites WHERE name NOT LIKE '%\\_road' ORDER BY id LIMIT 1 OFFSET $1)", index);
            }
            std::map<std::string, Position> ChestFor() override
            {
                std::map<std::string, Position> chests;
                auto c = PgPool().Take();
                if (!c) return chests;
                PgResult r = Pq(c.get(), "SELECT item, x, y, z FROM chests");
                for (int i = 0; i < r.Rows(); ++i) chests[r.Text(i, 0)] = Position(r.Int(i, 1), r.Int(i, 2), r.Int(i, 3));
                return chests;
            }
            void SaveChestFor(const std::map<std::string, Position>& chests) override
            {
                auto c = PgPool().Take();
                if (!c) return;
                std::vector<std::string> items;
                std::vector<int> xs, ys, zs;
                for (const auto& [item, p] : chests) { items.push_back(item); xs.push_back(p.x); ys.push_back(p.y); zs.push_back(p.z); }
                Pq(c.get(), "BEGIN");
                Pq(c.get(), "DELETE FROM chests");
                Pq(c.get(), "INSERT INTO chests (item, x, y, z) SELECT * FROM unnest($1::text[], $2::int[], $3::int[], $4::int[])", TextArray(items), IntArray(xs), IntArray(ys), IntArray(zs));
                if (!Pq(c.get(), "COMMIT").Ok()) Pq(c.get(), "ROLLBACK");
            }
            std::vector<Mine> Mines() override
            {
                std::vector<Mine> mines;
                auto c = PgPool().Take();
                if (!c) return mines;
                PgResult r = Pq(c.get(), "SELECT x, y, z, resources FROM mines ORDER BY id");
                for (int i = 0; i < r.Rows(); ++i) mines.push_back({ Position(r.Int(i, 0), r.Int(i, 1), r.Int(i, 2)), r.Text(i, 3) });
                return mines;
            }
            void AddMine(const Mine& mine) override
            {
                auto c = PgPool().Take();
                if (c) Pq(c.get(), "INSERT INTO mines (x, y, z, resources) VALUES ($1, $2, $3, $4)", mine.entrance.x, mine.entrance.y, mine.entrance.z, mine.resources);
            }
            void SetRole(const std::string& bot, const std::string& role, const bool on) override
            {
                auto c = PgPool().Take();
                if (!c) return;
                if (on) Pq(c.get(), "INSERT INTO roles (bot, role) VALUES ($1, $2) ON CONFLICT DO NOTHING", bot, role);
                else Pq(c.get(), "DELETE FROM roles WHERE bot = $1 AND role = $2", bot, role);
            }
            std::vector<std::string> WithRole(const std::string& role) override
            {
                std::vector<std::string> bots;
                auto c = PgPool().Take();
                if (!c) return bots;
                PgResult r = Pq(c.get(), "SELECT bot FROM roles WHERE role = $1 ORDER BY id", role);
                for (int i = 0; i < r.Rows(); ++i) bots.push_back(r.Text(i, 0));
                return bots;
            }
            std::string RoleStartingWith(const std::string& bot, const std::string& prefix) override
            {
                auto c = PgPool().Take();
                if (!c) return "";
                PgResult r = Pq(c.get(), "SELECT role FROM roles WHERE bot = $1 AND left(role, length($2)) = $2 ORDER BY id LIMIT 1", bot, prefix);
                return r.Rows() > 0 ? r.Text(0, 0) : "";
            }
        };
    }
#endif

    // ------------------------------------------------------------------------------------------------ Valkey

#ifdef BOTCRAFT_WITH_VALKEY
    namespace
    {
        // Keys (all under "bc:"):
        //   bc:m:<bot>:doing                 hash  key -> value
        //   bc:m:<bot>:own                   hash  what -> "x,y,z"
        //   bc:m:<bot>:dims                  set   dimensions it remembers places in
        //   bc:m:<bot>:kinds:<dim>           set   kinds it remembers there
        //   bc:m:<bot>:k:<dim>:<kind>        hash  "x,y,z" -> seen (one per place)
        //   bc:m:<bot>:c:<dim>:<cx,cy,cz>    set   "kind|x,y,z" of its places in that 16-block cell (box scans, forget)
        //   bc:f:...                         the same for the shared finds; bc:f:k:<dim>:<kind> holds "cx,cy,cz" -> "x,y,z,seen,by"
        //   bc:ch:<dim>:<x,y,z>              hash  item -> count ("" the chest itself)
        //   bc:chm:<dim>:<x,y,z>             hash  seen, by
        //   bc:ch:all:<dim>, bc:ch:dims      sets  of chests "x,y,z", of dimensions
        //   bc:ch:item:<item>                set   "dim|x,y,z" of chests holding it
        //   bc:sites, bc:mines               lists "x1,y1,z1,x2,y2,z2|name", "x,y,z|resources"
        //   bc:chestfor                      hash  item -> "x,y,z"
        //   bc:role:<role>                   zset  bot, by the order it got it (bc:roleseq); bc:roles:<bot> set of its roles
        using Reply = std::unique_ptr<redisReply, decltype(&freeReplyObject)>;

        redisContext* ValkeyOpen()
        {
            // valkey://[:password@]host[:port][/db]
            std::string rest = Url().substr(Url().find("://") + 3);
            std::string password;
            if (const size_t at = rest.find('@'); at != std::string::npos)
            {
                password = rest.substr(0, at);
                if (!password.empty() && password[0] == ':') password = password.substr(1);
                rest = rest.substr(at + 1);
            }
            int db = 0;
            if (const size_t slash = rest.find('/'); slash != std::string::npos)
            {
                db = std::atoi(rest.substr(slash + 1).c_str());
                rest = rest.substr(0, slash);
            }
            int port = 6379;
            if (const size_t colon = rest.rfind(':'); colon != std::string::npos)
            {
                port = std::atoi(rest.substr(colon + 1).c_str());
                rest = rest.substr(0, colon);
            }
            const timeval timeout{ 5, 0 };
            redisContext* c = redisConnectWithTimeout(rest.empty() ? "127.0.0.1" : rest.c_str(), port, timeout);
            if (c == nullptr || c->err)
            {
                Fail(std::string("valkey: can't connect: ") + (c ? c->errstr : "out of memory"));
                redisFree(c);
                return nullptr;
            }
            if (!password.empty()) freeReplyObject(redisCommand(c, "AUTH %s", password.c_str()));
            if (db != 0) freeReplyObject(redisCommand(c, "SELECT %d", db));
            return c;
        }

        Pool<redisContext>& ValkeyPool()
        {
            static Pool<redisContext> pool(ValkeyOpen, redisFree, [](redisContext* c) { return c->err == 0; });
            return pool;
        }

        /// @brief Commands sent together, their replies read in order (one round trip for all of them)
        class Batch
        {
        public:
            explicit Batch(redisContext* c_) : c(c_) {}
            Batch& Add(std::vector<std::string> args) { commands.push_back(std::move(args)); return *this; }
            std::vector<Reply> Send()
            {
                std::vector<Reply> replies;
                for (const auto& args : commands)
                {
                    std::vector<const char*> argv;
                    std::vector<size_t> lengths;
                    for (const std::string& a : args) { argv.push_back(a.data()); lengths.push_back(a.size()); }
                    redisAppendCommandArgv(c, static_cast<int>(argv.size()), argv.data(), lengths.data());
                }
                for (size_t i = 0; i < commands.size(); ++i)
                {
                    void* reply = nullptr;
                    if (redisGetReply(c, &reply) != REDIS_OK)
                    {
                        Fail(std::string("valkey: ") + c->errstr);
                        replies.emplace_back(nullptr, freeReplyObject);
                        continue;
                    }
                    replies.emplace_back(static_cast<redisReply*>(reply), freeReplyObject);
                }
                commands.clear();
                return replies;
            }
            bool Empty() const { return commands.empty(); }
        private:
            redisContext* c;
            std::vector<std::vector<std::string>> commands;
        };

        Reply Command(redisContext* c, std::vector<std::string> args)
        {
            auto replies = Batch(c).Add(std::move(args)).Send();
            return std::move(replies.front());
        }

        std::vector<std::string> Strings(const Reply& r)
        {
            std::vector<std::string> out;
            if (r && r->type == REDIS_REPLY_ARRAY)
            {
                for (size_t i = 0; i < r->elements; ++i) if (r->element[i]->str != nullptr) out.emplace_back(r->element[i]->str, r->element[i]->len);
            }
            return out;
        }
        std::string String(const Reply& r) { return r && r->str != nullptr ? std::string(r->str, r->len) : ""; }
        long long Integer(const Reply& r) { return r && r->type == REDIS_REPLY_INTEGER ? r->integer : (r && r->str ? std::atoll(r->str) : 0); }
        /// @brief A hash's fields and values (HGETALL)
        std::vector<std::pair<std::string, std::string>> Pairs(const Reply& r)
        {
            std::vector<std::pair<std::string, std::string>> out;
            const std::vector<std::string> flat = Strings(r);
            for (size_t i = 0; i + 1 < flat.size(); i += 2) out.push_back({ flat[i], flat[i + 1] });
            return out;
        }

        std::vector<Position> CellsOf(const Box& box)
        {
            std::vector<Position> cells;
            for (int x = box.low.x >> 4; x <= box.high.x >> 4; ++x)
                for (int y = box.low.y >> 4; y <= box.high.y >> 4; ++y)
                    for (int z = box.low.z >> 4; z <= box.high.z >> 4; ++z) cells.push_back(Position(x, y, z));
            return cells;
        }

        /// @brief Places of kind-hashes "<prefix>k:<dim>:<kind>" (fields "x,y,z"), kinds ending in suffix, nearest first
        std::vector<Place> ValkeyNearest(redisContext* c, const std::string& prefix, const std::string& dimension, const std::string& suffix,
            const Position& me, const int max, const bool fields_are_cells)
        {
            std::vector<std::string> kinds;
            for (const std::string& kind : Strings(Command(c, { "SMEMBERS", prefix + "kinds:" + dimension }))) if (EndsWith(kind, suffix)) kinds.push_back(kind);
            Batch batch(c);
            for (const std::string& kind : kinds) batch.Add({ fields_are_cells ? "HVALS" : "HKEYS", prefix + "k:" + dimension + ":" + kind });
            std::vector<Place> places;
            const auto replies = batch.Send();
            for (size_t i = 0; i < kinds.size(); ++i)
            {
                std::vector<Place> some;
                for (const std::string& s : Strings(replies[i]))
                {
                    // Finds hold "x,y,z,seen,by": the place is the first three numbers
                    if (const auto p = ParsePosition(fields_are_cells ? s.substr(0, s.find(',', s.find(',', s.find(',') + 1) + 1)) : s)) some.push_back({ kinds[i], *p });
                }
                MergeNearest(places, std::move(some), me, max);
            }
            return places;
        }

        class ValkeyMemory : public Memory
        {
        public:
            explicit ValkeyMemory(const std::string& bot) : prefix("bc:m:" + bot + ":") {}
            void SaveDoing(const std::string& key, const std::string& value) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                if (value.empty()) Command(c.get(), { "HDEL", prefix + "doing", key });
                else Command(c.get(), { "HSET", prefix + "doing", key, value });
            }
            std::map<std::string, std::string> LoadDoing() override
            {
                std::map<std::string, std::string> doing;
                auto c = ValkeyPool().Take();
                if (!c) return doing;
                for (const auto& [k, v] : Pairs(Command(c.get(), { "HGETALL", prefix + "doing" }))) doing[k] = v;
                return doing;
            }
            void SetOwn(const std::string& what, const Position& p) override
            {
                auto c = ValkeyPool().Take();
                if (c) Command(c.get(), { "HSET", prefix + "own", what, Key(p) });
            }
            void ForgetOwn(const std::string& what) override
            {
                auto c = ValkeyPool().Take();
                if (c) Command(c.get(), { "HDEL", prefix + "own", what });
            }
            std::optional<Position> GetOwn(const std::string& what) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return std::nullopt;
                return ParsePosition(String(Command(c.get(), { "HGET", prefix + "own", what })));
            }
            void RememberPlace(const std::string& dimension, const std::string& kind, const Position& p) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                // One place per position: another kind remembered there goes
                const std::string cell = prefix + "c:" + dimension + ":" + Key(CellOf(p));
                Batch write(c.get());
                for (const std::string& m : Strings(Command(c.get(), { "SMEMBERS", cell })))
                {
                    const size_t bar = m.find('|');
                    if (bar == std::string::npos || m.substr(bar + 1) != Key(p)) continue;
                    write.Add({ "HDEL", prefix + "k:" + dimension + ":" + m.substr(0, bar), Key(p) });
                    write.Add({ "SREM", cell, m });
                }
                write.Add({ "HSET", prefix + "k:" + dimension + ":" + kind, Key(p), std::to_string(Now()) });
                write.Add({ "SADD", cell, kind + "|" + Key(p) });
                write.Add({ "SADD", prefix + "kinds:" + dimension, kind });
                write.Add({ "SADD", prefix + "dims", dimension });
                write.Send();
            }
            bool KnownInCell(const std::string& dimension, const std::string& kind, const Position& o) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return false;
                for (const std::string& m : Strings(Command(c.get(), { "SMEMBERS", prefix + "c:" + dimension + ":" + Key(CellOf(o)) })))
                {
                    if (m.compare(0, kind.size() + 1, kind + "|") == 0) return true;
                }
                return false;
            }
            void ReplaceBox(const std::string& dimension, const Box& box, const std::vector<Place>& places) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                const long long now = Now();
                // What was remembered in the box goes
                const std::vector<Position> cells = CellsOf(box);
                Batch read(c.get());
                for (const Position& cell : cells) read.Add({ "SMEMBERS", prefix + "c:" + dimension + ":" + Key(cell) });
                const auto members = read.Send();
                Batch write(c.get());
                for (size_t i = 0; i < cells.size(); ++i)
                {
                    for (const std::string& m : Strings(members[i]))
                    {
                        const size_t bar = m.find('|');
                        const auto p = ParsePosition(m.substr(bar + 1));
                        if (bar == std::string::npos || !p || !box.Contains(*p)) continue;
                        write.Add({ "HDEL", prefix + "k:" + dimension + ":" + m.substr(0, bar), Key(*p) });
                        write.Add({ "SREM", prefix + "c:" + dimension + ":" + Key(cells[i]), m });
                    }
                }
                // What the scan saw, seen now
                write.Add({ "SADD", prefix + "dims", dimension });
                std::set<std::string> kinds;
                for (const Place& place : places)
                {
                    write.Add({ "HSET", prefix + "k:" + dimension + ":" + place.kind, Key(place.p), std::to_string(now) });
                    write.Add({ "SADD", prefix + "c:" + dimension + ":" + Key(CellOf(place.p)), place.kind + "|" + Key(place.p) });
                    kinds.insert(place.kind);
                }
                for (const std::string& kind : kinds) write.Add({ "SADD", prefix + "kinds:" + dimension, kind });
                write.Send();
                ForgetOldMobs(c.get(), now);
            }
            std::vector<Place> Nearest(const std::string& dimension, const std::string& suffix, const Position& me, const int max) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return {};
                return ValkeyNearest(c.get(), prefix, dimension, suffix, me, max, false);
            }
            void Forget(const Position& p) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                Batch write(c.get());
                for (const std::string& dimension : Strings(Command(c.get(), { "SMEMBERS", prefix + "dims" })))
                {
                    const std::string cell = prefix + "c:" + dimension + ":" + Key(CellOf(p));
                    for (const std::string& m : Strings(Command(c.get(), { "SMEMBERS", cell })))
                    {
                        const size_t bar = m.find('|');
                        if (bar == std::string::npos || m.substr(bar + 1) != Key(p)) continue;
                        write.Add({ "HDEL", prefix + "k:" + dimension + ":" + m.substr(0, bar), Key(p) });
                        write.Add({ "SREM", cell, m });
                    }
                }
                if (!write.Empty()) write.Send();
            }
            std::vector<std::pair<std::string, int>> Summary(const int max) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return {};
                std::map<std::string, int> counts;
                for (const std::string& dimension : Strings(Command(c.get(), { "SMEMBERS", prefix + "dims" })))
                {
                    const std::vector<std::string> kinds = Strings(Command(c.get(), { "SMEMBERS", prefix + "kinds:" + dimension }));
                    Batch batch(c.get());
                    for (const std::string& kind : kinds) batch.Add({ "HLEN", prefix + "k:" + dimension + ":" + kind });
                    const auto replies = batch.Send();
                    for (size_t i = 0; i < kinds.size(); ++i) counts[kinds[i]] += static_cast<int>(Integer(replies[i]));
                }
                std::vector<std::pair<std::string, int>> summary;
                for (const auto& [kind, n] : counts) if (n > 0) summary.push_back({ kind, n });
                std::sort(summary.begin(), summary.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
                if (summary.size() > static_cast<size_t>(std::max(0, max))) summary.resize(static_cast<size_t>(std::max(0, max)));
                return summary;
            }
            void Clear() override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                // Every key under the bot's prefix, found with SCAN (KEYS would stall the server)
                std::string cursor = "0";
                do
                {
                    const Reply r = Command(c.get(), { "SCAN", cursor, "MATCH", prefix + "*", "COUNT", "1000" });
                    if (!r || r->type != REDIS_REPLY_ARRAY || r->elements < 2) break;
                    cursor = std::string(r->element[0]->str, r->element[0]->len);
                    std::vector<std::string> del = { "UNLINK" };
                    for (size_t i = 0; i < r->element[1]->elements; ++i) del.emplace_back(r->element[1]->element[i]->str, r->element[1]->element[i]->len);
                    if (del.size() > 1) Command(c.get(), del);
                } while (cursor != "0");
            }
        private:
            void ForgetOldMobs(redisContext* c, const long long now)
            {
                for (const std::string& dimension : Strings(Command(c, { "SMEMBERS", prefix + "dims" })))
                {
                    Batch write(c);
                    for (const std::string& kind : Strings(Command(c, { "SMEMBERS", prefix + "kinds:" + dimension })))
                    {
                        if (kind.rfind("mob:", 0) != 0) continue;
                        for (const auto& [at, seen] : Pairs(Command(c, { "HGETALL", prefix + "k:" + dimension + ":" + kind })))
                        {
                            const auto p = ParsePosition(at);
                            if (!p || std::atoll(seen.c_str()) >= now - mob_memory_seconds) continue;
                            write.Add({ "HDEL", prefix + "k:" + dimension + ":" + kind, at });
                            write.Add({ "SREM", prefix + "c:" + dimension + ":" + Key(CellOf(*p)), kind + "|" + at });
                        }
                    }
                    if (!write.Empty()) write.Send();
                }
            }
            std::string prefix;
        };

        class ValkeyShared : public Shared
        {
        public:
            void ReportFinds(const std::string& dimension, const Box& box, const std::vector<Place>& finds, const std::string& by) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                const long long now = Now();
                const std::string f = "bc:f:";
                // Earlier finds in the box go
                const std::vector<Position> cells = CellsOf(box);
                Batch read(c.get());
                for (const Position& cell : cells) read.Add({ "SMEMBERS", f + "c:" + dimension + ":" + Key(cell) });
                const auto kinds_in = read.Send();
                Batch values(c.get());
                std::vector<std::pair<std::string, Position>> asked;
                for (size_t i = 0; i < cells.size(); ++i)
                {
                    for (const std::string& kind : Strings(kinds_in[i]))
                    {
                        values.Add({ "HGET", f + "k:" + dimension + ":" + kind, Key(cells[i]) });
                        asked.push_back({ kind, cells[i] });
                    }
                }
                Batch write(c.get());
                if (!values.Empty())
                {
                    const auto got = values.Send();
                    for (size_t i = 0; i < asked.size(); ++i)
                    {
                        const std::string v = String(got[i]);
                        const auto p = ParsePosition(v.substr(0, v.find(',', v.find(',', v.find(',') + 1) + 1)));
                        if (v.empty() || (p && box.Contains(*p)))
                        {
                            write.Add({ "HDEL", f + "k:" + dimension + ":" + asked[i].first, Key(asked[i].second) });
                            write.Add({ "SREM", f + "c:" + dimension + ":" + Key(asked[i].second), asked[i].first });
                        }
                    }
                }
                // This scan's, the first of each kind in a cell
                write.Add({ "SADD", f + "dims", dimension });
                std::set<std::tuple<std::string, int, int, int>> seen_cells;
                std::set<std::string> kinds;
                for (const Place& find : finds)
                {
                    const Position cell = CellOf(find.p);
                    if (!seen_cells.insert({ find.kind, cell.x, cell.y, cell.z }).second) continue;
                    write.Add({ "HSET", f + "k:" + dimension + ":" + find.kind, Key(cell), Key(find.p) + "," + std::to_string(now) + "," + by });
                    write.Add({ "SADD", f + "c:" + dimension + ":" + Key(cell), find.kind });
                    kinds.insert(find.kind);
                }
                for (const std::string& kind : kinds) write.Add({ "SADD", f + "kinds:" + dimension, kind });
                write.Send();
                // Mob sightings older than half an hour, anywhere
                for (const std::string& dim : Strings(Command(c.get(), { "SMEMBERS", f + "dims" })))
                {
                    Batch old(c.get());
                    for (const std::string& kind : Strings(Command(c.get(), { "SMEMBERS", f + "kinds:" + dim })))
                    {
                        if (kind.rfind("mob:", 0) != 0) continue;
                        for (const auto& [cell, v] : Pairs(Command(c.get(), { "HGETALL", f + "k:" + dim + ":" + kind })))
                        {
                            // "x,y,z,seen,by"
                            std::istringstream in(v);
                            std::string part;
                            std::vector<std::string> parts;
                            while (std::getline(in, part, ',')) parts.push_back(part);
                            if (parts.size() < 4 || std::atoll(parts[3].c_str()) >= now - mob_memory_seconds) continue;
                            old.Add({ "HDEL", f + "k:" + dim + ":" + kind, cell });
                            old.Add({ "SREM", f + "c:" + dim + ":" + cell, kind });
                        }
                    }
                    if (!old.Empty()) old.Send();
                }
            }
            std::vector<Place> NearestFinds(const std::string& dimension, const std::string& suffix, const Position& me, const int max) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return {};
                return ValkeyNearest(c.get(), "bc:f:", dimension, suffix, me, max, true);
            }
            void ForgetFind(const Position& p) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                const Position cell = CellOf(p);
                Batch write(c.get());
                for (const std::string& dimension : Strings(Command(c.get(), { "SMEMBERS", "bc:f:dims" })))
                {
                    for (const std::string& kind : Strings(Command(c.get(), { "SMEMBERS", "bc:f:c:" + dimension + ":" + Key(cell) })))
                    {
                        const std::string v = String(Command(c.get(), { "HGET", "bc:f:k:" + dimension + ":" + kind, Key(cell) }));
                        if (v.rfind(Key(p) + ",", 0) != 0) continue;
                        write.Add({ "HDEL", "bc:f:k:" + dimension + ":" + kind, Key(cell) });
                        write.Add({ "SREM", "bc:f:c:" + dimension + ":" + Key(cell), kind });
                    }
                }
                if (!write.Empty()) write.Send();
            }
            std::vector<Position> ChestsIn(const std::string& dimension, const Box& box) override
            {
                std::vector<Position> chests;
                auto c = ValkeyPool().Take();
                if (!c) return chests;
                for (const std::string& s : Strings(Command(c.get(), { "SMEMBERS", "bc:ch:all:" + dimension })))
                {
                    if (const auto p = ParsePosition(s); p && box.Contains(*p)) chests.push_back(*p);
                }
                return chests;
            }
            void ForgetChest(const std::string& dimension, const Position& p) override
            {
                auto c = ValkeyPool().Take();
                if (c) Forget(c.get(), dimension, p);
            }
            void RememberChest(const std::string& dimension, const Position& p, const std::map<std::string, int>& contents, const std::string& by) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                Forget(c.get(), dimension, p);
                const std::string at = Key(p);
                Batch write(c.get());
                write.Add({ "HSET", "bc:ch:" + dimension + ":" + at, "", "0" });
                for (const auto& [item, count] : contents)
                {
                    if (item.empty()) continue;
                    write.Add({ "HSET", "bc:ch:" + dimension + ":" + at, item, std::to_string(count) });
                    if (count > 0) write.Add({ "SADD", "bc:ch:item:" + item, dimension + "|" + at });
                }
                write.Add({ "HSET", "bc:chm:" + dimension + ":" + at, "seen", std::to_string(Now()), "by", by });
                write.Add({ "SADD", "bc:ch:all:" + dimension, at });
                write.Add({ "SADD", "bc:ch:dims", dimension });
                write.Send();
            }
            std::optional<Position> ChestHolding(const std::string& dimension, const std::string& item, const Position& me) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return std::nullopt;
                std::optional<Position> nearest;
                for (const std::string& s : Strings(Command(c.get(), { "SMEMBERS", "bc:ch:item:" + item })))
                {
                    if (s.rfind(dimension + "|", 0) != 0) continue;
                    const auto p = ParsePosition(s.substr(dimension.size() + 1));
                    if (p && (!nearest || Distance(*p, me) < Distance(*nearest, me))) nearest = p;
                }
                return nearest;
            }
            std::map<std::string, int> ChestItems(const std::string& dimension, const Position& p) override
            {
                std::map<std::string, int> items;
                auto c = ValkeyPool().Take();
                if (!c) return items;
                for (const auto& [item, count] : Pairs(Command(c.get(), { "HGETALL", "bc:ch:" + dimension + ":" + Key(p) })))
                {
                    if (!item.empty()) items[item] = std::atoi(count.c_str());
                }
                return items;
            }
            std::vector<ChestSeen> Chests(const int max) override
            {
                std::vector<ChestSeen> list;
                auto c = ValkeyPool().Take();
                if (!c) return list;
                for (const std::string& dimension : Strings(Command(c.get(), { "SMEMBERS", "bc:ch:dims" })))
                {
                    const std::vector<std::string> all = Strings(Command(c.get(), { "SMEMBERS", "bc:ch:all:" + dimension }));
                    Batch batch(c.get());
                    for (const std::string& at : all) batch.Add({ "HMGET", "bc:chm:" + dimension + ":" + at, "seen", "by" });
                    const auto replies = batch.Send();
                    for (size_t i = 0; i < all.size(); ++i)
                    {
                        const std::vector<std::string> meta = Strings(replies[i]);
                        if (const auto p = ParsePosition(all[i])) list.push_back({ *p, dimension, meta.size() > 0 ? std::atoll(meta[0].c_str()) : 0, meta.size() > 1 ? meta[1] : "", 0 });
                    }
                }
                std::sort(list.begin(), list.end(), [](const ChestSeen& a, const ChestSeen& b) { return a.seen > b.seen; });
                if (list.size() > static_cast<size_t>(std::max(0, max))) list.resize(static_cast<size_t>(std::max(0, max)));
                return list;
            }
            std::vector<ChestSeen> ChestsWith(const std::string& item, const int max) override
            {
                std::vector<ChestSeen> list;
                auto c = ValkeyPool().Take();
                if (!c) return list;
                const std::vector<std::string> holding = Strings(Command(c.get(), { "SMEMBERS", "bc:ch:item:" + item }));
                Batch batch(c.get());
                for (const std::string& s : holding)
                {
                    const std::string dimension = s.substr(0, s.find('|'));
                    const std::string at = s.substr(s.find('|') + 1);
                    batch.Add({ "HGET", "bc:ch:" + dimension + ":" + at, item });
                    batch.Add({ "HMGET", "bc:chm:" + dimension + ":" + at, "seen", "by" });
                }
                const auto replies = batch.Send();
                for (size_t i = 0; i < holding.size(); ++i)
                {
                    const std::string& s = holding[i];
                    const int count = std::atoi(String(replies[2 * i]).c_str());
                    const std::vector<std::string> meta = Strings(replies[2 * i + 1]);
                    const auto p = ParsePosition(s.substr(s.find('|') + 1));
                    if (p && count > 0) list.push_back({ *p, s.substr(0, s.find('|')), meta.size() > 0 ? std::atoll(meta[0].c_str()) : 0, meta.size() > 1 ? meta[1] : "", count });
                }
                std::sort(list.begin(), list.end(), [](const ChestSeen& a, const ChestSeen& b) { return a.count > b.count; });
                if (list.size() > static_cast<size_t>(std::max(0, max))) list.resize(static_cast<size_t>(std::max(0, max)));
                return list;
            }
            std::map<std::string, int> Stored(const std::string& dimension) override
            {
                std::map<std::string, int> stored;
                auto c = ValkeyPool().Take();
                if (!c) return stored;
                const std::vector<std::string> all = Strings(Command(c.get(), { "SMEMBERS", "bc:ch:all:" + dimension }));
                Batch batch(c.get());
                for (const std::string& at : all) batch.Add({ "HGETALL", "bc:ch:" + dimension + ":" + at });
                for (const Reply& r : batch.Send())
                {
                    for (const auto& [item, count] : Pairs(r)) if (!item.empty() && std::atoi(count.c_str()) > 0) stored[item] += std::atoi(count.c_str());
                }
                return stored;
            }
            std::vector<Site> Sites() override
            {
                std::vector<Site> sites;
                auto c = ValkeyPool().Take();
                if (!c) return sites;
                for (const std::string& s : Strings(Command(c.get(), { "LRANGE", "bc:sites", "0", "-1" })))
                {
                    if (const auto site = ParseSite(s); site && !EndsWith(site->name, "_road")) sites.push_back(*site);
                }
                return sites;
            }
            void AddSite(const Site& site) override
            {
                auto c = ValkeyPool().Take();
                if (c) Command(c.get(), { "RPUSH", "bc:sites", Key(site.low) + "," + Key(site.high) + "|" + site.name });
            }
            void RemoveSite(const size_t index) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                size_t kept = 0;
                const std::vector<std::string> all = Strings(Command(c.get(), { "LRANGE", "bc:sites", "0", "-1" }));
                for (size_t i = 0; i < all.size(); ++i)
                {
                    const auto site = ParseSite(all[i]);
                    if (!site || EndsWith(site->name, "_road")) continue;
                    if (kept++ == index)
                    {
                        // Marked, then removed: LREM by value would take the first equal one
                        const std::string mark = "removed " + std::to_string(Now()) + " " + std::to_string(i);
                        Command(c.get(), { "LSET", "bc:sites", std::to_string(i), mark });
                        Command(c.get(), { "LREM", "bc:sites", "1", mark });
                        return;
                    }
                }
            }
            std::map<std::string, Position> ChestFor() override
            {
                std::map<std::string, Position> chests;
                auto c = ValkeyPool().Take();
                if (!c) return chests;
                for (const auto& [item, at] : Pairs(Command(c.get(), { "HGETALL", "bc:chestfor" }))) if (const auto p = ParsePosition(at)) chests[item] = *p;
                return chests;
            }
            void SaveChestFor(const std::map<std::string, Position>& chests) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                Batch write(c.get());
                write.Add({ "MULTI" }).Add({ "DEL", "bc:chestfor" });
                for (const auto& [item, p] : chests) write.Add({ "HSET", "bc:chestfor", item, Key(p) });
                write.Add({ "EXEC" });
                write.Send();
            }
            std::vector<Mine> Mines() override
            {
                std::vector<Mine> mines;
                auto c = ValkeyPool().Take();
                if (!c) return mines;
                for (const std::string& s : Strings(Command(c.get(), { "LRANGE", "bc:mines", "0", "-1" })))
                {
                    const size_t bar = s.find('|');
                    if (const auto p = ParsePosition(s.substr(0, bar)); p && bar != std::string::npos) mines.push_back({ *p, s.substr(bar + 1) });
                }
                return mines;
            }
            void AddMine(const Mine& mine) override
            {
                auto c = ValkeyPool().Take();
                if (c) Command(c.get(), { "RPUSH", "bc:mines", Key(mine.entrance) + "|" + mine.resources });
            }
            void SetRole(const std::string& bot, const std::string& role, const bool on) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return;
                if (on)
                {
                    const long long order = Integer(Command(c.get(), { "INCR", "bc:roleseq" }));
                    Batch(c.get()).Add({ "ZADD", "bc:role:" + role, "NX", std::to_string(order), bot }).Add({ "SADD", "bc:roles:" + bot, role }).Send();
                }
                else
                {
                    Batch(c.get()).Add({ "ZREM", "bc:role:" + role, bot }).Add({ "SREM", "bc:roles:" + bot, role }).Send();
                }
            }
            std::vector<std::string> WithRole(const std::string& role) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return {};
                return Strings(Command(c.get(), { "ZRANGE", "bc:role:" + role, "0", "-1" }));
            }
            std::string RoleStartingWith(const std::string& bot, const std::string& prefix) override
            {
                auto c = ValkeyPool().Take();
                if (!c) return "";
                for (const std::string& role : Strings(Command(c.get(), { "SMEMBERS", "bc:roles:" + bot }))) if (role.rfind(prefix, 0) == 0) return role;
                return "";
            }
        private:
            static std::optional<Site> ParseSite(const std::string& s)
            {
                const size_t bar = s.find('|');
                if (bar == std::string::npos) return std::nullopt;
                std::istringstream in(s.substr(0, bar));
                Site site;
                char c[5] = {};
                if (!(in >> site.low.x >> c[0] >> site.low.y >> c[1] >> site.low.z >> c[2] >> site.high.x >> c[3] >> site.high.y >> c[4] >> site.high.z)) return std::nullopt;
                site.name = s.substr(bar + 1);
                return site;
            }
            static void Forget(redisContext* c, const std::string& dimension, const Position& p)
            {
                const std::string at = Key(p);
                Batch write(c);
                for (const std::string& item : Strings(Command(c, { "HKEYS", "bc:ch:" + dimension + ":" + at }))) if (!item.empty()) write.Add({ "SREM", "bc:ch:item:" + item, dimension + "|" + at });
                write.Add({ "DEL", "bc:ch:" + dimension + ":" + at, "bc:chm:" + dimension + ":" + at });
                write.Add({ "SREM", "bc:ch:all:" + dimension, at });
                write.Send();
            }
        };
    }
#endif

    // ------------------------------------------------------------------------------------------------ choosing

    std::unique_ptr<Memory> OpenMemory(const std::string& bot)
    {
        const std::string backend = Backend();
#ifdef BOTCRAFT_WITH_POSTGRES
        if (backend == "postgresql") return PgPool().Take() ? std::make_unique<PgMemory>(bot) : nullptr;
#endif
#ifdef BOTCRAFT_WITH_VALKEY
        if (backend == "valkey") return ValkeyPool().Take() ? std::make_unique<ValkeyMemory>(bot) : nullptr;
#endif
        if (backend != "sqlite")
        {
            Fail("BOTCRAFT_DB is " + backend + ", but this program was built without it");
            return nullptr;
        }
        return OpenSqliteMemory(bot);
    }

    Shared* SharedStore()
    {
        const std::string backend = Backend();
#ifdef BOTCRAFT_WITH_POSTGRES
        if (backend == "postgresql") { static PgShared shared; return &shared; }
#endif
#ifdef BOTCRAFT_WITH_VALKEY
        if (backend == "valkey") { static ValkeyShared shared; return &shared; }
#endif
        if (backend != "sqlite")
        {
            Fail("BOTCRAFT_DB is " + backend + ", but this program was built without it");
            return nullptr;
        }
        struct Connection
        {
            std::unique_ptr<SqliteShared> shared{ OpenSqliteShared() };
        };
        thread_local Connection connection;
        return connection.shared.get();
    }
}
