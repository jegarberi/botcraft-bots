#pragma once
// Where the bots keep what they know: each bot's memory (the places it saw, its own bed, what it was doing) and
// what they share (finds, chests and what is in them, building sites, mines, roles). BOTCRAFT_DB picks the store:
//   unset or sqlite              SQLite files: memory/<bot>.sqlite and botcraft.sqlite, next to bin/ (the default)
//   postgresql://user@host/db    PostgreSQL: one table set, the bots' memories keyed by bot name (libpq)
//   valkey://host:port[/db]      Valkey (or Redis): hashes and sets under the key prefix "bc:" (hiredis)
// Every operation is safe from any thread. All of them take positions as block coordinates.
#include "botcraft/Game/Vector3.hpp"

#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace store
{
    using Position = Botcraft::Vector3<int>;

    struct Place { std::string kind; Position p; };
    /// @brief An axis-aligned box, both corners included
    struct Box { Position low, high; bool Contains(const Position& p) const; };
    struct Site { Position low, high; std::string name; };
    struct Mine { Position entrance; std::string resources; };
    struct ChestSeen { Position p; std::string dimension; long long seen = 0; std::string by; int count = 0; };

    /// @brief One bot's memory
    class Memory
    {
    public:
        virtual ~Memory() = default;
        /// @brief What the bot is doing, by key, for the next time it joins; an empty value forgets the key
        virtual void SaveDoing(const std::string& key, const std::string& value) = 0;
        virtual std::map<std::string, std::string> LoadDoing() = 0;
        /// @brief Single places that are the bot's own (its bed, its shelter, its field)
        virtual void SetOwn(const std::string& what, const Position& p) = 0;
        virtual void ForgetOwn(const std::string& what) = 0;
        virtual std::optional<Position> GetOwn(const std::string& what) = 0;
        /// @brief One place, seen now (told by a teammate): what was remembered there before is replaced
        virtual void RememberPlace(const std::string& dimension, const std::string& kind, const Position& p) = 0;
        /// @brief A place of this kind remembered in the 16-block cell whose lowest corner is origin
        virtual bool KnownInCell(const std::string& dimension, const std::string& kind, const Position& origin) = 0;
        /// @brief A scan of box: what was remembered in it is replaced by places (seen now), and sightings of mobs
        /// (kinds "mob:...") older than half an hour anywhere are forgotten
        virtual void ReplaceBox(const std::string& dimension, const Box& box, const std::vector<Place>& places) = 0;
        /// @brief Remembered places whose kind ends in suffix, nearest to me first
        virtual std::vector<Place> Nearest(const std::string& dimension, const std::string& suffix, const Position& me, int max) = 0;
        /// @brief Whatever is remembered at p (any kind, any dimension)
        virtual void Forget(const Position& p) = 0;
        /// @brief How many places of each kind, most numerous first
        virtual std::vector<std::pair<std::string, int>> Summary(int max) = 0;
        /// @brief Writes between Begin and Commit go together (SQLite: one transaction)
        virtual void Begin() {}
        virtual void Commit() {}
    };

    /// @brief What all the bots share
    class Shared
    {
    public:
        virtual ~Shared() = default;
        // Finds: one place per kind and 16-block cell, reported by the bots for each other
        /// @brief A bot's scan of box: earlier finds in it are taken back (and mob sightings older than half an hour
        /// anywhere), these are written (the first of each kind in a cell)
        virtual void ReportFinds(const std::string& dimension, const Box& box, const std::vector<Place>& finds, const std::string& by) = 0;
        virtual std::vector<Place> NearestFinds(const std::string& dimension, const std::string& suffix, const Position& me, int max) = 0;
        virtual void ForgetFind(const Position& p) = 0;
        // What is in each chest, as the last bot that opened it saw it
        virtual std::vector<Position> ChestsIn(const std::string& dimension, const Box& box) = 0;
        virtual void ForgetChest(const std::string& dimension, const Position& p) = 0;
        /// @brief contents: item id -> count (may be empty: the chest is known, empty)
        virtual void RememberChest(const std::string& dimension, const Position& p, const std::map<std::string, int>& contents, const std::string& by) = 0;
        virtual std::optional<Position> ChestHolding(const std::string& dimension, const std::string& item, const Position& me) = 0;
        virtual std::map<std::string, int> ChestItems(const std::string& dimension, const Position& p) = 0;
        /// @brief Every known chest, the most recently seen first
        virtual std::vector<ChestSeen> Chests(int max) = 0;
        /// @brief The chests holding item, fullest first (count: how many it holds)
        virtual std::vector<ChestSeen> ChestsWith(const std::string& item, int max) = 0;
        /// @brief Items in all the chests of a dimension, summed
        virtual std::map<std::string, int> Stored(const std::string& dimension) = 0;
        // Building sites, in the order they were added (sites named *_road left out)
        virtual std::vector<Site> Sites() = 0;
        virtual void AddSite(const Site& site) = 0;
        /// @brief index: into Sites()
        virtual void RemoveSite(size_t index) = 0;
        // The chest for each item (--chests)
        virtual std::map<std::string, Position> ChestFor() = 0;
        virtual void SaveChestFor(const std::map<std::string, Position>& chests) = 0;
        // Mines, in the order they were added
        virtual std::vector<Mine> Mines() = 0;
        virtual void AddMine(const Mine& mine) = 0;
        // Roles the program gives the bots (feeders, trade:...), kept across restarts
        virtual void SetRole(const std::string& bot, const std::string& role, bool on) = 0;
        /// @brief The bots with a role, in the order they got it
        virtual std::vector<std::string> WithRole(const std::string& role) = 0;
        /// @brief A role of the bot starting with prefix ("" if none)
        virtual std::string RoleStartingWith(const std::string& bot, const std::string& prefix) = 0;
    };

    /// @brief The store BOTCRAFT_DB names ("sqlite", "postgresql" or "valkey"), and the error that kept it from
    /// opening ("" when it opened)
    std::string Backend();
    std::string Error();
    /// @brief A bot's memory, nullptr if the store can't be opened
    std::unique_ptr<Memory> OpenMemory(const std::string& bot);
    /// @brief The shared store (for SQLite, this thread's connection), nullptr if it can't be opened
    Shared* SharedStore();
}
