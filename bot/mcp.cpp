// MCP server (stdio, newline-delimited JSON-RPC) exposing a Botcraft bot as tools for an LLM,
// with a decision tree (autopilot), a live view/tree window, view snapshots and Lua plugins.
// Usage: bot_mcp --help
#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <malloc.h>
#include <functional>
#include <iostream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <future>
#include <thread>
#include <tuple>
#include <vector>

#include <arpa/inet.h>
#include <lua.hpp>
#include <netinet/in.h>
#include <poll.h>
#include <regex>
#include <sqlite3.h>
#include <fcntl.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>

#include "build_quality.hpp"
#include "idle_time.hpp"
#include "weather.hpp"
#include "construction_policy.hpp"
#include "spawn_names.hpp"
#include "site_index.hpp"
#include "botcraft/AI/BehaviourTree.hpp"
#include "botcraft/AI/Tasks/AllTasks.hpp"
#include "botcraft/AI/TemplatedBehaviourClient.hpp"
#include "botcraft/Game/AssetsManager.hpp"
#include "botcraft/Game/Entities/EntityManager.hpp"
#include "botcraft/Game/Entities/entities/AgeableMobEntity.hpp"
#include "botcraft/Game/Entities/entities/projectile/FishingHookEntity.hpp"
#include "botcraft/Game/Entities/LocalPlayer.hpp"
#include "botcraft/Game/Inventory/InventoryManager.hpp"
#include "botcraft/Game/Inventory/Item.hpp"
#include "botcraft/Game/Inventory/Window.hpp"
#include "botcraft/Game/World/World.hpp"
#include "botcraft/Game/World/Biome.hpp"
#include "botcraft/Network/NetworkManager.hpp"
#include "botcraft/Renderer/Settings.hpp"
#include "botcraft/Utilities/Logger.hpp"
#include "botcraft/Utilities/SleepUtilities.hpp"
#include "protocolCraft/AllPackets.hpp"
#include "protocolCraft/Utilities/Json.hpp"

using namespace Botcraft;
namespace Json = ProtocolCraft::Json;
using Json::Value;

/// @brief Plain text of a chat component the server sent as structured data (translation key then its arguments,
/// in order), e.g. "commands.locate.biome.success minecraft:forest chat.coordinates 120 ~ -340 512"
static void FlattenChat(const Value& v, std::string& out)
{
    if (v.is_string() || v.is_number())
    {
        out += (out.empty() ? "" : " ") + (v.is_string() ? v.get_string() : v.is_integer() ? std::to_string(v.get_number<long long int>()) : std::to_string(v.get_number<double>()));
    }
    else if (v.is_array())
    {
        for (size_t i = 0; i < v.size(); ++i)
        {
            FlattenChat(v[i], out);
        }
    }
    else if (v.is_object())
    {
        // NBT tags are dumped as { type, name, content }
        if (v.contains("type") && v.contains("content"))
        {
            FlattenChat(v["content"], out);
            return;
        }
        // Everything that is displayed, in order: the translation key first, then its arguments (which NBT
        // wraps under an empty key when they are of mixed types). Not the colors, hover texts or click commands
        static const std::set<std::string> hidden = { "color", "font", "insertion", "click_event", "hover_event", "clickEvent", "hoverEvent", "shadow_color", "type", "name" };
        if (v.contains("translate"))
        {
            FlattenChat(v["translate"], out);
        }
        for (const auto& [key, value] : v.get_object())
        {
            if (key != "translate" && hidden.count(key) == 0 && !value.is_bool())
            {
                FlattenChat(value, out);
            }
        }
    }
}

/// @brief What a bot shares with its team: who to stay close to, and the common objective
struct Team
{
    // Player name of the bot to travel with, "" if not in a team (the leader has its own name here)
    std::string leader;
    // Where the leader is, as told by the main process: used when the leader is out of sight
    bool leader_located = false;
    Botcraft::Vector3<double> leader_at;
    // Attack objective: kind of entity to hunt and how many are left
    std::string hunt_type;
    int hunt_left = 0;
    // Build objective: blocks to place, each with its block item (a filled box, a shelter...)
    bool building = false;
    std::string build_name;
    // The item "minecraft:air" means the spot has to be cleared
    std::vector<std::pair<Botcraft::Position, std::string>> blueprint;
    // The spots where the plan wants a block (not the ones to clear)
    std::set<Botcraft::Position> planned;
    // Spots of the blueprint still to do, as of the bot's last look, and when that was: a bot kept busy by
    // something more urgent has not looked for a while
    int build_left = 0;
    std::chrono::steady_clock::time_point build_looked_at = std::chrono::steady_clock::now() - std::chrono::hours(1);
    // Which hand-over this is: a look at the previous plan must not be written down as a look at this one
    int plan_id = 0;
    // Spots this bot could not do, with how many times it tried and when last. After a minute it tries
    // again (a teammate was standing there, the bot was badly placed), without discarding unfinished work
    std::map<Botcraft::Position, std::pair<int, std::chrono::steady_clock::time_point>> build_failed;

    // Team talk, over the game chat: "[team] <name>: <message>"
    //   any member:  found <block> at <x> <y> <z>         a find worth sharing
    //                bring me to <x> <y> <z>              too far from the site and not allowed to teleport
    //                need <item> at <x> <y> <z>           out of a block: a teammate nearby with plenty brings some
    //                status <what it is doing>            answer to the leader's "report"
    //   the leader:  <name> and <name> mine <block> at <x> <y> <z>
    //                all come to <x> <y> <z>
    //                all report
    std::vector<std::string> members;
    // Blocks the team is after: seeing one is a find to report
    std::vector<std::string> wanted;
    // How far (sideways) a member may be from the leader before going back to it
    double follow_distance = 16.0;
    // Order received from the leader, done before the bot's own search
    bool has_order = false;
    Botcraft::Position order_at;
    std::string order_what;
    // The building site, as a box: nothing is mined for material inside it (walls are made of the very stone
    // the bots go looking for)
    bool has_site = false;
    Botcraft::Position site_min;
    Botcraft::Position site_max;
    // A teammate nearby asked for some of this item: bring it over (see "need" in the team talk)
    bool has_request = false;
    std::string request_item;
    Botcraft::Position request_at;
    // Leader only: what each member last reported, and whose turn it is to help on a find
    std::map<std::string, std::string> reports;
    size_t next_helper = 0;
    // Finds already looked at: block and 16-block cell
    std::set<std::tuple<std::string, int, int, int>> announced;
    // New finds waiting to be told to the team, one per scan
    std::deque<std::pair<std::string, Botcraft::Position>> finds;
};

class McpBot;
// Chat lines waiting for the Lua chat handlers. Every bot sees the same chat, so only one feeds it
static std::atomic<McpBot*> chat_source = nullptr;
static std::mutex chat_events_mutex;
static std::condition_variable chat_events_cond;
static std::deque<std::string> chat_events;

class McpBot : public TemplatedBehaviourClient<McpBot>
{
public:
    /// @brief Return the chat lines received since the last call
    std::vector<std::string> TakeChat()
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        std::vector<std::string> output(chat.begin(), chat.end());
        chat.clear();
        return output;
    }

    /// @brief Queue a team message for the game chat (sent one per second, servers kick chat spammers)
    void Say(const std::string& text)
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        if (outbox.size() < 20)
        {
            outbox.push_back("[team] " + GetNetworkManager()->GetMyName() + ": " + text);
        }
    }

    bool PopOutgoing(std::string& line)
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        if (outbox.empty())
        {
            return false;
        }
        line = outbox.front();
        outbox.pop_front();
        return true;
    }

    /// @brief React to a team message heard in the chat (network thread: only updates state and queues answers)
    void OnTeamMessage(const std::string& sender, const std::string& message);

    /// @brief How many system messages (command answers...) were received so far
    long long GetSystemCount()
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        return system_count;
    }

    /// @brief The system messages received after the first `count` ones
    std::vector<std::string> GetSystemLinesAfter(const long long count)
    {
        std::lock_guard<std::mutex> lock(chat_mutex);
        std::vector<std::string> lines;
        for (const auto& [number, line] : system_lines)
        {
            if (number > count)
            {
                lines.push_back(line);
            }
        }
        return lines;
    }

    /// @brief Record what the bot is doing. Only call it from tree leaves (behaviour thread):
    /// it also writes the state in the blackboard so the GUI shows it
    void SetState(const std::string& s)
    {
        {
            std::lock_guard<std::mutex> lock(state_mutex);
            state_since = std::chrono::steady_clock::now();
            if (s != state)
            {
                flow.push_back(state + " -> " + s);
                if (flow.size() > 20)
                {
                    flow.pop_front();
                }
                state = s;
            }
        }
        GetBlackboard().Set<std::string>("state", s);
    }

    std::string GetState()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return state;
    }

    /// @brief When the current action started (last SetState call)
    std::chrono::steady_clock::time_point GetStateSince()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return state_since;
    }

    void RestartStateClock()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        state_since = std::chrono::steady_clock::now();
    }

    /// @brief Last state transitions, oldest first
    std::vector<std::string> GetFlow()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return std::vector<std::string>(flow.begin(), flow.end());
    }

    /// @brief Why the last action stopped or what the bot is looking for, shown by status and the panel
    void SetNote(const std::string& s)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        note = s;
    }

    std::string GetNote()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return note;
    }

    /// @brief Where and when the bot last died, how many stacks it carried then, and how many times it has died
    struct Death
    {
        Botcraft::Vector3<double> at;
        std::chrono::steady_clock::time_point when;
        int carrying = 0;
        int count = 0;
    };

    Death GetDeath()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return { died_at, died_when, died_carrying, deaths };
    }

    /// @brief True while there may be things of the bot's lying where it died
    std::atomic<bool> recover_items = false;
    /// @brief True when a bot died close by: its things may lie at salvage_at (read under team_mutex)
    std::atomic<bool> salvage = false;
    /// @brief A fight a bot nearby called for ("mob <type> <count> at x y z"), read under team_mutex.
    /// Until assist_until: after that the mobs have moved on, or the fight is over
    /// @brief Blocks this bot could not get (a tree it can't reach, ore it can't dig to): not tried again
    /// for 10 minutes. Behaviour thread only
    std::map<Botcraft::Position, std::chrono::steady_clock::time_point> unreachable;
    /// @brief Where the server last refused every placement nearby (protected area), and until when to not try there
    Botcraft::Position refused_at;
    std::chrono::steady_clock::time_point refused_until;
    /// @brief After a store that failed (chests out of reach): not before this time again
    std::chrono::steady_clock::time_point store_retry_at;
    std::string assist_type;
    int assist_count = 0;
    Botcraft::Position assist_at;
    std::chrono::steady_clock::time_point assist_until;
    /// @brief A player who hit a bot ("attacked by <name> at x y z"): every bot goes after them until avenge_until
    /// (read under team_mutex). Bots are known by the names heard in team talk, and by this bot's name pattern
    std::string avenge;
    Botcraft::Position avenge_at;
    std::chrono::steady_clock::time_point avenge_until;
    std::chrono::steady_clock::time_point avenge_call_at;
    std::set<std::string> bot_names;
    /// @brief When this bot last called the others to a fight: once every 30 seconds at most
    std::chrono::steady_clock::time_point mob_call_at;
    /// @brief Health at the last watchdog look, to see it being hurt; and the last call against a pillager
    float last_health = 20.0f;
    /// @brief A call to fight has interrupted what the bot was doing already (once per call, under team_mutex)
    bool assist_interrupted = false;
    std::chrono::steady_clock::time_point pillager_call_at;
    Botcraft::Position salvage_at;

    /// @brief Resource the autopilot gathers: block id and how many are left to collect
    std::pair<std::string, int> GetGoal()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return { goal_block, goal_left };
    }

    void SetGoal(const std::string& block, const int count)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        goal_block = count > 0 ? block : "";
        goal_left = std::max(count, 0);
    }

    /// @brief True if the plan being built wants a block at this spot: not to be broken to get unstuck or to
    /// dig a way through
    bool IsPlanned(const Botcraft::Position& p)
    {
        std::lock_guard<std::mutex> lock(team_mutex);
        return team.planned.count(p) != 0;
    }

    /// @brief True if the plan being built says something about this spot (a block to put, or air to clear)
    bool InBlueprint(const Botcraft::Position& p)
    {
        std::lock_guard<std::mutex> lock(team_mutex);
        return team.building && std::any_of(team.blueprint.begin(), team.blueprint.end(), [&](const auto& entry) { return entry.first == p; });
    }

    /// @brief True if a block is inside the team's building site: not to be mined for material
    bool InSite(const Botcraft::Position& p)
    {
        std::lock_guard<std::mutex> lock(team_mutex);
        return team.has_site && p.x >= team.site_min.x && p.x <= team.site_max.x && p.y >= team.site_min.y && p.y <= team.site_max.y &&
            p.z >= team.site_min.z && p.z <= team.site_max.z;
    }

    /// @brief Something the bot was asked to make (the make tool): which item, and how many it must end up with
    std::pair<std::string, int> GetMake()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return { make_item, make_count };
    }

    void SetMake(const std::string& item, const int count)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        make_item = count > 0 ? item : "";
        make_count = std::max(count, 0);
    }

    /// @brief Go back to the tree that runs when no tool is: the decision tree if autopilot is on, idle otherwise
    void ResumeDefault();

    std::atomic<bool> autopilot = false;
    Team team;
    std::mutex team_mutex;
    /// @brief Set when the autopilot has gathered or harvested something worth putting in chests
    std::atomic<bool> needs_store = false;
    /// @brief How many times in a row the autopilot walked away to look for something
    std::atomic<int> explorations = 0;
    /// @brief True while the autopilot still has gear to make (tools, armor, shield)
    std::atomic<bool> gear = true;
    /// @brief What the bot was doing when it last left (its memory, see SaveDoing), shown in its status
    std::string was_doing;
    /// @brief Gear actions that failed in a row
    std::atomic<int> gear_failures = 0;
    /// @brief Not before this time will the watchdog interrupt an action again for a nearby monster (stepper thread only)
    std::chrono::steady_clock::time_point danger_retry_at = std::chrono::steady_clock::now();
    /// @brief Set by the watchdog when the bot has not moved for a while: the tree breaks a block beside it
    std::atomic<bool> unstick = false;
    /// @brief True once the server has confirmed a teleport to this bot: it is an operator
    std::atomic<bool> can_teleport = false;
    /// @brief Last time the bot had to unstick itself (behaviour thread only)
    std::chrono::steady_clock::time_point last_unstick = std::chrono::steady_clock::now() - std::chrono::hours(1);
    /// @brief Not before this time will the bot look for a bed again tonight, and the beds that refused it (behaviour thread only)
    std::chrono::steady_clock::time_point sleep_retry_at = std::chrono::steady_clock::now();
    std::set<Botcraft::Position> beds_refused;
    // In bed: what it took off (armor, shield, weapon), as { the bag slot it went to, the slot it came from }
    std::vector<std::pair<short, short>> undressed;
    /// @brief Fuel for the furnace: the bot is out getting coal; and until when it settles for wood after
    /// finding none (behaviour thread only)
    bool coal_wanted = false;
    std::chrono::steady_clock::time_point no_coal_until = std::chrono::steady_clock::now();
    /// @brief Sharing: when the bot may next put a surplus in a chest, and per item when it may look in that
    /// item's chest again after finding it empty (behaviour thread only)
    std::chrono::steady_clock::time_point share_retry_at = std::chrono::steady_clock::now();
    std::map<std::string, std::chrono::steady_clock::time_point> chest_retry_at;
    /// @brief Not before this time will the bot check the light where it builds again (behaviour thread only)
    std::chrono::steady_clock::time_point light_retry_at = std::chrono::steady_clock::now();
    /// @brief When the decision tree last started from its root (behaviour thread only)
    std::chrono::steady_clock::time_point last_decision = std::chrono::steady_clock::now();
    /// @brief Not before this time will the bot look through all the rock around for ore again (behaviour thread only)
    std::chrono::steady_clock::time_point look_retry_at = std::chrono::steady_clock::now();
    /// @brief Per item, not before this time will the bot ask its teammates for some again (behaviour thread only)
    std::map<std::string, std::chrono::steady_clock::time_point> ask_retry_at;
    /// @brief Not before this time will the bot try to teleport again (it is refused to bots that are not operators)
    std::chrono::steady_clock::time_point teleport_retry_at = std::chrono::steady_clock::now();
    /// @brief Not before this time will the bot try to reach its building site again (behaviour thread only)
    std::chrono::steady_clock::time_point build_retry_at = std::chrono::steady_clock::now();
    /// @brief Not before this time will the bot visit a furnace again just to see if something is done (behaviour thread only)
    std::chrono::steady_clock::time_point furnace_retry_at = std::chrono::steady_clock::now();
    /// @brief Not before this time will the bot step away from a neighbour again (behaviour thread only)
    std::chrono::steady_clock::time_point spacing_retry_at = std::chrono::steady_clock::now();
    /// @brief Not before this time will the bot try to rejoin its team leader again (behaviour thread only)
    std::chrono::steady_clock::time_point follow_retry_at = std::chrono::steady_clock::now();
    /// @brief Set by an action that stands still on purpose (smelting): not moving is then not being stuck
    std::atomic<bool> working_in_place = false;
    /// @brief When the last such action ended: building places one block per tick, so between blocks the flag is down
    std::atomic<std::chrono::steady_clock::time_point> worked_in_place_at{};
    /// @brief How many times the watchdog found the bot stuck since it connected
    std::atomic<int> times_stuck = 0;
    Weather weather;
    IdleTime idle_time; // Behaviour thread only; fractional seconds survive short idle periods.
    /// @brief What the bot has done since it joined, for its performance (the brain plugin learns from it):
    /// built, collected, killed, harvested, bred, stored, given, idle_seconds
    std::map<std::string, int> stats;
    std::mutex stats_mutex;
    /// @brief Last place the watchdog saw the bot move from, and when (stepper thread only)
    Botcraft::Vector3<double> watch_position;
    std::chrono::steady_clock::time_point moved_at = std::chrono::steady_clock::now();
    /// @brief After a fruitless search for animals, when to hunt for food again (behaviour thread only)
    std::chrono::steady_clock::time_point food_retry_at = std::chrono::steady_clock::now();

    /// @brief Give up on a gear stage (resource can't be found, crafting keeps failing...)
    void SkipGear(const std::string& stage, const std::string& why)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        gear_skipped[stage] = why;
        gear_skipped_at[stage] = std::chrono::steady_clock::now();
    }

    /// @brief The gear stages that were given up, with the reason
    std::map<std::string, std::string> GetGearSkipped()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        return gear_skipped;
    }

    /// @brief True for 5 minutes after a stage was given up: then it gets another chance (the bot has moved,
    /// found a crafting table, picked something up...)
    bool IsGearSkipped(const std::string& stage)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        const auto skipped = gear_skipped_at.find(stage);
        if (skipped == gear_skipped_at.end())
        {
            return false;
        }
        if (std::chrono::steady_clock::now() - skipped->second > std::chrono::minutes(5))
        {
            gear_skipped.erase(stage);
            gear_skipped_at.erase(skipped);
            return false;
        }
        return true;
    }

    /// @brief Start the gear progression over, retrying the stages that were given up
    void ResetGear()
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        gear_skipped.clear();
        gear_skipped_at.clear();
        gear_stage.clear();
        gear = true;
        gear_failures = 0;
    }

    /// @brief Count one more action spent on this gear stage, return how many so far
    int CountGearStep(const std::string& stage)
    {
        std::lock_guard<std::mutex> lock(state_mutex);
        if (stage != gear_stage)
        {
            gear_stage = stage;
            gear_steps = 0;
        }
        return ++gear_steps;
    }
    /// @brief One tool action at a time per bot: the MCP thread, the web GUI and the Lua chat handlers all run some
    std::mutex run_mutex;

protected:
    using ProtocolCraft::Handler::Handle;
    virtual void Handle(ProtocolCraft::ClientboundGameEventPacket& packet) override
    {
        weather.Update(packet.GetType(), packet.GetParam());
        ManagersClient::Handle(packet);
    }
    virtual void Handle(ProtocolCraft::ClientboundLoginPacket& packet) override
    {
        weather.Reset();
        ManagersClient::Handle(packet);
    }
    virtual void Handle(ProtocolCraft::ClientboundRespawnPacket& packet) override
    {
        weather.Reset();
        ManagersClient::Handle(packet);
    }
    virtual void Handle(ProtocolCraft::ClientboundPlayerChatPacket& packet) override
    {
        PushChat("<" + GetPlayerName(packet.GetSender()) + "> " + packet.GetBody().GetContent());
    }
    virtual void Handle(ProtocolCraft::ClientboundRegistryDataPacket& packet) override
    {
        if (packet.GetRegistry().GetFull() == "minecraft:damage_type")
        {
            damage_types.clear();
            for (const auto& entry : packet.GetEntries())
            {
                damage_types.push_back(entry.GetId().GetFull());
            }
            LOG_INFO("Damage types known: " << damage_types.size());
        }
        ManagersClient::Handle(packet);
    }

    virtual void Handle(ProtocolCraft::ClientboundSetHealthPacket& packet) override
    {
        // Health lost, by cause (the reward counts it): all of it as damage_taken, and drowning, falls and
        // fire or lava apart, with the deaths they caused
        if (GetLocalPlayer() && !dead)
        {
            const int lost = static_cast<int>(std::ceil(GetLocalPlayer()->GetHealth() - std::max(0.0f, packet.GetHealth())));
            const bool recent = std::chrono::steady_clock::now() - last_hurt_when < std::chrono::seconds(2);
            std::lock_guard<std::mutex> lock(stats_mutex);
            if (lost > 0)
            {
                stats["damage_taken"] += lost;
                if (recent && !last_hurt.empty())
                {
                    stats[last_hurt + "_damage"] += lost;
                }
            }
            if (packet.GetHealth() <= 0.0f && recent && !last_hurt.empty())
            {
                stats[last_hurt + "_deaths"] += 1;
            }
        }
        // Health at zero: the bot just died. Where, and what it was carrying, is noted before the client
        // respawns it somewhere else: its things lie there for five minutes
        if (packet.GetHealth() <= 0.0f && !dead && GetLocalPlayer() && GetInventoryManager() && GetInventoryManager()->GetPlayerInventory())
        {
            dead = true;
            int carried = 0;
            for (const auto& [index, slot] : GetInventoryManager()->GetPlayerInventory()->GetSlots())
            {
                carried += !slot.IsEmptySlot();
            }
            std::lock_guard<std::mutex> lock(state_mutex);
            died_at = GetLocalPlayer()->GetPosition();
            died_when = std::chrono::steady_clock::now();
            died_carrying = carried;
            ++deaths;
            recover_items = carried > 0;
            if (carried > 0)
            {
                Say("died at " + std::to_string(static_cast<int>(std::floor(died_at.x))) + " " + std::to_string(static_cast<int>(std::floor(died_at.y))) + " " +
                    std::to_string(static_cast<int>(std::floor(died_at.z))));
            }
        }
        else if (packet.GetHealth() > 0.0f)
        {
            dead = false;
        }
        ManagersClient::Handle(packet);
    }

    virtual void Handle(ProtocolCraft::ClientboundDamageEventPacket& packet) override
    {
        // Hit by a player who is not one of the bots: every bot is called on them (the cause is the entity id + 1)
        const std::shared_ptr<LocalPlayer> me = GetLocalPlayer();
        if (me && packet.GetEntityId() == me->GetEntityID())
        {
            const int type = packet.GetSourceTypeId();
            const std::string name = type >= 0 && type < static_cast<int>(damage_types.size()) ? damage_types[type] : "";
            last_hurt = name == "minecraft:drown" ? "drown"
                : name == "minecraft:fall" || name == "minecraft:stalagmite" ? "fall"
                : name == "minecraft:lava" || name == "minecraft:in_fire" || name == "minecraft:on_fire" || name == "minecraft:hot_floor" ||
                    name == "minecraft:campfire" ? "burn" : "";
            last_hurt_when = std::chrono::steady_clock::now();
        }
        if (me && packet.GetEntityId() == me->GetEntityID() && packet.GetSourceCauseId() > 0)
        {
            const std::shared_ptr<Entity> attacker = GetEntityManager()->GetEntity(packet.GetSourceCauseId() - 1);
            const std::string name = attacker && attacker->IsRemotePlayer() ? GetPlayerName(attacker->GetUUID()) : "";
            const auto now = std::chrono::steady_clock::now();
            if (!name.empty() && !IsBotName(name) && now >= avenge_call_at)
            {
                avenge_call_at = now + std::chrono::seconds(10);
                const Vector3<double> pos = me->GetPosition();
                const Position at(static_cast<int>(std::floor(pos.x)), static_cast<int>(std::floor(pos.y)), static_cast<int>(std::floor(pos.z)));
                Say("attacked by " + name + " at " + std::to_string(at.x) + " " + std::to_string(at.y) + " " + std::to_string(at.z));
                std::lock_guard<std::mutex> lock(team_mutex);
                avenge = name;
                avenge_at = at;
                avenge_until = now + std::chrono::seconds(120);
            }
        }
        ManagersClient::Handle(packet);
    }

    /// @brief One of the bots: a name heard in team talk, or this bot's name with other digits at the end (Bot, Bot7)
    bool IsBotName(const std::string& name)
    {
        const auto base = [](const std::string& n) { return n.substr(0, n.find_last_not_of("0123456789") + 1); };
        std::lock_guard<std::mutex> lock(team_mutex);
        return bot_names.count(name) != 0 || base(name) == base(GetNetworkManager()->GetMyName());
    }

    virtual void Handle(ProtocolCraft::ClientboundSystemChatPacket& packet) override
    {
        std::string text = packet.GetContent().GetText();
        if (text.empty())
        {
            FlattenChat(packet.GetContent().Serialize(), text);
        }
        // Teleport confirmations only reach operators: seeing one means this bot can teleport, itself and others
        if (text.find("commands.teleport.success") != std::string::npos)
        {
            can_teleport = true;
        }
        {
            std::lock_guard<std::mutex> lock(chat_mutex);
            system_lines.push_back({ ++system_count, text });
            if (system_lines.size() > 50)
            {
                system_lines.pop_front();
            }
        }
        PushChat(text);
    }

private:
    void PushChat(const std::string& line)
    {
        // Translatable system messages have no plain text
        if (line.empty())
        {
            return;
        }
        {
            std::lock_guard<std::mutex> lock(chat_mutex);
            chat.push_back(line);
            if (chat.size() > 100)
            {
                chat.pop_front();
            }
        }
        // Team talk carries its sender in the text, so it reads the same whatever way the server relays chat
        const size_t tag = line.find("[team] ");
        const size_t colon = tag == std::string::npos ? std::string::npos : line.find(": ", tag);
        if (colon != std::string::npos)
        {
            const std::string sender = line.substr(tag + 7, colon - tag - 7);
            {
                std::lock_guard<std::mutex> lock(team_mutex);
                bot_names.insert(sender);
            }
            if (sender != GetNetworkManager()->GetMyName())
            {
                try
                {
                    OnTeamMessage(sender, line.substr(colon + 2));
                }
                catch (const std::exception&)
                {
                    // Not one of ours after all (a player typing "[team] ...")
                }
            }
        }
        if (chat_source == this)
        {
            {
                std::lock_guard<std::mutex> lock(chat_events_mutex);
                chat_events.push_back(line);
                if (chat_events.size() > 100)
                {
                    chat_events.pop_front();
                }
            }
            chat_events_cond.notify_all();
        }
    }

    std::mutex chat_mutex;
    std::deque<std::string> chat;
    std::deque<std::string> outbox;
    std::deque<std::pair<long long, std::string>> system_lines;
    long long system_count = 0;

    std::atomic<bool> dead = false;
    Botcraft::Vector3<double> died_at;
    std::chrono::steady_clock::time_point died_when;
    int died_carrying = 0;
    int deaths = 0;
    // Damage types by registry index, as the server lists them, and what last hurt the bot: drown, fall, burn
    std::vector<std::string> damage_types;
    std::string last_hurt;
    std::chrono::steady_clock::time_point last_hurt_when;

    std::mutex state_mutex;
    std::string state = "starting";
    std::chrono::steady_clock::time_point state_since = std::chrono::steady_clock::now();
    std::string note;
    std::deque<std::string> flow;
    std::string goal_block;
    int goal_left = 0;
    std::string make_item;
    int make_count = 0;
    std::map<std::string, std::string> gear_skipped;
    std::map<std::string, std::chrono::steady_clock::time_point> gear_skipped_at;
    std::string gear_stage;
    int gear_steps = 0;
};

// ---------------------------------------------------------------- helpers

/// "Oak_Log" -> "minecraft:oak_log"
static std::string Id(std::string s)
{
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c) { return std::tolower(c); });
    return s.find(':') == std::string::npos ? "minecraft:" + s : s;
}

static Position BlockPos(const Vector3<double>& v)
{
    return Position(static_cast<int>(std::floor(v.x)), static_cast<int>(std::floor(v.y)), static_cast<int>(std::floor(v.z)));
}

static Value PosJson(const Vector3<double>& v)
{
    Value o;
    o["x"] = std::round(v.x * 10.0) / 10.0;
    o["y"] = std::round(v.y * 10.0) / 10.0;
    o["z"] = std::round(v.z * 10.0) / 10.0;
    return o;
}

static double Num(const Value& a, const std::string& key)
{
    if (!a.is_object() || !a.contains(key) || !a[key].is_number())
    {
        throw std::runtime_error("missing numeric argument: " + key);
    }
    return a[key].get_number<double>();
}

static double Num(const Value& a, const std::string& key, const double fallback)
{
    return a.is_object() && a.contains(key) && a[key].is_number() ? a[key].get_number<double>() : fallback;
}

static std::string Str(const Value& a, const std::string& key)
{
    if (!a.is_object() || !a.contains(key) || !a[key].is_string())
    {
        throw std::runtime_error("missing string argument: " + key);
    }
    return a[key].get_string();
}

/// @brief The "name" argument as one word, or a default
static std::string SiteName(const Value& a, const std::string& otherwise)
{
    std::string name = a.is_object() && a.contains("name") && a["name"].is_string() ? a["name"].get_string() : otherwise;
    std::replace(name.begin(), name.end(), ' ', '_');
    return name;
}

static Position Pos(const Value& a)
{
    return Position(static_cast<int>(std::floor(Num(a, "x"))), static_cast<int>(std::floor(Num(a, "y"))), static_cast<int>(std::floor(Num(a, "z"))));
}

/// @brief The bot's inventory slots, empty until the server has sent them (right after joining)
/// @brief The player's slots: armor, storage, hotbar and off hand. Not the 2x2 crafting grid nor its output:
/// what shows there is not the bot's (the output is a preview), and counted it had a bot "holding" 4 planks it
/// kept failing to craft with
static std::map<short, ProtocolCraft::Slot> PlayerSlots(BehaviourClient& c)
{
    const std::shared_ptr<Window> inventory = c.GetInventoryManager()->GetPlayerInventory();
    std::map<short, ProtocolCraft::Slot> slots = inventory ? inventory->GetSlots() : std::map<short, ProtocolCraft::Slot>();
    slots.erase(slots.begin(), slots.lower_bound(Window::INVENTORY_ARMOR_START));
    return slots;
}

/// @brief While one of these lives, the bot is doing something that keeps it on the spot on purpose (cutting a
/// tree, smelting, sleeping): the watchdog must not take it for stuck. Scoped, so the flag is cleared even when
/// the action is interrupted halfway (a tree swap unwinds the stack)
struct WorkingInPlace
{
    McpBot* bot;
    explicit WorkingInPlace(BehaviourClient& c) : bot(dynamic_cast<McpBot*>(&c))
    {
        if (bot != nullptr)
        {
            bot->working_in_place = true;
        }
    }
    ~WorkingInPlace()
    {
        if (bot != nullptr)
        {
            bot->working_in_place = false;
            bot->worked_in_place_at = std::chrono::steady_clock::now();
        }
    }
    WorkingInPlace(const WorkingInPlace&) = delete;
    WorkingInPlace& operator=(const WorkingInPlace&) = delete;
};

/// @brief Sleep inside a behaviour task while staying interruptible
static void Wait(BehaviourClient& c, const int ms)
{
    Utilities::YieldForCondition([]() { return false; }, c, ms);
}

/// @brief Loaded blocks matching the predicate in a cube around the bot, nearest first
static std::vector<Position> FindBlocks(BehaviourClient& c, const std::function<bool(const Blockstate&)>& match, int radius, const size_t max_results,
    const std::function<bool(const Position&)>& allowed = nullptr)
{
    // Shell by shell outward from the bot (cubes of growing half-size), stopping as soon as the nearest
    // max_results are certain: the next shell can't hold anything nearer. A search with matches close by
    // looks at a few thousand blocks instead of the whole cube (~275k lookups at radius 32), and only the
    // kept results are sorted. ponytail: still the whole cube when there is nothing, index by chunk palette if that shows
    radius = std::clamp(radius, 1, 32);
    const std::shared_ptr<World> world = c.GetWorld();
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    const Position center = BlockPos(me);
    const int min_y = world->GetMinY();
    const int max_y = world->GetMinY() + world->GetHeight() - 1;
    const auto dist = [&](const Position& p) { return me.SqrDist(Vector3<double>(p.x + 0.5, p.y + 0.5, p.z + 0.5)); };
    const auto closer = [&](const Position& a, const Position& b) { return dist(a) < dist(b); };

    std::vector<Position> found;
    const auto look = [&](const int x, const int y, const int z)
        {
            if (y < min_y || y > max_y)
            {
                return;
            }
            const Position p(x, y, z);
            const Blockstate* block = world->GetBlock(p);
            // (allowed: if given, only the positions it accepts)
            if (block != nullptr && match(*block) && (!allowed || allowed(p)))
            {
                found.push_back(p);
            }
        };
    for (int r = 0; r <= radius; ++r)
    {
        for (int dy = -r; dy <= r; ++dy)
        {
            for (int dx = -r; dx <= r; ++dx)
            {
                if (std::abs(dy) == r || std::abs(dx) == r)
                {
                    for (int dz = -r; dz <= r; ++dz)
                    {
                        look(center.x + dx, center.y + dy, center.z + dz);
                    }
                }
                else
                {
                    look(center.x + dx, center.y + dy, center.z - r);
                    if (r > 0)
                    {
                        look(center.x + dx, center.y + dy, center.z + r);
                    }
                }
            }
        }
        if (found.size() >= max_results)
        {
            // Everything in the next shells is at least r + 0.5 - 1 away from the bot (it stands off-centre in its block)
            std::nth_element(found.begin(), found.begin() + (max_results - 1), found.end(), closer);
            const double kth = std::sqrt(dist(found[max_results - 1]));
            if (r - 0.5 >= kth)
            {
                break;
            }
        }
    }
    if (found.size() > max_results)
    {
        std::nth_element(found.begin(), found.begin() + (max_results - 1), found.end(), closer);
        found.resize(max_results);
    }
    std::sort(found.begin(), found.end(), closer);
    return found;
}

// ---------------------------------------------------------------- memory: places the bot has seen (SQLite)

// One database per bot, memory/NAME.sqlite. Every 10 seconds the blocks worth remembering around the bot
// are written to it, so it can walk back to a known tree, ore, crafting table, chest or bed instead of
// searching again, across restarts too. All the bots can live in one process: each one's own, by bot
static std::map<const BehaviourClient*, sqlite3*> memory_dbs;
// The scan (stepper thread), the tasks (behaviour thread) and the tools (MCP, web threads) all use them.
// ponytail: one lock for every bot's database, per-bot locks if the scans ever queue up behind it
static std::mutex memory_mutex;

/// @brief This bot's memory database, nullptr if none is open
static sqlite3* MemoryOf(const BehaviourClient& c)
{
    static std::mutex dbs_mutex;
    std::lock_guard<std::mutex> lock(dbs_mutex);
    const auto it = memory_dbs.find(&c);
    return it == memory_dbs.end() ? nullptr : it->second;
}

static void CloseMemory(const BehaviourClient& c)
{
    std::lock_guard<std::mutex> lock(memory_mutex);
    sqlite3_close(MemoryOf(c));
    memory_dbs.erase(&c);
}

static void OpenMemory(const BehaviourClient& c, const std::string& bot_name)
{
    CloseMemory(c);
    std::lock_guard<std::mutex> lock(memory_mutex);
    sqlite3* memory_db = nullptr;
    std::filesystem::create_directories("../memory");
    if (sqlite3_open(("../memory/" + bot_name + ".sqlite").c_str(), &memory_db) != SQLITE_OK ||
        sqlite3_exec(memory_db,
            // Write-ahead log, synced at checkpoints only: a commit no longer waits for the disk. With every bot
            // writing every 10 seconds, the syncs had the disk at 40% wait and the whole program stalled
            "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;"
            "CREATE TABLE IF NOT EXISTS places ("
            "  kind TEXT NOT NULL, dimension TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL,"
            "  seen INTEGER NOT NULL, PRIMARY KEY (dimension, x, y, z));"
            "CREATE INDEX IF NOT EXISTS places_kind ON places (kind);"
            // Single places that are the bot's own (its bed): not rewritten by the scans
            "CREATE TABLE IF NOT EXISTS own (what TEXT PRIMARY KEY, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL);"
            // What the bot is doing (see SaveDoing): taken up again when it joins next time
            "CREATE TABLE IF NOT EXISTS doing (key TEXT PRIMARY KEY, value TEXT NOT NULL);", nullptr, nullptr, nullptr) != SQLITE_OK)
    {
        LOG_ERROR("Memory: can't open the database of " << bot_name << ": " << (memory_db ? sqlite3_errmsg(memory_db) : "out of memory"));
        sqlite3_close(memory_db);
        return;
    }
    memory_dbs[&c] = memory_db;
}

/// @brief One line of what the bot is doing, in its memory; an empty value forgets it
static void SaveDoing(const BehaviourClient& c, const std::string& key, const std::string& value)
{
    sqlite3* const memory_db = MemoryOf(c);
    std::lock_guard<std::mutex> lock(memory_mutex);
    if (memory_db == nullptr)
    {
        return;
    }
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(memory_db, value.empty() ? "DELETE FROM doing WHERE key = ?1" : "INSERT OR REPLACE INTO doing (key, value) VALUES (?1, ?2)", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, key.c_str(), -1, SQLITE_TRANSIENT);
    if (!value.empty())
    {
        sqlite3_bind_text(statement, 2, value.c_str(), -1, SQLITE_TRANSIENT);
    }
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

static std::map<std::string, std::string> LoadDoing(const BehaviourClient& c)
{
    sqlite3* const memory_db = MemoryOf(c);
    std::map<std::string, std::string> doing;
    std::lock_guard<std::mutex> lock(memory_mutex);
    sqlite3_stmt* statement = nullptr;
    if (memory_db == nullptr || sqlite3_prepare_v2(memory_db, "SELECT key, value FROM doing", -1, &statement, nullptr) != SQLITE_OK)
    {
        return doing;
    }
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        doing[reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))] = reinterpret_cast<const char*>(sqlite3_column_text(statement, 1));
    }
    sqlite3_finalize(statement);
    return doing;
}

// One database shared by all the bots, botcraft.sqlite: the construction sites, the chests, the mines.
// Every bot process opens it, the busy timeout makes them wait for each other's writes
static sqlite3* SharedDb()
{
    static sqlite3* db = []
        {
            sqlite3* db = nullptr;
            if (sqlite3_open("../botcraft.sqlite", &db) != SQLITE_OK ||
                sqlite3_busy_timeout(db, 5000) != SQLITE_OK ||
                sqlite3_exec(db,
                    "PRAGMA journal_mode = WAL; PRAGMA synchronous = NORMAL;"
                    "CREATE TABLE IF NOT EXISTS sites (x1 INTEGER NOT NULL, y1 INTEGER NOT NULL, z1 INTEGER NOT NULL,"
                    "  x2 INTEGER NOT NULL, y2 INTEGER NOT NULL, z2 INTEGER NOT NULL, name TEXT NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS chests (item TEXT PRIMARY KEY, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL);"
                    "CREATE TABLE IF NOT EXISTS mines (x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL, resources TEXT NOT NULL);"
                    // Jobs the main program gives (feeders): given again after a restart
                    "CREATE TABLE IF NOT EXISTS roles (bot TEXT NOT NULL, role TEXT NOT NULL, PRIMARY KEY (bot, role));"
                    // What each bot reports to the others (see ReportFinds): ores, logs, crops, berry bushes and
                    // farm animals, one place per kind and 16-block cell. Recall reads it with the bot's own memory
                    "CREATE TABLE IF NOT EXISTS finds (kind TEXT NOT NULL, dimension TEXT NOT NULL, cx INTEGER NOT NULL, cy INTEGER NOT NULL,"
                    "  cz INTEGER NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL, z INTEGER NOT NULL, seen INTEGER NOT NULL, by TEXT NOT NULL,"
                    "  PRIMARY KEY (kind, dimension, cx, cy, cz));"
                    "CREATE INDEX IF NOT EXISTS finds_kind ON finds (kind);"
                    // What is in each chest, as the last bot that opened it saw it (see RememberChest): every bot
                    // reads it. A row with item '' marks the chest itself, empty or not
                    "CREATE TABLE IF NOT EXISTS chest_contents (dimension TEXT NOT NULL, x INTEGER NOT NULL, y INTEGER NOT NULL,"
                    "  z INTEGER NOT NULL, item TEXT NOT NULL, count INTEGER NOT NULL, seen INTEGER NOT NULL, by TEXT NOT NULL,"
                    "  PRIMARY KEY (dimension, x, y, z, item));"
                    "CREATE INDEX IF NOT EXISTS chest_contents_item ON chest_contents (item);",
                    nullptr, nullptr, nullptr) != SQLITE_OK)
            {
                LOG_ERROR("Can't open the shared database: " << (db ? sqlite3_errmsg(db) : "out of memory"));
                sqlite3_close(db);
                db = nullptr;
            }
            return db;
        }();
    return db;
}

/// @brief Remember a place that belongs to the bot ("bed")
static void SetOwn(const BehaviourClient& c, const std::string& what, const Position& p)
{
    sqlite3* const memory_db = MemoryOf(c);
    std::lock_guard<std::mutex> lock(memory_mutex);
    sqlite3_stmt* statement = nullptr;
    if (memory_db == nullptr || sqlite3_prepare_v2(memory_db, "INSERT OR REPLACE INTO own (what, x, y, z) VALUES (?1, ?2, ?3, ?4)", -1, &statement, nullptr) != SQLITE_OK)
    {
        return;
    }
    sqlite3_bind_text(statement, 1, what.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, p.x);
    sqlite3_bind_int(statement, 3, p.y);
    sqlite3_bind_int(statement, 4, p.z);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

static void ForgetOwn(const BehaviourClient& c, const std::string& what)
{
    sqlite3* const memory_db = MemoryOf(c);
    std::lock_guard<std::mutex> lock(memory_mutex);
    sqlite3_stmt* statement = nullptr;
    if (memory_db == nullptr || sqlite3_prepare_v2(memory_db, "DELETE FROM own WHERE what = ?1", -1, &statement, nullptr) != SQLITE_OK)
    {
        return;
    }
    sqlite3_bind_text(statement, 1, what.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

static bool GetOwn(const BehaviourClient& c, const std::string& what, Position& p)
{
    sqlite3* const memory_db = MemoryOf(c);
    std::lock_guard<std::mutex> lock(memory_mutex);
    sqlite3_stmt* statement = nullptr;
    if (memory_db == nullptr || sqlite3_prepare_v2(memory_db, "SELECT x, y, z FROM own WHERE what = ?1", -1, &statement, nullptr) != SQLITE_OK)
    {
        return false;
    }
    sqlite3_bind_text(statement, 1, what.c_str(), -1, SQLITE_TRANSIENT);
    const bool found = sqlite3_step(statement) == SQLITE_ROW;
    if (found)
    {
        p = Position(sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1), sqlite3_column_int(statement, 2));
    }
    sqlite3_finalize(statement);
    return found;
}

/// @brief Blocks worth remembering: resources and things the bot can use
static bool IsMemorable(const Blockstate& b)
{
    const std::string& name = b.GetName();
    for (const char* suffix : { "_ore", "_log", "_bed", ":crafting_table", ":furnace", ":chest", ":barrel", ":wheat", ":carrots", ":potatoes", ":beetroots", ":sweet_berry_bush" })
    {
        const size_t size = std::char_traits<char>::length(suffix);
        if (name.size() >= size && name.compare(name.size() - size, size, suffix) == 0)
        {
            return true;
        }
    }
    return false;
}


static std::vector<std::string> SplitSpec(const std::string& spec);
static bool MatchesAny(const std::string& block_name, const std::vector<std::string>& items);

/// @brief Add one place to the memory (told by a teammate)
static void RememberPlace(BehaviourClient& c, const std::string& kind, const Position& p)
{
    sqlite3* const memory_db = MemoryOf(c);
    const std::string dimension = c.GetWorld()->GetCurrentDimension();
    std::lock_guard<std::mutex> lock(memory_mutex);
    if (memory_db == nullptr)
    {
        return;
    }
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(memory_db, "INSERT OR REPLACE INTO places (kind, dimension, x, y, z, seen) VALUES (?1, ?2, ?3, ?4, ?5, strftime('%s', 'now'))", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, dimension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, p.x);
    sqlite3_bind_int(statement, 4, p.y);
    sqlite3_bind_int(statement, 5, p.z);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

/// @brief Leader: put two members on a find, the one who saw it and the next in turn, and tell them in the chat
static void AssignFind(McpBot& leader, const std::string& finder, const std::string& kind, const Position& p)
{
    const std::string me = leader.GetNetworkManager()->GetMyName();
    std::string helper;
    {
        std::lock_guard<std::mutex> lock(leader.team_mutex);
        Team& team = leader.team;
        for (size_t i = 0; i < team.members.size() && helper.empty(); ++i)
        {
            const std::string& candidate = team.members[team.next_helper++ % team.members.size()];
            if (candidate != finder)
            {
                helper = candidate;
            }
        }
        // The leader hears its own orders nowhere: take the one meant for itself here
        if (finder == me || helper == me)
        {
            team.has_order = true;
            team.order_at = p;
            team.order_what = kind;
        }
    }
    leader.Say(finder + (helper.empty() ? "" : " and " + helper) + " mine " + kind + " at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z));
}

void McpBot::OnTeamMessage(const std::string& sender, const std::string& message)
{
    std::vector<std::string> words;
    size_t start = 0;
    while (start < message.size())
    {
        const size_t end = std::min(message.find(' ', start), message.size());
        if (end > start)
        {
            words.push_back(message.substr(start, end - start));
        }
        start = end + 1;
    }
    if (words.size() >= 7 && words[0] == "attacked" && words[1] == "by" && words[3] == "at")
    {
        // A player hit one of the bots: all of them go after that player, wherever they are
        std::lock_guard<std::mutex> lock(team_mutex);
        avenge = words[2];
        avenge_at = Position(std::stoi(words[4]), std::stoi(words[5]), std::stoi(words[6]));
        avenge_until = std::chrono::steady_clock::now() + std::chrono::seconds(120);
        return;
    }
    if (words.size() >= 7 && words[0] == "mob" && words[3] == "at")
    {
        // A fight close by: join it (whether this bot is fit for it, health and weapon, is the tree's to say)
        const Position at(std::stoi(words[4]), std::stoi(words[5]), std::stoi(words[6]));
        const Vector3<double> here = GetLocalPlayer()->GetPosition();
        // A pillager calls everyone within 100 blocks (they come from afar: 3 minutes to get there), the
        // others whoever is within 48
        const bool pillager = words[1] == "pillager";
        const double reach = pillager ? 100.0 : 48.0;
        if (sender != GetNetworkManager()->GetMyName() && std::abs(here.x - at.x) <= reach && std::abs(here.z - at.z) <= reach &&
            std::abs(here.y - at.y) <= (pillager ? 40.0 : 16.0))
        {
            std::lock_guard<std::mutex> lock(team_mutex);
            assist_type = words[1];
            assist_count = std::clamp(std::stoi(words[2]), 1, 8);
            assist_at = at;
            assist_until = std::chrono::steady_clock::now() + std::chrono::seconds(pillager ? 180 : 90);
            assist_interrupted = false;
        }
        return;
    }
    if (words.size() >= 5 && words[0] == "died" && words[1] == "at")
    {
        // Close by, and not far above or below: a tree branch goes and picks up what lies there
        const Position at(std::stoi(words[2]), std::stoi(words[3]), std::stoi(words[4]));
        const Vector3<double> here = GetLocalPlayer()->GetPosition();
        if (sender != GetNetworkManager()->GetMyName() && std::abs(here.x - at.x) <= 24.0 && std::abs(here.z - at.z) <= 24.0 && std::abs(here.y - at.y) <= 6.0)
        {
            std::lock_guard<std::mutex> lock(team_mutex);
            salvage_at = at;
            salvage = true;
        }
        return;
    }
    const std::string me = GetNetworkManager()->GetMyName();
    std::string leader;
    {
        std::lock_guard<std::mutex> lock(team_mutex);
        leader = team.leader;
        if (leader.empty() || std::find(team.members.begin(), team.members.end(), sender) == team.members.end())
        {
            return;
        }
    }
    const auto position_at = [&](const size_t i) { return Position(std::stoi(words.at(i)), std::stoi(words.at(i + 1)), std::stoi(words.at(i + 2))); };

    if (words.size() >= 6 && words[0] == "found" && words[2] == "at")
    {
        // Everybody knows about it from now on; the leader decides who goes
        RememberPlace(*this, words[1], position_at(3));
        if (leader == me)
        {
            AssignFind(*this, sender, words[1], position_at(3));
        }
        return;
    }
    if (words.size() >= 6 && words[0] == "bring" && words[1] == "me" && words[2] == "to")
    {
        // A teammate too far from the site that can't teleport itself: any bot that can does it
        if (can_teleport)
        {
            const Position to = position_at(3);
            SendChatCommand("minecraft:tp " + sender + " " + std::to_string(to.x) + " " + std::to_string(to.y) + " " + std::to_string(to.z));
        }
        return;
    }
    if (words.size() >= 6 && words[0] == "need" && words[2] == "at")
    {
        // Close by and with plenty of it: this bot will walk over and drop half a stack (a tree branch does it)
        const Position at = position_at(3);
        const std::string item = words[1];
        if (GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(at.x, at.y, at.z)) < 32.0 * 32.0 && GetInventoryManager() && GetInventoryManager()->GetPlayerInventory())
        {
            int count = 0;
            for (const auto& [index, slot] : GetInventoryManager()->GetPlayerInventory()->GetSlots())
            {
                if (!slot.IsEmptySlot() && AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() == item)
                {
                    count += slot.GetItemCount();
                }
            }
            if (count >= 48)
            {
                std::lock_guard<std::mutex> lock(team_mutex);
                team.has_request = true;
                team.request_item = item;
                team.request_at = at;
            }
        }
        return;
    }
    if (words.size() >= 1 && words[0] == "status")
    {
        if (leader == me)
        {
            std::lock_guard<std::mutex> lock(team_mutex);
            team.reports[sender] = message.substr(7);
        }
        return;
    }
    // The rest are orders: only the leader gives them
    if (sender != leader)
    {
        return;
    }
    const auto verb = std::find_if(words.begin(), words.end(), [](const std::string& w) { return w == "mine" || w == "come" || w == "report"; });
    if (verb == words.end() || (std::find(words.begin(), verb, me) == verb && std::find(words.begin(), verb, "all") == verb))
    {
        return;
    }
    const size_t i = static_cast<size_t>(verb - words.begin());
    if (*verb == "report")
    {
        const Vector3<double> at = GetLocalPlayer()->GetPosition();
        Say("status " + GetState() + ", " + std::to_string(GetGoal().second) + " left, health " + std::to_string(static_cast<int>(GetLocalPlayer()->GetHealth())) +
            ", at " + std::to_string(static_cast<int>(at.x)) + " " + std::to_string(static_cast<int>(at.y)) + " " + std::to_string(static_cast<int>(at.z)));
    }
    else if (*verb == "mine" && words.at(i + 2) == "at")
    {
        RememberPlace(*this, words.at(i + 1), position_at(i + 3));
        std::lock_guard<std::mutex> lock(team_mutex);
        team.has_order = true;
        team.order_at = position_at(i + 3);
        team.order_what = words.at(i + 1);
    }
    else if (*verb == "come" && words.at(i + 1) == "to")
    {
        std::lock_guard<std::mutex> lock(team_mutex);
        team.has_order = true;
        team.order_at = position_at(i + 2);
        team.order_what = "";
    }
}

struct Ent
{
    int id;
    std::string name;
    Vector3<double> pos;
    double dist;
    bool animal = false;
};

static std::vector<Ent> FindEntities(BehaviourClient& c, std::string type, const double radius);
static bool IsPenned(BehaviourClient& c, const Vector3<double>& animal);

/// @brief Replace what is remembered in the 24 blocks around the bot with what is there now (monsters
/// included), tell the team about the first new find of something it is after, and call the bots around
/// to a fight when monsters gather
static bool InStoredSite(const Position& p);
static void ReportFinds(const std::string& by, const std::string& dimension, const Position& center, const int radius,
    const std::vector<std::pair<std::string, Position>>& finds);

static void Remember(McpBot& c)
{
    sqlite3* const memory_db = MemoryOf(c);
    constexpr int radius = 24;
    const std::shared_ptr<World> world = c.GetWorld();
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    const Position center(static_cast<int>(std::floor(me.x)), static_cast<int>(std::floor(me.y)), static_cast<int>(std::floor(me.z)));
    // Not while chunks are still arriving: a block we can't see yet is not a block that is gone
    for (const int dx : { -radius, radius })
    {
        for (const int dz : { -radius, radius })
        {
            if (!world->IsLoaded(Position(center.x + dx, center.y, center.z + dz)))
            {
                return;
            }
        }
    }
    const std::vector<Position> found = FindBlocks(c, IsMemorable, radius, 20000);
    const std::string dimension = world->GetCurrentDimension();
    // Monsters in sight: remembered with the blocks ("mob:zombie", recall finds them), and when there are two
    // or more close by, or the bot is hurt, the bots around are called to fight them together
    const std::vector<Ent> monsters = FindEntities(c, "hostile", radius);
    {
        std::map<std::string, int> close;
        for (const Ent& m : monsters)
        {
            // Not creepers: a crowd around one is what it waits for, and it blows up walls with them
            if (m.dist <= 16.0 && m.name != "creeper")
            {
                ++close[m.name];
            }
        }
        const auto now = std::chrono::steady_clock::now();
        if (!close.empty() && now >= c.mob_call_at && (monsters.size() >= 2 || c.GetLocalPlayer()->GetHealth() < 14.0f))
        {
            const auto most = std::max_element(close.begin(), close.end(), [](const auto& a, const auto& b) { return a.second < b.second; });
            const auto first = std::find_if(monsters.begin(), monsters.end(), [&](const Ent& m) { return m.name == most->first; });
            const Position at = BlockPos(first->pos);
            c.mob_call_at = now + std::chrono::seconds(30);
            c.Say("mob " + most->first + " " + std::to_string(most->second) + " at " + std::to_string(at.x) + " " + std::to_string(at.y) + " " + std::to_string(at.z));
            // And it takes them on itself, with the others coming
            std::lock_guard<std::mutex> lock(c.team_mutex);
            c.assist_type = most->first;
            c.assist_count = most->second;
            c.assist_at = at;
            c.assist_until = now + std::chrono::seconds(90);
        }
    }

    // What is new to the bot is a find for the team. What it already had in memory (seen before, or told
    // by a teammate) is not news: checked here, before this scan is written
    std::string team_leader;
    std::vector<std::string> wanted;
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        team_leader = c.team.leader;
        wanted = c.team.wanted;
    }
    for (size_t i = 0; i < found.size() && !team_leader.empty() && !wanted.empty(); ++i)
    {
        const Blockstate* block = world->GetBlock(found[i]);
        if (block == nullptr || !MatchesAny(block->GetName(), wanted) || InStoredSite(found[i]))
        {
            continue;
        }
        const std::string kind = block->GetName().substr(block->GetName().find(':') + 1);
        // Veins count once: a 16-block cell at a time
        const int cell_x = found[i].x >> 4;
        const int cell_y = found[i].y >> 4;
        const int cell_z = found[i].z >> 4;
        {
            std::lock_guard<std::mutex> lock(c.team_mutex);
            if (!c.team.announced.insert({ kind, cell_x, cell_y, cell_z }).second)
            {
                continue;
            }
        }
        bool known = false;
        {
            std::lock_guard<std::mutex> lock(memory_mutex);
            sqlite3_stmt* statement = nullptr;
            if (memory_db != nullptr && sqlite3_prepare_v2(memory_db,
                "SELECT 1 FROM places WHERE dimension = ?1 AND kind = ?2 AND x BETWEEN ?3 AND ?3 + 15 AND y BETWEEN ?4 AND ?4 + 15 AND z BETWEEN ?5 AND ?5 + 15 LIMIT 1",
                -1, &statement, nullptr) == SQLITE_OK)
            {
                sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_text(statement, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
                sqlite3_bind_int(statement, 3, cell_x * 16);
                sqlite3_bind_int(statement, 4, cell_y * 16);
                sqlite3_bind_int(statement, 5, cell_z * 16);
                known = sqlite3_step(statement) == SQLITE_ROW;
            }
            sqlite3_finalize(statement);
        }
        if (!known)
        {
            std::lock_guard<std::mutex> lock(c.team_mutex);
            c.team.finds.push_back({ kind, found[i] });
        }
    }
    // One message per scan, so the chat is not flooded
    {
        bool news = false;
        std::pair<std::string, Position> find;
        {
            std::lock_guard<std::mutex> lock(c.team_mutex);
            if (!c.team.finds.empty())
            {
                news = true;
                find = c.team.finds.front();
                c.team.finds.pop_front();
            }
        }
        if (news && team_leader == c.GetNetworkManager()->GetMyName())
        {
            AssignFind(c, team_leader, find.first, find.second);
        }
        else if (news)
        {
            c.Say("found " + find.first + " at " + std::to_string(find.second.x) + " " + std::to_string(find.second.y) + " " + std::to_string(find.second.z));
        }
    }

    std::lock_guard<std::mutex> lock(memory_mutex);
    if (memory_db == nullptr)
    {
        return;
    }
    sqlite3_exec(memory_db, "BEGIN", nullptr, nullptr, nullptr);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(memory_db, "DELETE FROM places WHERE dimension = ?1 AND x BETWEEN ?2 AND ?3 AND y BETWEEN ?4 AND ?5 AND z BETWEEN ?6 AND ?7", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, center.x - radius);
    sqlite3_bind_int(statement, 3, center.x + radius);
    sqlite3_bind_int(statement, 4, center.y - radius);
    sqlite3_bind_int(statement, 5, center.y + radius);
    sqlite3_bind_int(statement, 6, center.z - radius);
    sqlite3_bind_int(statement, 7, center.z + radius);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_prepare_v2(memory_db, "INSERT OR REPLACE INTO places (kind, dimension, x, y, z, seen) VALUES (?1, ?2, ?3, ?4, ?5, strftime('%s', 'now'))", -1, &statement, nullptr);
    for (const Position& p : found)
    {
        const Blockstate* block = world->GetBlock(p);
        if (block == nullptr)
        {
            continue;
        }
        // "minecraft:oak_log" is remembered as "oak_log"
        const std::string kind = block->GetName().substr(block->GetName().find(':') + 1);
        const auto ends = [&](const std::string& end) { return kind.size() >= end.size() && kind.compare(kind.size() - end.size(), end.size(), end) == 0; };
        if ((ends("_log") || ends("_ore")) && InStoredSite(p))
        {
            continue;
        }
        sqlite3_bind_text(statement, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, dimension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 3, p.x);
        sqlite3_bind_int(statement, 4, p.y);
        sqlite3_bind_int(statement, 5, p.z);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    // Farm animals too (not penned ones: those are kept), for hunting and food: where they were last seen
    std::vector<Ent> seen_alive = monsters;
    for (const Ent& a : FindEntities(c, "animal", radius))
    {
        if ((a.name == "cow" || a.name == "pig" || a.name == "sheep" || a.name == "chicken") && !IsPenned(c, a.pos))
        {
            seen_alive.push_back(a);
        }
    }
    for (const Ent& m : seen_alive)
    {
        const Position p = BlockPos(m.pos);
        const std::string kind = "mob:" + m.name;
        sqlite3_bind_text(statement, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, dimension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 3, p.x);
        sqlite3_bind_int(statement, 4, p.y);
        sqlite3_bind_int(statement, 5, p.z);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    // Mobs wander off and despawn: a sighting older than half an hour is forgotten
    sqlite3_exec(memory_db, "DELETE FROM places WHERE kind LIKE 'mob:%' AND seen < strftime('%s', 'now') - 1800", nullptr, nullptr, nullptr);
    sqlite3_exec(memory_db, "COMMIT", nullptr, nullptr, nullptr);

    // And the finds worth sharing, for the other bots: everything but its own furniture (beds, tables, furnaces)
    // and the monsters (called out in the chat). Chests and barrels are shared: what is in them is known once
    // a bot opens one (chest_contents)
    std::vector<std::pair<std::string, Position>> finds;
    for (const Position& p : found)
    {
        const Blockstate* block = world->GetBlock(p);
        const std::string kind = block == nullptr ? "" : block->GetName().substr(block->GetName().find(':') + 1);
        const auto ends = [&](const std::string& end) { return kind.size() >= end.size() && kind.compare(kind.size() - end.size(), end.size(), end) == 0; };
        if (!kind.empty() && !ends("_bed") && kind != "crafting_table" && kind != "furnace" && !((ends("_log") || ends("_ore")) && InStoredSite(p)))
        {
            finds.push_back({ kind, p });
        }
    }
    // Chests remembered in this box that are not there any more: their contents are forgotten
    {
        sqlite3* shared = SharedDb();
        sqlite3_stmt* statement = nullptr;
        std::vector<Position> gone;
        if (shared != nullptr && sqlite3_prepare_v2(shared, "SELECT DISTINCT x, y, z FROM chest_contents WHERE dimension = ?1"
            " AND x BETWEEN ?2 AND ?3 AND y BETWEEN ?4 AND ?5 AND z BETWEEN ?6 AND ?7", -1, &statement, nullptr) == SQLITE_OK)
        {
            sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(statement, 2, center.x - radius);
            sqlite3_bind_int(statement, 3, center.x + radius);
            sqlite3_bind_int(statement, 4, center.y - radius);
            sqlite3_bind_int(statement, 5, center.y + radius);
            sqlite3_bind_int(statement, 6, center.z - radius);
            sqlite3_bind_int(statement, 7, center.z + radius);
            while (sqlite3_step(statement) == SQLITE_ROW)
            {
                const Position p(sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1), sqlite3_column_int(statement, 2));
                const Blockstate* block = world->GetBlock(p);
                if (block != nullptr && block->GetName().find("chest") == std::string::npos && block->GetName().find("barrel") == std::string::npos)
                {
                    gone.push_back(p);
                }
            }
            sqlite3_finalize(statement);
        }
        for (const Position& p : gone)
        {
            sqlite3_prepare_v2(shared, "DELETE FROM chest_contents WHERE dimension = ?1 AND x = ?2 AND y = ?3 AND z = ?4", -1, &statement, nullptr);
            sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(statement, 2, p.x);
            sqlite3_bind_int(statement, 3, p.y);
            sqlite3_bind_int(statement, 4, p.z);
            sqlite3_step(statement);
            sqlite3_finalize(statement);
        }
    }
    for (size_t i = monsters.size(); i < seen_alive.size(); ++i)
    {
        finds.push_back({ "mob:" + seen_alive[i].name, BlockPos(seen_alive[i].pos) });
    }
    ReportFinds(c.GetNetworkManager()->GetMyName(), dimension, center, radius, finds);
}

/// @brief Tell the other bots what this scan of the box around center found: what was reported in the box before
/// and is not there any more is taken back, the rest is written, one place per kind and 16-block cell
static void ReportFinds(const std::string& by, const std::string& dimension, const Position& center, const int radius,
    const std::vector<std::pair<std::string, Position>>& finds)
{
    sqlite3* db = SharedDb();
    if (db == nullptr)
    {
        return;
    }
    sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM finds WHERE dimension = ?1 AND x BETWEEN ?2 AND ?3 AND y BETWEEN ?4 AND ?5 AND z BETWEEN ?6 AND ?7"
        " OR (kind LIKE 'mob:%' AND seen < strftime('%s', 'now') - 1800)", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, center.x - radius);
    sqlite3_bind_int(statement, 3, center.x + radius);
    sqlite3_bind_int(statement, 4, center.y - radius);
    sqlite3_bind_int(statement, 5, center.y + radius);
    sqlite3_bind_int(statement, 6, center.z - radius);
    sqlite3_bind_int(statement, 7, center.z + radius);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_prepare_v2(db, "INSERT OR REPLACE INTO finds (kind, dimension, cx, cy, cz, x, y, z, seen, by)"
        " VALUES (?1, ?2, ?3 >> 4, ?4 >> 4, ?5 >> 4, ?3, ?4, ?5, strftime('%s', 'now'), ?6)", -1, &statement, nullptr);
    std::set<std::tuple<std::string, int, int, int>> cells;
    for (const auto& [kind, p] : finds)
    {
        if (!cells.insert({ kind, p.x >> 4, p.y >> 4, p.z >> 4 }).second)
        {
            continue;
        }
        sqlite3_bind_text(statement, 1, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, dimension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 3, p.x);
        sqlite3_bind_int(statement, 4, p.y);
        sqlite3_bind_int(statement, 5, p.z);
        sqlite3_bind_text(statement, 6, by.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

/// @brief Remembered places whose kind ends with this ("iron_ore" also finds deepslate_iron_ore, "_log" any log,
/// "bed" any bed), nearest to the bot first
static std::vector<std::pair<std::string, Position>> Recall(BehaviourClient& c, std::string kind, const int max_results)
{
    sqlite3* const memory_db = MemoryOf(c);
    kind = kind.substr(kind.find(':') == std::string::npos ? 0 : kind.find(':') + 1);
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    const std::string dimension = c.GetWorld()->GetCurrentDimension();
    std::vector<std::pair<std::string, Position>> places;
    std::lock_guard<std::mutex> lock(memory_mutex);
    if (memory_db == nullptr)
    {
        return places;
    }
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(memory_db,
        "SELECT kind, x, y, z FROM places WHERE dimension = ?1 AND kind LIKE '%' || ?2"
        " ORDER BY (x - ?3) * (x - ?3) + (y - ?4) * (y - ?4) + (z - ?5) * (z - ?5) LIMIT ?6", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, static_cast<int>(me.x));
    sqlite3_bind_int(statement, 4, static_cast<int>(me.y));
    sqlite3_bind_int(statement, 5, static_cast<int>(me.z));
    sqlite3_bind_int(statement, 6, max_results);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        places.push_back({ reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)),
            Position(sqlite3_column_int(statement, 1), sqlite3_column_int(statement, 2), sqlite3_column_int(statement, 3)) });
    }
    sqlite3_finalize(statement);
    // And what the other bots reported (ReportFinds), the nearest of both
    sqlite3* shared = SharedDb();
    if (shared != nullptr && sqlite3_prepare_v2(shared,
        "SELECT kind, x, y, z FROM finds WHERE dimension = ?1 AND kind LIKE '%' || ?2"
        " ORDER BY (x - ?3) * (x - ?3) + (y - ?4) * (y - ?4) + (z - ?5) * (z - ?5) LIMIT ?6", -1, &statement, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_text(statement, 2, kind.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 3, static_cast<int>(me.x));
        sqlite3_bind_int(statement, 4, static_cast<int>(me.y));
        sqlite3_bind_int(statement, 5, static_cast<int>(me.z));
        sqlite3_bind_int(statement, 6, max_results);
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            const Position p(sqlite3_column_int(statement, 1), sqlite3_column_int(statement, 2), sqlite3_column_int(statement, 3));
            if (std::none_of(places.begin(), places.end(), [&](const auto& known) { return known.second == p; }))
            {
                places.push_back({ reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)), p });
            }
        }
        sqlite3_finalize(statement);
        const auto distance = [&](const Position& p) { return (p.x - me.x) * (p.x - me.x) + (p.y - me.y) * (p.y - me.y) + (p.z - me.z) * (p.z - me.z); };
        std::sort(places.begin(), places.end(), [&](const auto& a, const auto& b) { return distance(a.second) < distance(b.second); });
        places.resize(std::min(places.size(), static_cast<size_t>(max_results)));
    }
    return places;
}

static void Forget(const BehaviourClient& c, const Position& p)
{
    sqlite3* const memory_db = MemoryOf(c);
    std::lock_guard<std::mutex> lock(memory_mutex);
    if (memory_db == nullptr)
    {
        return;
    }
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(memory_db, "DELETE FROM places WHERE x = ?1 AND y = ?2 AND z = ?3", -1, &statement, nullptr);
    sqlite3_bind_int(statement, 1, p.x);
    sqlite3_bind_int(statement, 2, p.y);
    sqlite3_bind_int(statement, 3, p.z);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    // Gone for the other bots too
    if (sqlite3* shared = SharedDb(); shared != nullptr && sqlite3_prepare_v2(shared, "DELETE FROM finds WHERE x = ?1 AND y = ?2 AND z = ?3", -1, &statement, nullptr) == SQLITE_OK)
    {
        sqlite3_bind_int(statement, 1, p.x);
        sqlite3_bind_int(statement, 2, p.y);
        sqlite3_bind_int(statement, 3, p.z);
        sqlite3_step(statement);
        sqlite3_finalize(statement);
    }
}

/// @brief How many places of each kind are remembered, most numerous first
static Value MemorySummary(const BehaviourClient& c)
{
    sqlite3* const memory_db = MemoryOf(c);
    Value summary = Json::Object();
    std::lock_guard<std::mutex> lock(memory_mutex);
    if (memory_db == nullptr)
    {
        return summary;
    }
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(memory_db, "SELECT kind, COUNT(*) FROM places GROUP BY kind ORDER BY 2 DESC LIMIT 40", -1, &statement, nullptr);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        summary[reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))] = sqlite3_column_int(statement, 1);
    }
    sqlite3_finalize(statement);
    return summary;
}

/// @brief How many items of each kind are remembered in chests in this dimension
static Value StoredSummary(const std::string& dimension)
{
    Value summary = Json::Object();
    sqlite3* db = SharedDb();
    sqlite3_stmt* statement = nullptr;
    if (db == nullptr || sqlite3_prepare_v2(db, "SELECT item, SUM(count) FROM chest_contents"
        " WHERE dimension = ?1 AND item != '' AND count > 0 GROUP BY item", -1, &statement, nullptr) != SQLITE_OK)
    {
        return summary;
    }
    sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        std::string item = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
        if (item.rfind("minecraft:", 0) == 0)
        {
            item = item.substr(10);
        }
        summary[item] = sqlite3_column_int(statement, 1);
    }
    sqlite3_finalize(statement);
    return summary;
}

/// @brief Entities around the bot, nearest first.
/// @param type "" for everything, an entity name ("zombie"), or a group: "hostile", "animal", "player"
static std::vector<Ent> FindEntities(BehaviourClient& c, std::string type, const double radius)
{
    type = type.rfind("minecraft:", 0) == 0 ? type.substr(10) : type;
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    std::vector<Ent> found;
    {
        auto entities = c.GetEntityManager()->GetEntities();
        for (const auto& [id, e] : *entities)
        {
            if (!e || e->IsLocalPlayer())
            {
                continue;
            }
            const std::string name = e->GetName();
            const bool match = type.empty() || name == type ||
                // Not the endermen: neutral until provoked, and the worst thing to provoke (one that comes
                // right at the bot is attacking: the defend branch fights it by name)
                (type == "hostile" && e->IsMonster() && name != "enderman") ||
                (type == "animal" && e->IsAnimal()) ||
                (type == "player" && e->IsRemotePlayer());
            const Vector3<double> pos = e->GetPosition();
            const double dist = std::sqrt(me.SqrDist(pos));
            if (match && dist <= radius)
            {
                found.push_back({ id, name, pos, dist, e->IsAnimal() });
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const Ent& a, const Ent& b) { return a.dist < b.dist; });
    return found;
}

struct Crop
{
    int mature_age;
    const char* seed;
};
static const std::map<std::string, Crop> crops = {
    { "minecraft:wheat", { 7, "minecraft:wheat_seeds" } },
    { "minecraft:carrots", { 7, "minecraft:carrot" } },
    { "minecraft:potatoes", { 7, "minecraft:potato" } },
    { "minecraft:beetroots", { 3, "minecraft:beetroot_seeds" } },
    { "minecraft:nether_wart", { 3, "minecraft:nether_wart" } },
};

static std::vector<Position> RipeCrops(BehaviourClient& c, const int radius, const size_t max_results)
{
    return FindBlocks(c, [](const Blockstate& b)
        {
            const auto crop = crops.find(b.GetName());
            return crop != crops.end() && b.GetVariableValue("age") == std::to_string(crop->second.mature_age);
        }, radius, max_results);
}

/// @brief Name of a food item the bot holds, "" if none
// What the bots eat (and hand over to the hungry ones)
static const std::set<std::string> foods = {
        "minecraft:bread", "minecraft:cooked_beef", "minecraft:cooked_porkchop", "minecraft:cooked_chicken",
        "minecraft:cooked_mutton", "minecraft:cooked_cod", "minecraft:cooked_salmon", "minecraft:baked_potato",
        "minecraft:golden_carrot", "minecraft:carrot", "minecraft:apple", "minecraft:melon_slice",
        "minecraft:sweet_berries", "minecraft:cookie", "minecraft:pumpkin_pie", "minecraft:beetroot", "minecraft:potato",
        // Raw meat feeds less but is what a hunt gives (not chicken: it poisons)
        "minecraft:beef", "minecraft:porkchop", "minecraft:mutton",
};

/// @brief The most nourishing food in the pack (hunger + saturation, from the Minecraft wiki): a steak (8 + 12.8)
/// before raw beef (3 + 1.8), so what was cooked gets eaten and raw meat is kept for cooking
static std::string FindFood(BehaviourClient& c)
{
    static const std::map<std::string, double> worth = {
        { "minecraft:golden_carrot", 20.4 }, { "minecraft:cooked_beef", 20.8 }, { "minecraft:cooked_porkchop", 20.8 },
        { "minecraft:cooked_mutton", 15.6 }, { "minecraft:cooked_salmon", 15.6 }, { "minecraft:cooked_chicken", 13.2 },
        { "minecraft:pumpkin_pie", 12.8 }, { "minecraft:bread", 12.0 }, { "minecraft:baked_potato", 12.0 },
        { "minecraft:cooked_cod", 10.0 }, { "minecraft:apple", 6.4 }, { "minecraft:carrot", 6.6 }, { "minecraft:beetroot", 2.2 },
        { "minecraft:melon_slice", 3.2 }, { "minecraft:sweet_berries", 2.4 }, { "minecraft:cookie", 2.4 }, { "minecraft:potato", 1.6 },
        { "minecraft:beef", 4.8 }, { "minecraft:porkchop", 4.8 }, { "minecraft:mutton", 3.2 },
    };
    std::string best;
    double best_worth = -1.0;
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (slot.IsEmptySlot())
        {
            continue;
        }
        const std::string& item = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName();
        if (foods.count(item) != 0)
        {
            const auto it = worth.find(item);
            const double w = it == worth.end() ? 1.0 : it->second;
            if (w > best_worth)
            {
                best = item;
                best_worth = w;
            }
        }
    }
    return best;
}

// ---------------------------------------------------------------- tasks (run on the behaviour thread)

static bool EndsWith(const std::string& s, const std::string& suffix)
{
    return s.size() >= suffix.size() && s.compare(s.size() - suffix.size(), suffix.size(), suffix) == 0;
}

static int CountItem(BehaviourClient& c, const std::string& item)
{
    int count = 0;
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (!slot.IsEmptySlot() && AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() == item)
        {
            count += slot.GetItemCount();
        }
    }
    return count;
}

// Best first
static const char* tool_tiers[] = { "netherite", "diamond", "iron", "stone", "golden", "wooden" };
constexpr int stone_tier = 3;
constexpr int any_tier = 5;

/// @brief Best tool of this kind ("pickaxe", "axe", "sword", "shovel") the bot holds, "" if none is at least worst_tier
static std::string HeldTool(BehaviourClient& c, const std::string& kind, const int worst_tier = any_tier)
{
    for (int i = 0; i <= worst_tier; ++i)
    {
        const std::string item = std::string("minecraft:") + tool_tiers[i] + "_" + kind;
        if (CountItem(c, item) > 0)
        {
            return item;
        }
    }
    return "";
}

constexpr int iron_tier = 2;

/// @brief Dig a 2-high tunnel toward a block the pathfinder can't walk to (buried ore), as stairs when it is
/// lower or higher, until it is within reach. Needs a pickaxe. Stops at water, lava and bedrock.
// ponytail: only the blocks dug are checked for fluids, a lava pocket beside the tunnel can still flow in
static void LightUp(BehaviourClient& c, const Position& where, const bool on_the_surface_too = false);

/// @brief True if this spot or one of the six blocks touching it is lava: opening it would let the lava in
// Boxes around what the bots have built, kept in the shared database so every bot process, and the next
// run, knows them: nothing inside is broken, except by a build whose own plan covers the spot
static std::mutex sites_mutex;

static std::vector<Site> LoadSites()
{
    std::vector<Site> sites;
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(SharedDb(), "SELECT x1, y1, z1, x2, y2, z2, name FROM sites WHERE name NOT LIKE '%_road' ORDER BY rowid", -1, &statement, nullptr);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        sites.push_back({ Position(sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1), sqlite3_column_int(statement, 2)),
            Position(sqlite3_column_int(statement, 3), sqlite3_column_int(statement, 4), sqlite3_column_int(statement, 5)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 6)) });
    }
    sqlite3_finalize(statement);
    return sites;
}

static bool InStoredSite(const Position& p)
{
    // Asked for every block a search looks at (gathering filters with it): the boxes are read again only
    // every 5 seconds. ponytail: a box added by another bot process counts from then on
    static SiteIndex sites;
    static std::chrono::steady_clock::time_point read_at;
    std::lock_guard<std::mutex> lock(sites_mutex);
    const auto now = std::chrono::steady_clock::now();
    if (now - read_at > std::chrono::seconds(5))
    {
        sites.Reset(LoadSites());
        read_at = now;
    }
    return sites.Contains(p);
}

static bool NearStoredSite(const Position& p, const int margin)
{
    std::lock_guard<std::mutex> lock(sites_mutex);
    for (const Site& site : LoadSites())
    {
        if (p.x >= site.min.x - margin && p.x <= site.max.x + margin &&
            p.z >= site.min.z - margin && p.z <= site.max.z + margin)
        {
            return true;
        }
    }
    return false;
}

/// @return false if a stored box already covers it
static bool AddSite(const Position& a, const Position& b, const std::string& name)
{
    const Position low(std::min(a.x, b.x), std::min(a.y, b.y), std::min(a.z, b.z));
    const Position high(std::max(a.x, b.x), std::max(a.y, b.y), std::max(a.z, b.z));
    std::lock_guard<std::mutex> lock(sites_mutex);
    for (const Site& site : LoadSites())
    {
        if (site.Contains(low) && site.Contains(high))
        {
            return false;
        }
    }
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(SharedDb(), "INSERT INTO sites (x1, y1, z1, x2, y2, z2, name) VALUES (?1, ?2, ?3, ?4, ?5, ?6, ?7)", -1, &statement, nullptr);
    sqlite3_bind_int(statement, 1, low.x);
    sqlite3_bind_int(statement, 2, low.y);
    sqlite3_bind_int(statement, 3, low.z);
    sqlite3_bind_int(statement, 4, high.x);
    sqlite3_bind_int(statement, 5, high.y);
    sqlite3_bind_int(statement, 6, high.z);
    sqlite3_bind_text(statement, 7, name.empty() ? "build" : name.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    return true;
}

/// @brief Every dig of the bots goes through here: nothing inside a stored construction box is broken, unless
/// the bot is building and its own blueprint covers that spot (levelling, a vault, a block put the wrong way)
static Status SafeDig(BehaviourClient& c, const Position& p, const bool send_swing = false, const PlayerDiggingFace face = PlayerDiggingFace::Up,
    const bool allow_pathfinding = true)
{
    McpBot* bot = dynamic_cast<McpBot*>(&c);
    if (InStoredSite(p) && (bot == nullptr || !bot->InBlueprint(p)))
    {
        return Status::Failure;
    }
    // With a pickaxe in hand: the stone one for common blocks (saves the iron one's durability),
    // the best one for ores worth it (gold, diamond, redstone, emerald need iron) and obsidian
    const ProtocolCraft::Slot hand = c.GetInventoryManager()->GetHotbarSelected();
    const Blockstate* block = c.GetWorld()->GetBlock(p);
    if (!hand.IsEmptySlot() && block != nullptr && EndsWith(AssetsManager::getInstance().GetItem(hand.GetItemId())->GetName(), "_pickaxe"))
    {
        const std::string& name = block->GetName();
        const bool valuable = (name.find("_ore") != std::string::npos && !EndsWith(name, "coal_ore")) ||
            EndsWith(name, ":ancient_debris") || EndsWith(name, "obsidian");
        const std::string pickaxe = valuable ? HeldTool(c, "pickaxe")
            : CountItem(c, "minecraft:stone_pickaxe") > 0 ? "minecraft:stone_pickaxe" : "";
        if (!pickaxe.empty())
        {
            SetItemInHand(c, pickaxe);
        }
    }
    return Botcraft::Dig(c, p, send_swing, face, allow_pathfinding);
}

static bool LavaNear(const std::shared_ptr<World>& world, const Position& p)
{
    for (const Position& d : { Position(0, 0, 0), Position(1, 0, 0), Position(-1, 0, 0), Position(0, 1, 0), Position(0, -1, 0), Position(0, 0, 1), Position(0, 0, -1) })
    {
        const Blockstate* block = world->GetBlock(p + d);
        if (block != nullptr && block->IsLava())
        {
            return true;
        }
    }
    return false;
}

static void Count(BehaviourClient& c, const char* what, const int n = 1);

static Status PlaceCheckedBlock(BehaviourClient& c, const std::string& item, const Position& p,
    std::optional<PlayerDiggingFace> face = std::nullopt, const bool wait_confirmation = false,
    const bool allow_midair_placing = false, const bool allow_pathfinding = true)
{
    const bool chest = item == "minecraft:chest" || item == "minecraft:trapped_chest";
    const auto world = c.GetWorld();
    const Blockstate* before = chest ? world->GetBlock(p) : nullptr;
    const bool already_there = before != nullptr && before->GetName() == item;
    const Status result = Botcraft::PlaceBlock(c, item, p, face, wait_confirmation || chest, allow_midair_placing, allow_pathfinding);
    if (!chest || already_there || result != Status::Success) return result;
    const Blockstate* placed = world->GetBlock(p);
    if (placed == nullptr || placed->GetName() != item) return result;
    const std::string facing = placed->GetVariableValue("facing");
    int mismatched = 0;
    for (const Position& d : { Position(1, 0, 0), Position(-1, 0, 0), Position(0, 0, 1), Position(0, 0, -1) })
    {
        const Blockstate* neighbor = world->GetBlock(p + d);
        if (neighbor != nullptr && (neighbor->GetName() == "minecraft:chest" || neighbor->GetName() == "minecraft:trapped_chest") &&
            Construction::MisalignedChests(facing, neighbor->GetVariableValue("facing"))) ++mismatched;
    }
    Count(c, "misaligned_chest_pairs", mismatched);
    Count(c, "construction_penalty", 10 * mismatched);
    return result;
}

static void PenalizeNarrowMine(BehaviourClient& c, const Position& feet, const Position& direction)
{
    const auto world = c.GetWorld();
    const Position across(std::abs(direction.z), 0, std::abs(direction.x));
    bool lanes[3] = {};
    for (int lane = -1; lane <= 1; ++lane)
    {
        const Position p = feet + across * lane;
        const Blockstate* a = world->GetBlock(p);
        const Blockstate* b = world->GetBlock(p + Position(0, 1, 0));
        if (a == nullptr || b == nullptr) return;
        lanes[lane + 1] = a->IsAir() && b->IsAir();
    }
    if (Construction::NarrowMine(lanes[1], lanes[0], lanes[2]))
    {
        Count(c, "narrow_mine_slices");
        Count(c, "construction_penalty", 10);
    }
}

/// @param allow_hands Dig with bare hands if the bot has no pickaxe: very slow and nothing drops, only to get out of a hole
static Status TunnelToward(BehaviourClient& c, const Position& target, const int max_steps, const bool allow_hands = false)
{
    const std::shared_ptr<World> world = c.GetWorld();
    const std::string pickaxe = HeldTool(c, "pickaxe");
    if (pickaxe.empty() && !allow_hands)
    {
        return Status::Failure;
    }
    if (!pickaxe.empty())
    {
        SetItemInHand(c, pickaxe);
    }
    const auto is_floor = [&](const Position& p) { const Blockstate* b = world->GetBlock(p); return b != nullptr && b->IsSolid(); };
    McpBot* const builder = dynamic_cast<McpBot*>(&c);
    const auto in_site = [&](const Position& p) { return InStoredSite(p) || (builder != nullptr && builder->InSite(p)); };
    {
        const Position start = BlockPos(c.GetLocalPlayer()->GetPosition());
        if (in_site(start) || (target.x - start.x) * (target.x - start.x) + (target.z - start.z) * (target.z - start.z) > 6 * 6)
        {
            GoTo(c, Position(target.x, start.y, target.z), 3);
        }
    }
    for (int step = 0; step < max_steps; ++step)
    {
        const Position me = BlockPos(c.GetLocalPlayer()->GetPosition());
        const int dx = target.x - me.x;
        const int dy = target.y - me.y;
        const int dz = target.z - me.z;
        if (dx * dx + dy * dy + dz * dz <= 3 * 3)
        {
            return Status::Success;
        }
        // One step along the longest horizontal distance (any side when right above or below, the stairs then zigzag)
        Position side = std::abs(dx) >= std::abs(dz) ? Position(dx >= 0 ? 1 : -1, 0, 0) : Position(0, 0, dz > 0 ? 1 : -1);
        if (in_site(me + side))
        {
            for (const Position& d : { Position(1, 0, 0), Position(-1, 0, 0), Position(0, 0, 1), Position(0, 0, -1) })
            {
                if (!in_site(me + d))
                {
                    side = d;
                    break;
                }
            }
        }
        // Up, level or down as the target asks, but only onto a spot that has a floor (stairs can't cross a void)
        int slope = dy < 0 ? -1 : dy > 0 ? 1 : 0;
        if (!is_floor(me + side + Position(0, slope - 1, 0)))
        {
            slope = is_floor(me + side + Position(0, -1, 0)) ? 0 : is_floor(me + side + Position(0, -2, 0)) ? -1 : is_floor(me + side) ? 1 : 2;
        }
        if (slope == 2)
        {
            return Status::Failure;
        }
        const Position next = me + side + Position(0, slope, 0);
        // Room for the body at the next spot, plus headroom to jump when going up
        std::vector<Position> cells = { next + Position(0, 1, 0), next };
        if (slope < 0)
        {
            cells.insert(cells.begin(), me + side + Position(0, 1, 0));
        }
        else if (slope > 0)
        {
            cells.insert(cells.begin(), me + Position(0, 2, 0));
        }
        bool changed = false;
        for (const Position& cell : cells)
        {
            const Blockstate* block = world->GetBlock(cell);
            // Not into water, bedrock, or anything with lava against it: that is how a miner dies.
            // Nor through what the team is building
            if (block == nullptr || block->IsFluid() || block->GetHardness() < 0.0f || LavaNear(world, cell) ||
                (!block->IsAir() && builder != nullptr && builder->IsPlanned(cell)))
            {
                return Status::Failure;
            }
            if (!block->IsAir())
            {
                if (SafeDig(c, cell, true, PlayerDiggingFace::Up, false) == Status::Failure)
                {
                    if (changed) PenalizeNarrowMine(c, next, side);
                    return Status::Failure;
                }
                changed = true;
            }
        }
        if (changed) PenalizeNarrowMine(c, next, side);
        const Blockstate* floor = world->GetBlock(next + Position(0, -1, 0));
        if (floor == nullptr || floor->IsFluid() || GoTo(c, next) == Status::Failure)
        {
            return Status::Failure;
        }
        // A torch every 6 blocks of tunnel, on the spot just left: monsters only spawn in the dark
        if (step % 6 == 5)
        {
            LightUp(c, me);
        }
    }
    return Status::Failure;
}

/// @brief Rise one block where the bot stands: jump, and put a block under its feet
static bool PillarUp(BehaviourClient& c, const std::string& item)
{
    const std::shared_ptr<World> world = c.GetWorld();
    const std::shared_ptr<LocalPlayer> player = c.GetLocalPlayer();
    const double start = player->GetPosition().y;
    const Position feet = BlockPos(player->GetPosition());
    // Headroom for the jump (leaves, usually)
    const Blockstate* above = world->GetBlock(feet + Position(0, 2, 0));
    if (above != nullptr && !above->IsAir() && SafeDig(c, feet + Position(0, 2, 0), true, PlayerDiggingFace::Down, false) == Status::Failure)
    {
        return false;
    }
    // Hold jump until the feet are clear of the block to place
    for (int i = 0; i < 60 && player->GetPosition().y < start + 1.05; ++i)
    {
        player->SetInputsJump(true);
        c.Yield();
    }
    if (player->GetPosition().y < start + 1.0)
    {
        return false;
    }
    PlaceCheckedBlock(c, item, feet, std::nullopt, false, false, false);
    Utilities::YieldForCondition([&]() { return player->GetOnGround(); }, c, 1500);
    return player->GetPosition().y > start + 0.9;
}

/// @brief Cut a whole tree: every log connected to this one, bottom first (from the ground, from inside the
/// trunk, then standing on a pillar it builds for the top), then pick up the wood and replant a sapling of
/// the same kind if the bot carries one.
/// @return How many logs were cut
static int FellTree(BehaviourClient& c, const Position& first)
{
    const std::shared_ptr<World> world = c.GetWorld();
    const Blockstate* first_block = world->GetBlock(first);
    if (first_block == nullptr)
    {
        return 0;
    }
    const std::string kind = first_block->GetName();
    // The logs touching each other, diagonals included (branches of big trees)
    std::set<Position> seen = { first };
    std::vector<Position> tree = { first };
    for (size_t i = 0; i < tree.size() && tree.size() < 160; ++i)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            for (int dy = -1; dy <= 1; ++dy)
            {
                for (int dz = -1; dz <= 1; ++dz)
                {
                    const Position next = tree[i] + Position(dx, dy, dz);
                    const Blockstate* block = world->GetBlock(next);
                    if (block != nullptr && block->GetName() == kind && seen.insert(next).second)
                    {
                        tree.push_back(next);
                    }
                }
            }
        }
    }
    std::sort(tree.begin(), tree.end(), [](const Position& a, const Position& b) { return a.y < b.y; });
    const Position base = tree.front();
    const WorkingInPlace at_the_tree(c);
    const std::string axe = HeldTool(c, "axe");
    if (!axe.empty())
    {
        SetItemInHand(c, axe);
    }
    int felled = 0;
    std::vector<Position> left;
    for (const Position& log : tree)
    {
        // From the ground while it can be reached, then from inside the trunk, looking up
        if (SafeDig(c, log, true) == Status::Success ||
            (GoTo(c, base) == Status::Success && SafeDig(c, log, true, PlayerDiggingFace::Down, false) == Status::Success))
        {
            ++felled;
        }
        else if (felled == 0)
        {
            // Not even the first log: the tree is out of reach, its other logs are not tried one by one (two
            // path searches each, every one of them failing)
            return 0;
        }
        else
        {
            left.push_back(log);
        }
    }
    // The top of a tall tree: pillar up inside the trunk, with the wood just cut or any common block,
    // cutting what comes within reach at each level, then dig the pillar back down
    int height = 0;
    if (!left.empty() && felled > 0 && GoTo(c, base) == Status::Success)
    {
        // (giant jungle trees reach 30 blocks)
        while (!left.empty() && height < 32)
        {
            const size_t before = left.size();
            left.erase(std::remove_if(left.begin(), left.end(), [&](const Position& log)
                {
                    const bool cut = SafeDig(c, log, true, PlayerDiggingFace::Down, false) == Status::Success;
                    felled += cut;
                    return cut;
                }), left.end());
            if (left.size() < before)
            {
                continue;
            }
            std::string block;
            for (const std::string& candidate : { std::string("minecraft:dirt"), std::string("minecraft:cobblestone"), kind })
            {
                if (block.empty() && CountItem(c, candidate) > 0)
                {
                    block = candidate;
                }
            }
            if (block.empty() || !PillarUp(c, block))
            {
                break;
            }
            ++height;
        }
        for (; height > 0; --height)
        {
            // Back down, taking the pillar along: here the block under the feet is exactly the one to dig
            const Position under = BlockPos(c.GetLocalPlayer()->GetPosition()) + Position(0, -1, 0);
            if (SafeDig(c, under, true, PlayerDiggingFace::Up, false) == Status::Failure)
            {
                break;
            }
            Utilities::YieldForCondition([&]() { return c.GetLocalPlayer()->GetOnGround(); }, c, 1500);
        }
    }
    // What the pillar in the trunk could not reach (the branches of acacias and big oaks): each once more from
    // the ground, walking up to it, now that the trunk is out of the way. The whole tree, no floating logs
    for (const Position& log : left)
    {
        const Blockstate* still = world->GetBlock(log);
        if (still != nullptr && still->GetName() == kind && SafeDig(c, log, true) == Status::Success)
        {
            ++felled;
        }
    }
    if (felled > 0)
    {
        Wait(c, 600);
        GoTo(c, base, 1);
        Wait(c, 300);
        if (NearStoredSite(base, 48))
        {
            for (int dx = -3; dx <= 3; ++dx)
            {
                for (int dz = -3; dz <= 3; ++dz)
                {
                    const Position s = base + Position(dx, 0, dz);
                    const Blockstate* b = world->GetBlock(s);
                    if (b != nullptr && EndsWith(b->GetName(), "_sapling"))
                    {
                        SafeDig(c, s, true, PlayerDiggingFace::Up, false);
                    }
                }
            }
        }
        else
        {
            // oak_log grows from oak_sapling
            std::string sapling = kind.substr(0, kind.size() - 4) + "_sapling";
            const size_t stripped = sapling.find("stripped_");
            if (stripped != std::string::npos)
            {
                sapling.erase(stripped, 9);
            }
            const Blockstate* ground = world->GetBlock(base + Position(0, -1, 0));
            if (CountItem(c, sapling) > 0 && ground != nullptr && (ground->GetName() == "minecraft:dirt" || ground->GetName() == "minecraft:grass_block" || ground->GetName() == "minecraft:podzol"))
            {
                PlaceBlock(c, sapling, base, PlayerDiggingFace::Up, true);
            }
        }
    }
    return felled;
}

/// @brief Mine the nearest matching blocks and pick up the drops until count is reached. A log means its whole tree
/// @param tunnel Dig a way to the ones that can't be walked to (needs a pickaxe)
/// @brief One more of something done, in the bot's stats
static void Count(BehaviourClient& c, const char* what, const int n)
{
    McpBot* bot = dynamic_cast<McpBot*>(&c);
    if (bot != nullptr && n > 0)
    {
        std::lock_guard<std::mutex> lock(bot->stats_mutex);
        bot->stats[what] += n;
    }
}

static Status IdleTask(McpBot& c)
{
    c.SetState("idle");
    const bool enabled = c.autopilot.load();
    const auto start = std::chrono::steady_clock::now();
    const auto record = [&]()
    {
        Count(c, "idle_seconds", c.idle_time.Record(std::chrono::steady_clock::now() - start, enabled && c.autopilot.load()));
    };
    try { Wait(c, 1000); }
    catch (...) { record(); throw; } // Tree swaps interrupt a wait by unwinding it.
    record();
    return Status::Success;
}

static Status CollectTask(BehaviourClient& c, const std::function<bool(const Blockstate&)>& match, const int count, const int radius, std::atomic<int>& done, const bool tunnel = false)
{
    std::set<Position> failed;
    // Material is taken from outside the building site: its walls are made of what the bots are after
    McpBot* const builder = dynamic_cast<McpBot*>(&c);
    // What failed lately counts as failed here too: the callers ask for one block a tick, and a tree out of
    // reach was walked around again and again, for good
    const auto now = std::chrono::steady_clock::now();
    if (builder != nullptr)
    {
        for (auto it = builder->unreachable.begin(); it != builder->unreachable.end();)
        {
            if (now - it->second > std::chrono::minutes(10))
            {
                it = builder->unreachable.erase(it);
            }
            else
            {
                failed.insert(it->first);
                ++it;
            }
        }
    }
    int failures = 0;
    const auto give_up_on = [&](const Position& p)
        {
            failed.insert(p);
            if (builder != nullptr)
            {
                builder->unreachable[p] = std::chrono::steady_clock::now();
            }
        };
    const std::function<bool(const Position&)> outside_the_site = [builder](const Position& p) { return !InStoredSite(p) && (builder == nullptr || !builder->InSite(p)); };
    while (done < count)
    {
        std::vector<Position> found = FindBlocks(c, match, radius, failed.size() + 8, outside_the_site);
        auto target = std::find_if(found.begin(), found.end(), [&](const Position& p)
            {
                const Blockstate* b = c.GetWorld()->GetBlock(p);
                return failed.count(p) == 0 && b != nullptr && EndsWith(b->GetName(), "_log") && NearStoredSite(p, 48);
            });
        if (target == found.end())
        {
            target = std::find_if(found.begin(), found.end(), [&](const Position& p) { return failed.count(p) == 0; });
        }
        if (target == found.end() || failures >= 8)
        {
            return done > 0 ? Status::Success : Status::Failure;
        }
        if (tunnel && LavaNear(c.GetWorld(), *target))
        {
            // Ore with lava against it stays where it is
            give_up_on(*target);
            ++failures;
            continue;
        }
        const Blockstate* wanted = c.GetWorld()->GetBlock(*target);
        if (wanted != nullptr && EndsWith(wanted->GetName(), "_log"))
        {
            const int felled = FellTree(c, *target);
            if (felled == 0)
            {
                // The whole tree, not this log alone: its other logs are just as far out of reach (one at a
                // time, a bot walked around the same tree for minutes)
                const std::shared_ptr<World> w = c.GetWorld();
                for (int dx = -3; dx <= 3; ++dx)
                {
                    for (int dz = -3; dz <= 3; ++dz)
                    {
                        for (int dy = -12; dy <= 20; ++dy)
                        {
                            const Position p = *target + Position(dx, dy, dz);
                            const Blockstate* b = w->GetBlock(p);
                            if (b != nullptr && EndsWith(b->GetName(), "_log"))
                            {
                                give_up_on(p);
                            }
                        }
                    }
                }
                give_up_on(*target);
                ++failures;
            }
            done += felled;
            Count(c, "collected", felled);
            continue;
        }
        if (SafeDig(c, *target, true) == Status::Failure &&
            (!tunnel || TunnelToward(c, *target, 48) == Status::Failure || SafeDig(c, *target, true, PlayerDiggingFace::Up, false) == Status::Failure))
        {
            give_up_on(*target);
            ++failures;
            continue;
        }
        ++done;
        Count(c, "collected");
        // Wherever the bot works, underground or in the open at night: keep the place lit
        LightUp(c, BlockPos(c.GetLocalPlayer()->GetPosition()), true);
        // Drops can't be picked up for half a second, then walk into the hole to get them
        Wait(c, 600);
        if (GoTo(c, *target) == Status::Failure)
        {
            GoTo(c, *target, 1);
        }
        Wait(c, 200);
    }
    return Status::Success;
}

static Status CollectTask(BehaviourClient& c, const std::string& id, const int count, const int radius, std::atomic<int>& done)
{
    return CollectTask(c, [&](const Blockstate& b) { return b.GetName() == id; }, count, radius, done);
}

/// @brief Walk a long way toward x z in hops the pathfinder can plan (it only knows the loaded chunks)
static Status TravelTask(BehaviourClient& c, const double x, const double z)
{
    int stalled = 0;
    while (true)
    {
        const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
        const double dx = x - me.x;
        const double dz = z - me.z;
        const double distance = std::sqrt(dx * dx + dz * dz);
        if (distance <= 12.0)
        {
            return Status::Success;
        }
        const double hop = std::min(distance, 32.0);
        GoTo(c, BlockPos(Vector3<double>(me.x + dx / distance * hop, me.y, me.z + dz / distance * hop)), 6);
        Vector3<double> now = c.GetLocalPlayer()->GetPosition();
        if ((now.x - me.x) * (now.x - me.x) + (now.z - me.z) * (now.z - me.z) < 4.0 * 4.0)
        {
            // No walkable way (in a pit, a cave, behind a wall): dig one, climbing if there is rock overhead
            const Position feet = BlockPos(now);
            bool underground = false;
            for (int up = 2; up <= 24 && !underground; ++up)
            {
                const Blockstate* above = c.GetWorld()->GetBlock(feet + Position(0, up, 0));
                underground = above != nullptr && above->IsSolid();
            }
            TunnelToward(c, Position(static_cast<int>(now.x + dx / distance * 12.0), feet.y + (underground ? 10 : 0), static_cast<int>(now.z + dz / distance * 12.0)), 16);
            now = c.GetLocalPlayer()->GetPosition();
        }
        // Three hops in a row that barely moved, even by digging: the way is blocked (sea, lava, no pickaxe...)
        stalled = (now.x - me.x) * (now.x - me.x) + (now.z - me.z) * (now.z - me.z) < 4.0 * 4.0 ? stalled + 1 : 0;
        if (stalled >= 3)
        {
            return Status::Failure;
        }
    }
}

/// @brief True if an animal is kept by someone: a fence, a gate, a wall or iron bars within 8 blocks of it.
/// Those are left alone, whatever the bot is short of
static bool IsPenned(BehaviourClient& c, const Vector3<double>& animal)
{
    const std::shared_ptr<World> world = c.GetWorld();
    const Position at(static_cast<int>(std::floor(animal.x)), static_cast<int>(std::floor(animal.y)), static_cast<int>(std::floor(animal.z)));
    for (int dy = -2; dy <= 3; ++dy)
    {
        for (int dx = -8; dx <= 8; ++dx)
        {
            for (int dz = -8; dz <= 8; ++dz)
            {
                const Blockstate* block = world->GetBlock(at + Position(dx, dy, dz));
                if (block == nullptr || block->IsAir())
                {
                    continue;
                }
                const std::string& name = block->GetName();
                if (EndsWith(name, "_fence") || EndsWith(name, "_fence_gate") || EndsWith(name, "_wall") || EndsWith(name, ":iron_bars"))
                {
                    return true;
                }
            }
        }
    }
    return false;
}

/// @brief The animals of this kind around that the bot may hunt: the wild ones, not the penned ones
static std::vector<Ent> WildAnimals(BehaviourClient& c, const std::string& type, const double radius)
{
    std::vector<Ent> wild;
    for (const Ent& e : FindEntities(c, type, radius))
    {
        if (!e.animal || !IsPenned(c, e.pos))
        {
            wild.push_back(e);
        }
    }
    return wild;
}

/// @brief How many blocks of wool, of any colour, the bot carries
static int CountWool(BehaviourClient& c)
{
    int count = 0;
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (!slot.IsEmptySlot() && EndsWith(AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName(), "_wool"))
        {
            count += slot.GetItemCount();
        }
    }
    return count;
}

/// @brief Shear sheep: wool (1 to 3 blocks each) and the sheep lives, and grows it back by grazing.
/// Needs shears. Penned sheep too: it does them no harm.
/// @param sheared How many sheep gave wool
static Status ShearTask(BehaviourClient& c, const int count, const double radius, std::atomic<int>& sheared)
{
    if (CountItem(c, "minecraft:shears") == 0)
    {
        return Status::Failure;
    }
    // Each sheep once: one that gives nothing was shorn already
    std::set<int> tried;
    while (sheared < count)
    {
        const std::vector<Ent> sheep = FindEntities(c, "sheep", radius);
        const auto next = std::find_if(sheep.begin(), sheep.end(), [&](const Ent& e) { return tried.count(e.id) == 0; });
        if (next == sheep.end())
        {
            break;
        }
        tried.insert(next->id);
        const int before = CountWool(c);
        if (SetItemInHand(c, "minecraft:shears") == Status::Failure || InteractEntity(c, next->id, Hand::Main, true) == Status::Failure)
        {
            continue;
        }
        // The wool drops around the sheep
        Wait(c, 500);
        GoTo(c, BlockPos(next->pos), 1);
        Wait(c, 500);
        sheared += CountWool(c) > before;
    }
    return sheared > 0 ? Status::Success : Status::Failure;
}

/// @brief Milk cows: an empty bucket used on each cow in turn, a milk bucket back for each. The cow is unharmed
/// @param milked How many buckets were filled
static Status MilkTask(BehaviourClient& c, const int count, const double radius, std::atomic<int>& milked)
{
    std::set<int> tried;
    while (milked < count && CountItem(c, "minecraft:bucket") > 0)
    {
        const std::vector<Ent> cows = FindEntities(c, "cow", radius);
        const auto next = std::find_if(cows.begin(), cows.end(), [&](const Ent& e) { return tried.count(e.id) == 0; });
        if (next == cows.end())
        {
            break;
        }
        tried.insert(next->id);
        const int before = CountItem(c, "minecraft:milk_bucket");
        if (SetItemInHand(c, "minecraft:bucket") == Status::Failure || InteractEntity(c, next->id, Hand::Main, true) == Status::Failure)
        {
            continue;
        }
        Wait(c, 300);
        milked += CountItem(c, "minecraft:milk_bucket") > before;
    }
    return milked > 0 ? Status::Success : Status::Failure;
}

/// @brief What an animal follows when a player holds it
static std::string LureFood(const std::string& type)
{
    static const std::map<std::string, std::string> foods = {
        { "cow", "minecraft:wheat" }, { "mooshroom", "minecraft:wheat" }, { "sheep", "minecraft:wheat" }, { "goat", "minecraft:wheat" },
        { "pig", "minecraft:carrot" }, { "rabbit", "minecraft:carrot" }, { "chicken", "minecraft:wheat_seeds" },
        { "horse", "minecraft:golden_carrot" }, { "donkey", "minecraft:golden_carrot" }, { "llama", "minecraft:hay_block" },
        { "wolf", "minecraft:bone" }, { "cat", "minecraft:cod" }, { "bee", "minecraft:dandelion" }, { "fox", "minecraft:sweet_berries" },
    };
    const auto food = foods.find(type);
    return food == foods.end() ? "" : food->second;
}

/// @brief Lead animals somewhere: with their food in hand, go up to them, then walk to the destination in
/// short hops, waiting for them to catch up and going back for them when they lose interest.
/// @param arrived How many of them are within 8 blocks of the destination at the end
static Status LureTask(BehaviourClient& c, const std::string& type, const std::string& food, const Position& destination, std::atomic<int>& arrived)
{
    const std::shared_ptr<LocalPlayer> player = c.GetLocalPlayer();
    const auto followers = [&](const double within)
        {
            return static_cast<int>(FindEntities(c, type, within).size());
        };
    const auto to_destination = [&]()
        {
            const Vector3<double> me = player->GetPosition();
            return std::sqrt((me.x - destination.x) * (me.x - destination.x) + (me.z - destination.z) * (me.z - destination.z));
        };
    int lost = 0;
    for (int hop = 0; hop < 200 && to_destination() > 3.0; ++hop)
    {
        // Animals follow the food from 10 blocks away at most: none that close means going to get them
        if (followers(8.0) == 0)
        {
            const std::vector<Ent> around = FindEntities(c, type, 48.0);
            if (around.empty() || ++lost > 6)
            {
                break;
            }
            GoTo(c, BlockPos(around.front().pos), 3, 0, 0, true, false);
        }
        SetItemInHand(c, food);
        // A few blocks toward the destination, at a walk, then a pause for them to catch up
        const Vector3<double> me = player->GetPosition();
        const double left = to_destination();
        const double step = std::min(left, 5.0);
        const Position next(static_cast<int>(std::floor(me.x + (destination.x - me.x) / left * step)), static_cast<int>(std::floor(me.y)),
            static_cast<int>(std::floor(me.z + (destination.z - me.z) / left * step)));
        if (GoTo(c, left <= 5.0 ? destination : next, 1, 0, 0, true, false, 0.7f) == Status::Failure)
        {
            break;
        }
        Wait(c, 1500);
    }
    // Hold still a moment with the food out, so the stragglers come in
    SetItemInHand(c, food);
    Wait(c, 3000);
    arrived = 0;
    for (const Ent& e : FindEntities(c, type, 64.0))
    {
        arrived += (e.pos.x - destination.x) * (e.pos.x - destination.x) + (e.pos.z - destination.z) * (e.pos.z - destination.z) <= 8.0 * 8.0;
    }
    return arrived > 0 ? Status::Success : Status::Failure;
}

/// @brief Hunt the nearest entities of this type/group until count are dead (never a penned animal)
/// @param player_name Only the player of this name (type is then ignored)
static Status KillTask(BehaviourClient& c, const std::string& type, const int count, const double radius, std::atomic<int>& done, const std::string& player_name = "")
{
    const std::shared_ptr<EntityManager> entity_manager = c.GetEntityManager();
    const std::shared_ptr<LocalPlayer> player = c.GetLocalPlayer();
    // The sword, else an axe (it hits harder but recovers slower): the swing waits for a full recovery
    std::string weapon = HeldTool(c, "sword");
    const bool axe = weapon.empty() && !(weapon = HeldTool(c, "axe")).empty();
    if (!weapon.empty())
    {
        SetItemInHand(c, weapon);
    }
    const int cooldown_ms = axe ? 1000 : 650;
    std::set<int> skipped;
    while (done < count)
    {
        std::vector<Ent> found;
        if (player_name.empty())
        {
            found = WildAnimals(c, type, radius);
        }
        else
        {
            for (const Ent& e : FindEntities(c, "player", radius))
            {
                const std::shared_ptr<Entity> entity = entity_manager->GetEntity(e.id);
                if (entity && c.GetPlayerName(entity->GetUUID()) == player_name)
                {
                    found.push_back(e);
                }
            }
        }
        const auto target = std::find_if(found.begin(), found.end(), [&](const Ent& e) { return skipped.count(e.id) == 0; });
        if (target == found.end())
        {
            return Status::Failure;
        }
        const int id = target->id;
        Vector3<double> last_seen = target->pos;
        bool dead = false;
        // 60 swings at most per target, then give up on it
        for (int swing = 0; swing < 60 && !dead; ++swing)
        {
            const std::shared_ptr<Entity> entity = entity_manager->GetEntity(id);
            if (!entity)
            {
                dead = true;
                break;
            }
            const Vector3<double> pos = entity->GetPosition();
            last_seen = pos;
            if (player->GetPosition().SqrDist(pos) > 3.0 * 3.0 && GoTo(c, BlockPos(pos), 2) == Status::Failure)
            {
                break;
            }
            LookAt(c, pos + Vector3<double>(0.0, 1.0, 0.0));
            std::shared_ptr<ProtocolCraft::ServerboundAttackPacket> attack = std::make_shared<ProtocolCraft::ServerboundAttackPacket>();
            attack->SetEntityId(id);
            c.GetNetworkManager()->Send(attack);
            // A creeper: hit and back away at once, out of its blast (it goes off within 3 blocks), as the
            // knockback pushes it; then come back for the next hit. Its blast would take walls down too
            if (entity->GetName() == "creeper")
            {
                const Vector3<double> me = player->GetPosition();
                Vector3<double> away(me.x - pos.x, 0.0, me.z - pos.z);
                const double length = std::sqrt(away.x * away.x + away.z * away.z);
                if (length > 0.01)
                {
                    GoTo(c, BlockPos(me + away * (5.0 / length)), 1);
                }
            }
            // Weapon cooldown
            Wait(c, cooldown_ms);
        }
        if (dead)
        {
            ++done;
            Count(c, "killed");
            // Walk where it died to pick up the drops
            Wait(c, 600);
            GoTo(c, BlockPos(last_seen), 1);
        }
        else
        {
            skipped.insert(id);
        }
    }
    return Status::Success;
}

/// @brief Harvest every ripe crop in range, pick up the drops and replant
static int CraftOne(BehaviourClient& c, const std::string& item, std::string& error);

static Status FarmTask(BehaviourClient& c, const int radius, const bool replant, std::atomic<int>& harvested, std::atomic<int>& replanted)
{
    const std::shared_ptr<World> world = c.GetWorld();
    for (const Position& p : RipeCrops(c, radius, 512))
    {
        const Blockstate* block = world->GetBlock(p);
        const auto crop = block == nullptr ? crops.end() : crops.find(block->GetName());
        if (crop == crops.end() || Dig(c, p, true) == Status::Failure)
        {
            continue;
        }
        ++harvested;
        Count(c, "harvested");
        // Walk onto the plot to pick up the drops, they include what we replant with
        Wait(c, 300);
        GoTo(c, p, 1);
        if (replant && PlaceBlock(c, crop->second.seed, p, std::nullopt, true) == Status::Success)
        {
            ++replanted;
        }
    }
    // Bone meal on what is still growing: 2 to 5 growth stages at once, whatever the light and the water
    // (a bone makes 3; skeletons drop them). 16 a round at most
    const std::string meal = "minecraft:bone_meal";
    std::string error;
    if (CountItem(c, meal) == 0 && CountItem(c, "minecraft:bone") > 0)
    {
        CraftOne(c, meal, error);
    }
    if (CountItem(c, meal) > 0)
    {
        const auto growing = [](const Blockstate& b)
            {
                const auto crop = crops.find(b.GetName());
                if (crop == crops.end() || crop->first == "minecraft:nether_wart")
                {
                    return false;
                }
                const std::string age = b.GetVariableValue("age");
                return !age.empty() && std::atoi(age.c_str()) < crop->second.mature_age;
            };
        for (const Position& p : FindBlocks(c, growing, radius, 16))
        {
            if (CountItem(c, meal) == 0 || SetItemInHand(c, meal) == Status::Failure)
            {
                break;
            }
            if (c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(p.x + 0.5, p.y, p.z + 0.5)) > 4.0 * 4.0 && GoTo(c, p, 3) == Status::Failure)
            {
                continue;
            }
            InteractWithBlock(c, p, PlayerDiggingFace::Up);
            Wait(c, 250);
        }
    }
    return Status::Success;
}

// ---------------------------------------------------------------- storage: one chest per resource

// --chests: store what was collected or harvested right away
static bool auto_store = false;
// Item id -> chest that holds it. Shared by all the bots and saved in the shared database so restarts reuse the same chests
static std::map<std::string, Position> chests;
static std::mutex chests_mutex;

static void LoadChests()
{
    sqlite3_stmt* statement = nullptr;
    if (sqlite3_prepare_v2(SharedDb(), "SELECT item, x, y, z FROM chests", -1, &statement, nullptr) != SQLITE_OK)
    {
        return;
    }
    chests.clear();
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        chests[reinterpret_cast<const char*>(sqlite3_column_text(statement, 0))] =
            Position(sqlite3_column_int(statement, 1), sqlite3_column_int(statement, 2), sqlite3_column_int(statement, 3));
    }
    sqlite3_finalize(statement);
}

/// @brief Tools, weapons and armor: things the bot keeps on itself
static bool IsGear(const std::string& item)
{
    for (const char* kind : { "_pickaxe", "_axe", "_sword", "_shovel", "_hoe", "_helmet", "_chestplate", "_leggings", "_boots", ":shield" })
    {
        if (EndsWith(item, kind))
        {
            return true;
        }
    }
    return false;
}

static void SaveChests()
{
    sqlite3* db = SharedDb();
    sqlite3_exec(db, "BEGIN IMMEDIATE; DELETE FROM chests", nullptr, nullptr, nullptr);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "INSERT INTO chests (item, x, y, z) VALUES (?1, ?2, ?3, ?4)", -1, &statement, nullptr);
    for (const auto& [name, position] : chests)
    {
        sqlite3_bind_text(statement, 1, name.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, position.x);
        sqlite3_bind_int(statement, 3, position.y);
        sqlite3_bind_int(statement, 4, position.z);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

/// @brief Which kind of thing an item is, for sorting a vault: food, weapons, armor, equipment (tools),
/// valuables (ores, ingots, gems, coal), building (blocks), materials (everything one crafts with)
static std::string CategoryOf(const std::string& item)
{
    static const std::set<std::string> foods = {
        "bread", "apple", "golden_apple", "carrot", "golden_carrot", "potato", "baked_potato", "beetroot", "melon_slice", "sweet_berries",
        "cookie", "pumpkin_pie", "cake", "beef", "cooked_beef", "porkchop", "cooked_porkchop", "mutton", "cooked_mutton", "chicken",
        "cooked_chicken", "rabbit", "cooked_rabbit", "cod", "cooked_cod", "salmon", "cooked_salmon", "wheat", "egg", "milk_bucket",
        "mushroom_stew", "beetroot_soup", "rabbit_stew", "honey_bottle", "dried_kelp", "rotten_flesh", "wheat_seeds", "beetroot_seeds",
    };
    const std::string name = item.substr(item.find(':') + 1);
    const auto ends = [&](const char* suffix) { return EndsWith(name, suffix); };
    const auto has = [&](const char* part) { return name.find(part) != std::string::npos; };
    if (foods.count(name) != 0) { return "food"; }
    if (ends("_sword") || name == "bow" || name == "crossbow" || name == "trident" || name == "mace" || ends("arrow")) { return "weapons"; }
    if (ends("_helmet") || ends("_chestplate") || ends("_leggings") || ends("_boots") || name == "shield" || name == "elytra") { return "armor"; }
    if (ends("_pickaxe") || ends("_axe") || ends("_shovel") || ends("_hoe") || name == "shears" || name == "flint_and_steel" || ends("bucket") ||
        name == "fishing_rod" || name == "compass" || name == "clock" || name == "torch" || name == "spyglass") { return "equipment"; }
    if (ends("_ore") || has("raw_") || ends("_ingot") || ends("_nugget") || name == "diamond" || name == "emerald" || name == "coal" || name == "charcoal" ||
        name == "redstone" || name == "lapis_lazuli" || name == "quartz" || name == "amethyst_shard" || has("netherite") ||
        (ends("_block") && (has("iron") || has("gold") || has("diamond") || has("emerald") || has("coal") || has("redstone") || has("lapis") || has("copper")))) { return "valuables"; }
    if (name == "stone" || has("cobble") || has("dirt") || ends("_planks") || ends("_log") || ends("_wood") || has("brick") || has("sand") || name == "gravel" ||
        has("glass") || ends("_stairs") || ends("_slab") || ends("_wall") || ends("_fence") || ends("_door") || has("granite") || has("diorite") || has("andesite") ||
        has("deepslate") || has("terracotta") || has("concrete") || name == "grass_block" || name == "clay" || has("tuff") || has("basalt")) { return "building"; }
    return "materials";
}

// Vault chests found full by this bot, and when: passed over for ten minutes (somebody may empty them)
static std::map<Position, std::chrono::steady_clock::time_point> full_chests;

/// @brief The vault chest for this item's category, if a vault was declared and is within 96 blocks.
/// A vault has one chest per category on each of its floors ("category:food", "category:food#2"...):
/// the first that this bot has not found full
static bool CategoryChest(BehaviourClient& c, const std::string& item, Position& chest)
{
    std::lock_guard<std::mutex> lock(chests_mutex);
    LoadChests();
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    for (int floor = 1; floor <= 16; ++floor)
    {
        const auto known = chests.find("category:" + CategoryOf(item) + (floor == 1 ? "" : "#" + std::to_string(floor)));
        if (known == chests.end())
        {
            break;
        }
        const auto full = full_chests.find(known->second);
        if (full != full_chests.end() && now - full->second < std::chrono::minutes(10))
        {
            continue;
        }
        if (c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(known->second.x, known->second.y, known->second.z)) > 96.0 * 96.0)
        {
            return false;
        }
        // In sight and not a chest (any more): not a vault to store in (a vault declared for a castle whose
        // ground was levelled again sent every bot to an empty spot)
        const Blockstate* block = c.GetWorld()->GetBlock(known->second);
        if (block != nullptr && !EndsWith(block->GetName(), "chest"))
        {
            return false;
        }
        chest = known->second;
        return true;
    }
    return false;
}

/// @brief A vault chest took nothing more: remember it, and if that was the last floor's, say so in the
/// team's chest list. The castle plugin sees it and has a floor dug underneath
static void ChestIsFull(const Position& chest, const std::string& item)
{
    std::lock_guard<std::mutex> lock(chests_mutex);
    LoadChests();
    full_chests[chest] = std::chrono::steady_clock::now();
    bool vault_chest = false;
    bool another = false;
    for (int floor = 1; floor <= 16; ++floor)
    {
        const auto known = chests.find("category:" + CategoryOf(item) + (floor == 1 ? "" : "#" + std::to_string(floor)));
        if (known == chests.end())
        {
            break;
        }
        vault_chest = vault_chest || known->second == chest;
        const auto full = full_chests.find(known->second);
        another = another || full == full_chests.end();
    }
    if (vault_chest && !another)
    {
        chests["vault:full"] = chest;
        SaveChests();
    }
}

static bool IsChest(const std::shared_ptr<World>& world, const Position& p)
{
    const Blockstate* block = world->GetBlock(p);
    return block != nullptr && block->GetName() == "minecraft:chest";
}

/// @brief Place a block item (chest, crafting table, furnace) on a free spot next to the bot.
/// A chest always goes down as a double chest, two side by side (the bot needs two)
static bool PlaceNearby(BehaviourClient& c, const std::string& item, Position& placed)
{
    const std::shared_ptr<World> world = c.GetWorld();
    // Free spot: air with a solid floor (not leaves: they rot away) and air above (or a chest lid can't open), not
    // where the bot stands, and not touching a chest (two chests would merge into a double chest and mix two resources)
    const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
    const auto is_air = [&](const Position& p) { const Blockstate* b = world->GetBlock(p); return b != nullptr && b->IsAir(); };
    const auto free = [&](const Position& p)
        {
            const Blockstate* floor = world->GetBlock(p + Position(0, -1, 0));
            return is_air(p) && floor != nullptr && floor->IsSolid() && !EndsWith(floor->GetName(), "_leaves") && is_air(p + Position(0, 1, 0)) &&
                !(p.x == feet.x && p.z == feet.z) && !IsChest(world, p + Position(1, 0, 0)) && !IsChest(world, p + Position(-1, 0, 0)) &&
                !IsChest(world, p + Position(0, 0, 1)) && !IsChest(world, p + Position(0, 0, -1));
        };
    // A chest faces the bot that places it, and two side by side facing the same way join: the second one beside
    // the first across the bot's line of sight, both seen from the same side
    const bool pair = item == "minecraft:chest";
    const auto side = [&](const Position& p) { const int dx = p.x - feet.x, dz = p.z - feet.z; return std::abs(dx) == std::abs(dz) ? 0 : std::abs(dx) > std::abs(dz) ? (dx > 0 ? 1 : 2) : (dz > 0 ? 3 : 4); };
    int refused = 0;
    McpBot* const bot = dynamic_cast<McpBot*>(&c);
    if (bot != nullptr && std::chrono::steady_clock::now() < bot->refused_until &&
        std::abs(feet.x - bot->refused_at.x) <= 16 && std::abs(feet.z - bot->refused_at.z) <= 16)
    {
        return false;
    }
    for (const Position& p : FindBlocks(c, [](const Blockstate& b) { return b.IsAir(); }, 4, 729))
    {
        if (!free(p))
        {
            continue;
        }
        std::optional<Position> second;
        if (pair)
        {
            const Position across = side(p) <= 2 ? Position(0, 0, 1) : Position(1, 0, 0);
            for (const Position& q : { p + across, p - across })
            {
                if (!second && side(p) != 0 && side(q) == side(p) && free(q))
                {
                    second = q;
                }
            }
            if (!second)
            {
                continue;
            }
        }
        if (PlaceCheckedBlock(c, item, p, std::nullopt, true) == Status::Success)
        {
            placed = p;
            // ponytail: the second chest failing leaves a single one, still a chest to store in
            if (second && CountItem(c, item) > 0)
            {
                PlaceCheckedBlock(c, item, *second, std::nullopt, true);
            }
            return true;
        }
        // Three spots side by side refused by the server: the area is protected (around the world spawn,
        // for bots that are not operators), and every other spot would be waited for in vain
        if (++refused >= 3)
        {
            // And not again around here for 5 minutes (a store tries a chest for each kind of item it carries)
            if (bot != nullptr)
            {
                bot->refused_at = feet;
                bot->refused_until = std::chrono::steady_clock::now() + std::chrono::minutes(5);
            }
            return false;
        }
    }
    return false;
}

/// @brief First empty slot of an open container window in [begin, end), -1 if none.
/// In a container window the container's own slots come first, then the player's 27 storage and 9 hotbar slots
static short EmptySlot(const std::shared_ptr<Window>& window, const short begin, const short end)
{
    for (short index = begin; index < end; ++index)
    {
        if (window->GetSlot(index).IsEmptySlot())
        {
            return index;
        }
    }
    return -1;
}

/// @brief Tell every bot what is in this chest (the shared chest_contents table), as the open window shows it
static void RememberChest(BehaviourClient& c, const Position& p, const std::shared_ptr<Window>& window)
{
    sqlite3* db = SharedDb();
    if (db == nullptr || window == nullptr)
    {
        return;
    }
    std::map<std::string, int> contents = { { "", 0 } };
    for (const auto& [index, slot] : window->GetSlots())
    {
        if (index < window->GetFirstPlayerInventorySlot() && !slot.IsEmptySlot())
        {
            contents[AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName()] += slot.GetItemCount();
        }
    }
    const std::string dimension = c.GetWorld()->GetCurrentDimension();
    const std::string by = c.GetNetworkManager()->GetMyName();
    sqlite3_exec(db, "BEGIN IMMEDIATE", nullptr, nullptr, nullptr);
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(db, "DELETE FROM chest_contents WHERE dimension = ?1 AND x = ?2 AND y = ?3 AND z = ?4", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 2, p.x);
    sqlite3_bind_int(statement, 3, p.y);
    sqlite3_bind_int(statement, 4, p.z);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    sqlite3_prepare_v2(db, "INSERT INTO chest_contents (dimension, x, y, z, item, count, seen, by)"
        " VALUES (?1, ?2, ?3, ?4, ?5, ?6, strftime('%s', 'now'), ?7)", -1, &statement, nullptr);
    for (const auto& [item, count] : contents)
    {
        sqlite3_bind_text(statement, 1, dimension.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, p.x);
        sqlite3_bind_int(statement, 3, p.y);
        sqlite3_bind_int(statement, 4, p.z);
        sqlite3_bind_text(statement, 5, item.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 6, count);
        sqlite3_bind_text(statement, 7, by.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_step(statement);
        sqlite3_reset(statement);
    }
    sqlite3_finalize(statement);
    sqlite3_exec(db, "COMMIT", nullptr, nullptr, nullptr);
}

/// @brief Close a chest, telling the other bots what is left in it
static void CloseChest(BehaviourClient& c, const short container_id, const Position& p)
{
    RememberChest(c, p, c.GetInventoryManager()->GetWindow(container_id));
    CloseContainer(c, container_id);
}

/// @brief The nearest chest within 96 blocks that some bot saw holding this item
static bool ChestHolding(BehaviourClient& c, const std::string& item, Position& chest)
{
    sqlite3* db = SharedDb();
    sqlite3_stmt* statement = nullptr;
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    if (db == nullptr || sqlite3_prepare_v2(db, "SELECT x, y, z FROM chest_contents WHERE dimension = ?1 AND item = ?2 AND count > 0"
        " ORDER BY (x - ?3) * (x - ?3) + (y - ?4) * (y - ?4) + (z - ?5) * (z - ?5) LIMIT 1", -1, &statement, nullptr) != SQLITE_OK)
    {
        return false;
    }
    sqlite3_bind_text(statement, 1, c.GetWorld()->GetCurrentDimension().c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, item.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_int(statement, 3, static_cast<int>(me.x));
    sqlite3_bind_int(statement, 4, static_cast<int>(me.y));
    sqlite3_bind_int(statement, 5, static_cast<int>(me.z));
    const bool found = sqlite3_step(statement) == SQLITE_ROW;
    if (found)
    {
        chest = Position(sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1), sqlite3_column_int(statement, 2));
    }
    sqlite3_finalize(statement);
    return found && me.SqrDist(Vector3<double>(chest.x, chest.y, chest.z)) <= 96.0 * 96.0;
}

/// @brief A chest within 16 blocks with a free slot, so that no new one is placed while one has room: the nearest
/// few looked into (not the vault's: those keep to their category), the ones found full passed over for ten minutes
static bool RoomyChest(BehaviourClient& c, Position& chest)
{
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    int looked = 0;
    for (const Position& p : FindBlocks(c, [](const Blockstate& b) { return b.GetName() == "minecraft:chest"; }, 16, 8))
    {
        {
            std::lock_guard<std::mutex> lock(chests_mutex);
            const auto full = full_chests.find(p);
            const bool vault = std::any_of(chests.begin(), chests.end(), [&](const auto& k) { return k.second == p && k.first.rfind("category:", 0) == 0; });
            if (vault || (full != full_chests.end() && now - full->second < std::chrono::minutes(10)))
            {
                continue;
            }
        }
        if (++looked > 3)
        {
            break;
        }
        bool room = false;
        if (GoTo(c, p, 4, 0, 1) == Status::Success && OpenContainer(c, p) == Status::Success)
        {
            const short id = c.GetInventoryManager()->GetFirstOpenedWindowId();
            const std::shared_ptr<Window> window = c.GetInventoryManager()->GetWindow(id);
            room = EmptySlot(window, 0, window->GetFirstPlayerInventorySlot()) >= 0;
            CloseChest(c, id, p);
        }
        if (room)
        {
            chest = p;
            return true;
        }
        std::lock_guard<std::mutex> lock(chests_mutex);
        full_chests[p] = now;
    }
    return false;
}

/// @brief The chest of this item: the known one if it still exists and had room the last time, otherwise one
/// nearby with room, otherwise a new double chest placed next to the bot
static bool ChestFor(BehaviourClient& c, const std::string& item, Position& chest, std::string& error)
{
    const std::shared_ptr<World> world = c.GetWorld();
    {
        std::lock_guard<std::mutex> lock(chests_mutex);
        // Other bots run in their own process and add chests too
        LoadChests();
        const auto known = chests.find(item);
        const auto full = known == chests.end() ? full_chests.end() : full_chests.find(known->second);
        if (known != chests.end() && IsChest(world, known->second) &&
            (full == full_chests.end() || std::chrono::steady_clock::now() - full->second >= std::chrono::minutes(10)))
        {
            chest = known->second;
            return true;
        }
    }
    if (RoomyChest(c, chest))
    {
        std::lock_guard<std::mutex> lock(chests_mutex);
        chests[item] = chest;
        SaveChests();
        return true;
    }
    std::string craft_error;
    for (int i = CountItem(c, "minecraft:chest"); i < 2; ++i)
    {
        CraftOne(c, "minecraft:chest", craft_error);
    }
    if (CountItem(c, "minecraft:chest") < 2)
    {
        error = "not two chest items in the inventory to create storage for " + item + " (craft some: 8 planks in a ring)";
        return false;
    }
    if (!PlaceNearby(c, "minecraft:chest", chest))
    {
        error = "no free spot within 4 blocks to place a chest for " + item;
        return false;
    }
    std::lock_guard<std::mutex> lock(chests_mutex);
    chests[item] = chest;
    SaveChests();
    return true;
}

/// @brief Move a whole stack to an empty slot of the open container: pick it up, put it down.
/// (A shift-click would do it in one, but Botcraft's inventory does not know that click and the error it
/// raises stops the bot's behaviour for good.)
static bool MoveStack(BehaviourClient& c, const short container_id, const short from, const short to)
{
    return to >= 0 && SwapItemsInContainer(c, container_id, from, to) == Status::Success;
}

/// @brief Put the inventory in chests, one chest per item type, creating the missing chests.
/// @param everything false: keep the hotbar (tools, food, seeds) and the chest items. true: empty the whole inventory
static Status StoreTask(BehaviourClient& c, const bool everything, std::atomic<int>& stored, std::string& error)
{
    const std::shared_ptr<InventoryManager> inventory_manager = c.GetInventoryManager();
    // Chest items go last so they are still there to create the other chests
    std::vector<std::string> items;
    bool has_chests = false;
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (slot.IsEmptySlot() || index < Window::INVENTORY_STORAGE_START || (!everything && index >= Window::INVENTORY_HOTBAR_START))
        {
            continue;
        }
        const std::string& item = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName();
        if (!everything && IsGear(item))
        {
            continue;
        }
        if (item == "minecraft:chest")
        {
            has_chests = true;
        }
        else if (std::find(items.begin(), items.end(), item) == items.end())
        {
            items.push_back(item);
        }
    }
    if (everything && has_chests)
    {
        items.push_back("minecraft:chest");
    }

    Status result = Status::Success;
    for (const std::string& item : items)
    {
        Position chest;
        bool found = CategoryChest(c, item, chest) || ChestFor(c, item, chest, error);
        bool open = found && GoTo(c, chest, 4, 0, 1) == Status::Success && OpenContainer(c, chest) == Status::Success;
        if (found && !open && item != "minecraft:chest")
        {
            // Its chest is out of reach (far away, behind the castle site): one here with room or a new double
            // chest, that is the item's chest from now on, for the bots working around here
            {
                std::lock_guard<std::mutex> lock(chests_mutex);
                full_chests[chest] = std::chrono::steady_clock::now();
                chests.erase(item);
            }
            if (ChestFor(c, item, chest, error))
            {
                open = GoTo(c, chest, 4, 0, 1) == Status::Success && OpenContainer(c, chest) == Status::Success;
            }
        }
        if (!open)
        {
            if (error.empty())
            {
                error = "could not reach or open the chest of " + item;
            }
            result = Status::Failure;
            continue;
        }
        const short container_id = inventory_manager->GetFirstOpenedWindowId();
        const std::shared_ptr<Window> window = inventory_manager->GetWindow(container_id);
        // In a container window the player's 27 storage slots then 9 hotbar slots come after the container's own
        // ponytail: the offhand and armor slots are not reachable from a chest window and stay on the bot
        const short first = window->GetFirstPlayerInventorySlot();
        for (const auto& [index, slot] : window->GetSlots())
        {
            if (index < first || slot.IsEmptySlot() || (!everything && index >= first + 27) ||
                AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() != item)
            {
                continue;
            }
            // First onto the chest's stacks of the same item, each filled up to a full stack (a click on a stack of
            // the same item tops it up, the rest comes back to the bot's slot)
            const int stack = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetStackSize();
            for (short to = 0; to < first && stack > 1 && !window->GetSlot(index).IsEmptySlot(); ++to)
            {
                const ProtocolCraft::Slot there = window->GetSlot(to);
                const int had = window->GetSlot(index).GetItemCount();
                if (!there.IsEmptySlot() && there.GetItemId() == slot.GetItemId() && there.GetItemCount() < stack && MoveStack(c, container_id, index, to))
                {
                    const int now = window->GetSlot(index).IsEmptySlot() ? 0 : window->GetSlot(index).GetItemCount();
                    stored += had - now;
                    Count(c, "stored", had - now);
                }
            }
            if (window->GetSlot(index).IsEmptySlot())
            {
                continue;
            }
            // The rest, into the chest's next free slot. Full chest: the rest stays on the bot for now
            // (the next floor of the vault takes it at the next round)
            const short free_slot = EmptySlot(window, 0, first);
            if (free_slot < 0)
            {
                ChestIsFull(chest, item);
                break;
            }
            const int rest = window->GetSlot(index).GetItemCount();
            if (MoveStack(c, container_id, index, free_slot))
            {
                stored += rest;
                Count(c, "stored", rest);
            }
        }
        CloseChest(c, container_id, chest);
    }
    return result;
}

// ---------------------------------------------------------------- gear: the bot equips itself

// Progression, one small action per tree tick so eating and fighting can still interrupt it:
//   wooden tools (logs -> planks -> crafting table -> sticks -> pickaxe, axe, sword, shovel)
//   stone tools  (mine stone with the wooden pickaxe)
//   leather armor (hunt cows), worn as soon as each piece is crafted
//   shield (iron ore -> furnace -> ingot), held in the off hand
// A stage whose resource can't be found, or that keeps failing, is skipped with a note.

using Grid = std::array<std::array<std::string, 3>, 3>;

/// @brief An item of the inventory whose id ends with suffix and that the bot has at least min_count of, "" if none
static std::string ItemLike(BehaviourClient& c, const std::string& suffix, const int min_count = 1)
{
    std::map<std::string, int> counts;
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (slot.IsEmptySlot())
        {
            continue;
        }
        const std::string& item = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName();
        if (EndsWith(item, suffix))
        {
            counts[item] += slot.GetItemCount();
        }
    }
    for (const auto& [item, count] : counts)
    {
        if (count >= min_count)
        {
            return item;
        }
    }
    return "";
}

/// @brief Get within reach of a block to use (crafting table, furnace, chest): on foot, else by digging a way
/// (the ground around a base gets full of pits), by hand if need be
static Status ReachBlock(BehaviourClient& c, const Position& p)
{
    // From the eyes to the block: a table 5 blocks overhead is "near" for the pathfinder and out of reach for the hand
    const auto within_reach = [&]()
        {
            const Vector3<double> eyes = c.GetLocalPlayer()->GetPosition() + Vector3<double>(0.0, 1.6, 0.0);
            return eyes.SqrDist(Vector3<double>(p.x + 0.5, p.y + 0.5, p.z + 0.5)) <= 4.0 * 4.0;
        };
    return (GoTo(c, p, 4, 0, 1) == Status::Success && within_reach()) || TunnelToward(c, p, 32, true) == Status::Success ? Status::Success : Status::Failure;
}

static int CraftOne(BehaviourClient& c, const std::string& item, std::string& error);

/// @brief Craft one recipe, in the inventory grid if it fits in 2x2, at the nearest crafting table otherwise
/// @brief Anything but a shield in the off hand goes back into the inventory: the crafting windows don't
/// show that slot, and a bot with its sticks there failed every craft while counting 12 of them
static void ClearOffhand(BehaviourClient& c)
{
    std::string item;
    short free_slot = -1;
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (index == Window::INVENTORY_OFFHAND_INDEX && !slot.IsEmptySlot())
        {
            item = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName();
        }
        if (free_slot < 0 && index >= Window::INVENTORY_STORAGE_START && index < Window::INVENTORY_OFFHAND_INDEX && slot.IsEmptySlot())
        {
            free_slot = index;
        }
    }
    if (!item.empty() && item != "minecraft:shield" && item != "minecraft:totem_of_undying" && free_slot >= 0)
    {
        MoveStack(c, Window::PLAYER_INVENTORY_INDEX, Window::INVENTORY_OFFHAND_INDEX, free_slot);
    }
    // And what a craft that failed halfway left in the 2x2 grid: back into the inventory, where it counts
    const std::shared_ptr<Window> inventory = c.GetInventoryManager()->GetPlayerInventory();
    for (short grid = Window::INVENTORY_CRAFTING_INPUT_START; inventory && grid < Window::INVENTORY_ARMOR_START; ++grid)
    {
        if (inventory->GetSlot(grid).IsEmptySlot())
        {
            continue;
        }
        short to = -1;
        for (const auto& [index, slot] : PlayerSlots(c))
        {
            if (index >= Window::INVENTORY_STORAGE_START && index < Window::INVENTORY_OFFHAND_INDEX && slot.IsEmptySlot())
            {
                to = index;
                break;
            }
        }
        MoveStack(c, Window::PLAYER_INVENTORY_INDEX, grid, to);
    }
}

/// @brief Fewer than 3 free slots: junk thrown away until there are (dirt, gravel, stone kinds nobody uses,
/// rotten flesh..., and cobblestone beyond 3 stacks). A bot full to the brim, its chests out of reach, had
/// every plank and table it crafted thrown out, and never made its tools again
static void MakeRoom(BehaviourClient& c)
{
    static const std::set<std::string> junk = { "minecraft:dirt", "minecraft:gravel", "minecraft:diorite", "minecraft:andesite",
        "minecraft:granite", "minecraft:tuff", "minecraft:rotten_flesh", "minecraft:poisonous_potato", "minecraft:spider_eye",
        "minecraft:snowball", "minecraft:netherrack", "minecraft:cobbled_deepslate", "minecraft:calcite", "minecraft:dripstone_block",
        "minecraft:pointed_dripstone", "minecraft:moss_carpet", "minecraft:short_grass", "minecraft:wheat_seeds" };
    const auto free_slots = [&]()
        {
            int n = 0;
            for (const auto& [index, slot] : PlayerSlots(c))
            {
                n += index >= Window::INVENTORY_STORAGE_START && index < Window::INVENTORY_OFFHAND_INDEX && slot.IsEmptySlot();
            }
            return n;
        };
    if (free_slots() >= 3)
    {
        return;
    }
    // Thrown toward open air, then the bot steps the other way: in a tunnel the junk bounced back off the wall
    // and was picked up again at once
    const std::shared_ptr<World> world = c.GetWorld();
    const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
    Position away(1, 0, 0);
    int best_air = -1;
    for (const Position& d : { Position(1, 0, 0), Position(-1, 0, 0), Position(0, 0, 1), Position(0, 0, -1) })
    {
        int air = 0;
        for (int k = 1; k <= 4; ++k)
        {
            const Blockstate* b = world->GetBlock(feet + d * k + Position(0, 1, 0));
            if (b == nullptr || !b->IsAir())
            {
                break;
            }
            ++air;
        }
        if (air > best_air)
        {
            best_air = air;
            away = d;
        }
    }
    LookAt(c, Vector3<double>(feet.x + 0.5 + away.x * 4, feet.y + 2.0, feet.z + 0.5 + away.z * 4));
    const Position step_back = feet - away * 3;
    const int free_before = free_slots();
    int thrown = 0;
    int cobblestone_kept = 0;
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (index < Window::INVENTORY_STORAGE_START || index >= Window::INVENTORY_OFFHAND_INDEX || slot.IsEmptySlot())
        {
            continue;
        }
        const std::string item = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName();
        const bool spare_cobblestone = item == "minecraft:cobblestone" && ++cobblestone_kept > 3;
        if (junk.count(item) != 0 || spare_cobblestone)
        {
            // (the last argument is how many to keep)
            thrown += DropItemsFromContainer(c, Window::PLAYER_INVENTORY_INDEX, index, 0) == Status::Success;
            if (free_slots() >= 3)
            {
                break;
            }
        }
    }
    GoTo(c, step_back, 1);
    LOG_INFO("Making room: " << thrown << " stacks thrown away, free slots " << free_before << " -> " << free_slots());
}

static Status CraftAt(BehaviourClient& c, const Grid& grid, std::string& error)
{
    ClearOffhand(c);
    MakeRoom(c);
    int min_x = 3, max_x = -1, min_y = 3, max_y = -1;
    for (int y = 0; y < 3; ++y)
    {
        for (int x = 0; x < 3; ++x)
        {
            if (!grid[y][x].empty())
            {
                min_x = std::min(min_x, x);
                max_x = std::max(max_x, x);
                min_y = std::min(min_y, y);
                max_y = std::max(max_y, y);
            }
        }
    }
    const bool needs_table = max_x - min_x >= 2 || max_y - min_y >= 2;
    if (needs_table)
    {
        const std::vector<Position> tables = FindBlocks(c, [](const Blockstate& b) { return b.GetName() == "minecraft:crafting_table"; }, 32, 1);
        if (tables.empty())
        {
            error = "this recipe needs a crafting_table and none is within 32 blocks: craft one and place it first";
            return Status::Failure;
        }
        if (ReachBlock(c, tables[0]) == Status::Failure || OpenContainer(c, tables[0]) == Status::Failure)
        {
            // That one is no use (perched somewhere, walled in): a table of the bot's own, right here
            std::string ignored;
            Position own;
            if (CountItem(c, "minecraft:crafting_table") == 0)
            {
                CraftOne(c, "minecraft:crafting_table", ignored);
            }
            if (CountItem(c, "minecraft:crafting_table") == 0 || !PlaceNearby(c, "minecraft:crafting_table", own) ||
                ReachBlock(c, own) == Status::Failure || OpenContainer(c, own) == Status::Failure)
            {
                error = "could not reach or open a crafting table, nor place a new one (4 planks make one)";
                return Status::Failure;
            }
        }
    }
    const Status result = CraftNamed(c, grid, true);
    if (needs_table)
    {
        CloseContainer(c);
    }
    if (result == Status::Failure)
    {
        error = "craft failed: check the recipe shape and that the inventory holds every ingredient";
    }
    return result;
}

/// @brief Walk 24 blocks away to look for something, each time in a new direction.
/// @return false once the bot has explored enough times without finding: time to give up
static bool Explore(McpBot& c, const std::string& what)
{
    constexpr int max_explorations = 8;
    if (c.explorations >= max_explorations)
    {
        c.explorations = 0;
        return false;
    }
    c.SetNote("no " + what + " within 32 blocks, exploring to find some (" + std::to_string(c.explorations + 1) + "/" + std::to_string(max_explorations) + ")");
    // Golden angle: successive directions never line up
    const double angle = 2.4 * c.explorations++;
    const Vector3<double> start = c.GetLocalPlayer()->GetPosition();
    const Vector3<double> target = start + Vector3<double>(24.0 * std::cos(angle), 0.0, 24.0 * std::sin(angle));
    if (GoTo(c, BlockPos(target), 8) == Status::Failure && c.GetLocalPlayer()->GetPosition().SqrDist(start) < 4.0)
    {
        // Nowhere to walk: the bot is shut in (a pit, a cave). Dig that way, upward if there is rock overhead
        bool underground = false;
        for (int up = 2; up <= 40 && !underground; ++up)
        {
            const Blockstate* above = c.GetWorld()->GetBlock(BlockPos(start) + Position(0, up, 0));
            underground = above != nullptr && above->IsSolid();
        }
        TunnelToward(c, BlockPos(target) + Position(0, underground ? 12 : 0, 0), 16, true);
    }
    return true;
}

/// @brief Walk to the nearest remembered place of this kind, if there is one out of sight but not too far.
/// @return true if the bot walked (or tried to): look around again before exploring
static bool GoToRemembered(McpBot& c, const std::string& kind)
{
    const std::vector<std::pair<std::string, Position>> places = Recall(c, kind, 8);
    for (const auto& [found_kind, p] : places)
    {
        const double sqr_dist = c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(p.x, p.y, p.z));
        if (sqr_dist <= 16.0 * 16.0 || ((EndsWith(found_kind, "_log") || EndsWith(found_kind, "_ore")) && InStoredSite(p)))
        {
            Forget(c, p);
            continue;
        }
        if (sqr_dist > 200.0 * 200.0)
        {
            return false;
        }
        c.SetNote("going back to a remembered " + found_kind + " at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z));
        if (GoTo(c, p, 12) == Status::Failure && TunnelToward(c, p, 40, true) == Status::Failure &&
            c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(p.x, p.y, p.z)) > 12.0 * 12.0)
        {
            // Neither on foot nor by digging: don't try again
            Forget(c, p);
        }
        return true;
    }
    return false;
}

static void SkipStage(McpBot& c, const std::string& stage, const std::string& why)
{
    c.SkipGear(stage, why);
    c.gear_failures = 0;
    c.explorations = 0;
    c.SetNote("skipped " + stage + ": " + why);
    LOG_WARNING("Gear: skipped " << stage << ": " << why);
}

/// @brief "iron_ore, minecraft:gold_ore" -> { "iron_ore", "gold_ore" }
static std::vector<std::string> SplitSpec(const std::string& spec)
{
    std::vector<std::string> items;
    size_t start = 0;
    while (start <= spec.size())
    {
        const size_t end = std::min(spec.find(',', start), spec.size());
        std::string item = spec.substr(start, end - start);
        item.erase(std::remove(item.begin(), item.end(), ' '), item.end());
        if (item.rfind("minecraft:", 0) == 0)
        {
            item = item.substr(10);
        }
        if (!item.empty())
        {
            items.push_back(item);
        }
        start = end + 1;
    }
    return items;
}

/// @brief True if a block is one of the wanted ones. An ore also matches its deepslate and nether variants
static bool MatchesAny(const std::string& block_name, const std::vector<std::string>& items)
{
    return std::any_of(items.begin(), items.end(), [&](const std::string& item)
        {
            return block_name == "minecraft:" + item || (EndsWith(item, "_ore") && EndsWith(block_name, "_" + item));
        });
}

static bool NeedsIronPickaxe(const std::string& item)
{
    return item.find("gold") != std::string::npos || item.find("diamond") != std::string::npos ||
        item.find("redstone") != std::string::npos || item.find("emerald") != std::string::npos;
}

/// @brief The client is sent every block of the chunks around it, ore inside the rock included. So rather
/// than dig blind, look through everything loaded within 96 blocks, in the layers where the wanted ores
/// are, and remember the nearest ones that have no lava against them. (This does what guessing ore from
/// the world seed would, without redoing the game's terrain generation.)
/// @return How many places were added to the memory
static int LookThroughRock(McpBot& c, const std::vector<std::string>& items, const int level)
{
    const std::shared_ptr<World> world = c.GetWorld();
    const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
    constexpr int reach = 96;
    std::vector<std::pair<double, Position>> found;
    for (int y = std::max(world->GetMinY() + 1, std::min(level, feet.y) - 12); y <= std::max(level, feet.y) + 12; ++y)
    {
        for (int x = feet.x - reach; x <= feet.x + reach; ++x)
        {
            for (int z = feet.z - reach; z <= feet.z + reach; ++z)
            {
                const Position p(x, y, z);
                const Blockstate* block = world->GetBlock(p);
                if (block != nullptr && !block->IsAir() && MatchesAny(block->GetName(), items) && !LavaNear(world, p))
                {
                    found.push_back({ (x - feet.x) * (x - feet.x) + (y - feet.y) * (y - feet.y) + (z - feet.z) * (z - feet.z), p });
                }
            }
        }
    }
    std::sort(found.begin(), found.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    const size_t kept = std::min<size_t>(found.size(), 200);
    for (size_t i = 0; i < kept; ++i)
    {
        const Blockstate* block = world->GetBlock(found[i].second);
        if (block != nullptr)
        {
            RememberPlace(c, block->GetName().substr(block->GetName().find(':') + 1), found[i].second);
        }
    }
    return static_cast<int>(kept);
}

// Where bots started digging down for ore, from the surface: the entrances of the team's mines.
// One per 24 blocks at most; kept in the shared database so every bot process, and the next run, knows them
static std::mutex mines_mutex;

struct Mine
{
    Position entrance;
    // What the bot was after when it opened it: "iron_ore,coal_ore"
    std::string resources;
};

static std::vector<Mine> LoadMines()
{
    std::vector<Mine> mines;
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(SharedDb(), "SELECT x, y, z, resources FROM mines ORDER BY rowid", -1, &statement, nullptr);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        mines.push_back({ Position(sqlite3_column_int(statement, 0), sqlite3_column_int(statement, 1), sqlite3_column_int(statement, 2)),
            reinterpret_cast<const char*>(sqlite3_column_text(statement, 3)) });
    }
    sqlite3_finalize(statement);
    return mines;
}

static bool AddMine(const Position& entrance, const std::string& resources)
{
    std::lock_guard<std::mutex> lock(mines_mutex);
    for (const Mine& mine : LoadMines())
    {
        if (std::abs(mine.entrance.x - entrance.x) <= 24 && std::abs(mine.entrance.z - entrance.z) <= 24)
        {
            return false;
        }
    }
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(SharedDb(), "INSERT INTO mines (x, y, z, resources) VALUES (?1, ?2, ?3, ?4)", -1, &statement, nullptr);
    sqlite3_bind_int(statement, 1, entrance.x);
    sqlite3_bind_int(statement, 2, entrance.y);
    sqlite3_bind_int(statement, 3, entrance.z);
    sqlite3_bind_text(statement, 4, resources.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
    return true;
}

/// @brief No wanted ore within sight: tunnel to a remembered one, else dig down to the level where it
/// generates and tunnel along it (ores show up from 32 blocks away, through the rock).
/// @return false once the bot has searched enough times without finding: time to give up
/// @brief A wide mine, `steps` blocks along `dir` from where the bot stands: 3 wide, and going down a block a
/// step (a staircase 4 high, with a cobblestone stair across each step so it can be walked back up) or, at
/// ore level, level (a gallery 3 high). A torch on the wall every 6 blocks. Stops before water, lava and
/// bedrock, as the narrow tunnels did.
/// @return how many steps were dug
static int WideMine(McpBot& c, const Position& dir, const int steps, const bool down)
{
    const std::shared_ptr<World> world = c.GetWorld();
    const Position perp(std::abs(dir.z), 0, std::abs(dir.x));
    const Position start = BlockPos(c.GetLocalPlayer()->GetPosition());
    // A torch on the wall at +perp: clicked on that wall's face
    const PlayerDiggingFace wall_face = perp.x != 0 ? PlayerDiggingFace::West : PlayerDiggingFace::North;
    const std::string stairs = "minecraft:cobblestone_stairs";
    int dug = 0;
    for (int i = 1; i <= steps; ++i)
    {
        const int floor_y = down ? start.y - i : start.y;            // where the feet are on this step
        const Position base = start + dir * i;
        const int high = down ? 3 : 2;                                // cells above the feet to clear
        // Safe to open? No fluid, lava near or unbreakable block anywhere in the slice
        for (int w = -1; w <= 1; ++w)
        {
            for (int h = 0; h <= high; ++h)
            {
                const Position cell(base.x + perp.x * w, floor_y + h, base.z + perp.z * w);
                const Blockstate* b = world->GetBlock(cell);
                if (b == nullptr || b->IsFluid() || b->GetHardness() < 0.0f || LavaNear(world, cell))
                {
                    c.SetNote("wide mine: stopped before water, lava or bedrock");
                    return dug;
                }
            }
        }
        bool changed = false;
        for (int h = high; h >= 0; --h)
        {
            for (int w = -1; w <= 1; ++w)
            {
                const Position cell(base.x + perp.x * w, floor_y + h, base.z + perp.z * w);
                const Blockstate* b = world->GetBlock(cell);
                const bool needs_dig = b != nullptr && !b->IsAir();
                if (needs_dig && SafeDig(c, cell, true) == Status::Failure)
                {
                    if (changed) PenalizeNarrowMine(c, Position(base.x, floor_y, base.z), dir);
                    c.SetNote("wide mine: could not dig at " + std::to_string(cell.x) + " " + std::to_string(cell.y) + " " + std::to_string(cell.z));
                    return dug;
                }
                if (needs_dig) changed = true;
            }
        }
        // Something to stand on
        const Position under(base.x, floor_y - 1, base.z);
        const Blockstate* floor = world->GetBlock(under);
        if ((floor == nullptr || !floor->IsSolid()) && PlaceBlock(c, "minecraft:cobblestone", under, std::nullopt, true, false, false) == Status::Failure)
        {
            c.SetNote("wide mine: no floor to step on");
            return dug;
        }
        if (GoTo(c, Position(base.x, floor_y, base.z), 0) == Status::Failure)
        {
            return dug;
        }
        ++dug;
        // The stairs on the step behind, now at head height, looking back up the mine: a stair faces the way
        // the player looks when placing it, so its high side is toward the way out
        if (down && i >= 2)
        {
            std::string error;
            if (CountItem(c, stairs) < 3 && CountItem(c, "minecraft:cobblestone") >= 6)
            {
                CraftOne(c, stairs, error);
            }
            for (int w = -1; w <= 1 && CountItem(c, stairs) > 0; ++w)
            {
                const Position step(base.x - dir.x + perp.x * w, floor_y + 1, base.z - dir.z + perp.z * w);
                const Blockstate* b = world->GetBlock(step);
                if (b != nullptr && b->IsAir())
                {
                    LookAt(c, Vector3<double>(step.x + 0.5 - dir.x * 2.0, step.y + 0.5, step.z + 0.5 - dir.z * 2.0));
                    PlaceBlock(c, stairs, step, PlayerDiggingFace::Up, true, false, false);
                }
            }
        }
        // A torch on the wall every 6 blocks, at head height
        if (i % 6 == 0)
        {
            std::string error;
            if (CountItem(c, "minecraft:torch") == 0 && (CountItem(c, "minecraft:coal") > 0 || CountItem(c, "minecraft:charcoal") > 0))
            {
                if (CountItem(c, "minecraft:stick") == 0)
                {
                    CraftOne(c, "minecraft:stick", error);
                }
                CraftOne(c, "minecraft:torch", error);
            }
            if (CountItem(c, "minecraft:torch") > 0)
            {
                PlaceBlock(c, "minecraft:torch", Position(base.x + perp.x, floor_y + 1, base.z + perp.z), wall_face, true, false, false);
            }
        }
        // Back to the pickaxe for the next slice
        const std::string pickaxe = HeldTool(c, "pickaxe");
        if (!pickaxe.empty())
        {
            SetItemInHand(c, pickaxe);
        }
    }
    return dug;
}

static bool SeekOre(McpBot& c, const std::vector<std::string>& items)
{
    constexpr int max_searches = 30;
    if (c.explorations >= max_searches)
    {
        c.explorations = 0;
        return false;
    }
    const int search = c.explorations++;
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    const Position feet = BlockPos(me);
    const auto distance_to = [&](const Position& p) { return std::sqrt(c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(p.x + 0.5, p.y + 0.5, p.z + 0.5))); };

    bool remembered = false;
    std::pair<std::string, Position> nearest;
    for (const std::string& item : items)
    {
        for (const auto& place : Recall(c, item, 1))
        {
            if (!remembered || distance_to(place.second) < distance_to(nearest.second))
            {
                remembered = true;
                nearest = place;
            }
        }
    }
    if (remembered && distance_to(nearest.second) <= 200.0)
    {
        const Position& p = nearest.second;
        c.SetNote("going to a remembered " + nearest.first + " at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z));
        const double before = distance_to(p);
        // On foot as far as the pathfinder gets, then by tunnel
        GoTo(c, p, 12);
        if (TunnelToward(c, p, 40) == Status::Failure && distance_to(p) > before - 2.0)
        {
            // No way there (water, lava, bedrock): don't try again
            Forget(c, p);
        }
        return true;
    }

    // Deepest level where the wanted ores are common
    int level = 320;
    const auto wide_look = [&]()
        {
            // Nothing known: look through the rock all around, once every two minutes, then go to what it shows
            if (std::chrono::steady_clock::now() < c.look_retry_at)
            {
                return false;
            }
            c.look_retry_at = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            const int seen = LookThroughRock(c, items, level);
            if (seen > 0)
            {
                c.SetNote("looked through the rock: " + std::to_string(seen) + " " + items.front() + " (and alike) within 96 blocks, going for the nearest");
            }
            return seen > 0;
        };
    for (const std::string& item : items)
    {
        level = std::min(level,
            // Deep caves are flooded with lava from y -54 down: one level above it, where both are still plentiful
            item.find("diamond") != std::string::npos || item.find("redstone") != std::string::npos ? -53 :
            item.find("gold") != std::string::npos ? -16 :
            item.find("lapis") != std::string::npos ? 0 :
            item.find("copper") != std::string::npos ? 48 :
            item.find("coal") != std::string::npos ? 64 :
            item.find("emerald") != std::string::npos ? 80 : 16);
    }
    if (wide_look())
    {
        // The next round finds them in the memory and tunnels there
        return true;
    }
    static const Position directions[] = { Position(1, 0, 0), Position(0, 0, 1), Position(-1, 0, 0), Position(0, 0, -1) };
    const Position& direction = directions[search % 4];
    const bool descend = feet.y > level + 3;
    c.SetNote(descend ? "no " + items.front() + " in sight: digging down to y=" + std::to_string(level) + " where it generates (now at y=" + std::to_string(feet.y) + ")" :
        "no " + items.front() + " in sight: tunnelling along ore level (" + std::to_string(search + 1) + "/" + std::to_string(max_searches) + ")");
    std::string looking_for;
    for (const std::string& item : items)
    {
        looking_for += (looking_for.empty() ? "" : ",") + item;
    }
    if (descend && c.GetWorld()->GetSkyLight(feet) > 0 && AddMine(feet, looking_for))
    {
        // Going underground from the open: this is a mine entrance from now on (roads lead to them)
        c.Say("opened a mine at " + std::to_string(feet.x) + " " + std::to_string(feet.y) + " " + std::to_string(feet.z));
    }
    // Wide, lit and with stairs: a staircase down to the ore level, then galleries along it
    const int steps = descend ? std::min(20, feet.y - level) : 20;
    if (WideMine(c, direction, std::max(steps, 1), descend) == 0 && distance_to(feet) < 2.0)
    {
        // Can't dig that way at all (water, lava...): walk somewhere else
        GoTo(c, Position(feet.x + direction.x * 24, feet.y, feet.z + direction.z * 24), 8);
    }
    return true;
}

/// @brief Keep track of gear actions failing in a row
static void Check(McpBot& c, const Status result)
{
    c.gear_failures = result == Status::Success ? 0 : c.gear_failures + 1;
}

/// @brief Mine one matching block with the best tool of this kind in hand, or explore to find some
/// @brief Up toward the sky, a staircase: each way in turn until one gets the bot somewhere (one way can run
/// into lava, which the tunnel stops before: always the same way, a bot stayed put for good)
static void DigUp(McpBot& c)
{
    static const Position ways[] = { Position(6, 12, 0), Position(0, 12, 6), Position(-6, 12, 0), Position(0, 12, -6) };
    const Vector3<double> start = c.GetLocalPlayer()->GetPosition();
    for (int i = 0; i < 4; ++i)
    {
        const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
        TunnelToward(c, feet + ways[(c.explorations + i) % 4], 24, true);
        if (c.GetLocalPlayer()->GetPosition().SqrDist(start) > 2.0 * 2.0)
        {
            return;
        }
    }
    // Every way blocked: start from another one next time
    ++c.explorations;
}

static void MineOne(McpBot& c, const std::function<bool(const Blockstate&)>& match, const std::string& what, const std::string& tool_kind, const std::string& stage, const std::string& remembered_kind = "")
{
    const bool ore = EndsWith(remembered_kind, "_ore");
    // Wood from underground (no sky above, below sea level: not just under a roof): up to the surface first,
    // whatever trees show through the rock (going for them from a cave failed path after path)
    {
        const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
        if (tool_kind == "axe" && feet.y < 62 && c.GetWorld()->GetSkyLight(feet + Position(0, 1, 0)) == 0)
        {
            c.SetNote(stage + ": underground, needs wood: digging up to the surface");
            DigUp(c);
            return;
        }
    }
    // Counted as the collecting below counts it: what lies in a construction site is not there to take
    // (found here and refused there, the bot searched the same blocks twice a tick, forever, without moving)
    // (nor what it gave up on lately: see CollectTask)
    if (FindBlocks(c, match, 32, 1, [&](const Position& p) { return !InStoredSite(p) && !c.InSite(p) && c.unreachable.count(p) == 0; }).empty())
    {
        if (ore)
        {
            if (!SeekOre(c, { remembered_kind }))
            {
                SkipStage(c, stage, "no " + what + " found after digging for it");
            }
            return;
        }
        // No trees in a cave: up to the surface first, a staircase dug toward the sky (before the remembered
        // trees: from underground, walking to one up on the surface failed search after search)
        const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
        if (tool_kind == "axe" && feet.y < 62 && c.GetWorld()->GetSkyLight(feet + Position(0, 1, 0)) == 0)
        {
            c.SetNote(stage + ": underground, no trees: digging up to the surface");
            DigUp(c);
            return;
        }
        if (!remembered_kind.empty() && GoToRemembered(c, remembered_kind))
        {
            return;
        }
        if (!Explore(c, what))
        {
            SkipStage(c, stage, "no " + what + " found after exploring");
        }
        return;
    }
    c.explorations = 0;
    const std::string tool = HeldTool(c, tool_kind);
    if (!tool.empty())
    {
        SetItemInHand(c, tool);
    }
    c.SetNote(stage + ": getting " + what);
    std::atomic<int> done = 0;
    const bool tunnel = ore || (tool_kind == "pickaxe" && !tool.empty());
    Check(c, CollectTask(c, match, 1, 32, done, tunnel));
}

/// @brief True if the bot holds count planks of one kind. Otherwise does one action toward it (craft from a log, cut a tree)
static bool EnsurePlanks(McpBot& c, const int count, const std::string& stage)
{
    if (!ItemLike(c, "_planks", count).empty())
    {
        return true;
    }
    const std::string log = ItemLike(c, "_log");
    if (!log.empty())
    {
        c.SetNote(stage + ": crafting planks");
        Grid grid;
        grid[0][0] = log;
        std::string error;
        Check(c, CraftAt(c, grid, error));
        return false;
    }
    if (stage == "building")
    {
        if (c.GetGoal().second <= 0)
        {
            c.SetGoal("oak_log,spruce_log,birch_log,jungle_log,acacia_log,dark_oak_log,mangrove_log,cherry_log,pale_oak_log", 16);
            c.explorations = 0;
        }
        c.SetNote("building: out of wood, gathering logs");
        return false;
    }
    MineOne(c, [](const Blockstate& b) { return EndsWith(b.GetName(), "_log"); }, "trees", "axe", stage, "_log");
    return false;
}

/// @brief True if a crafting table is in range. Otherwise does one action toward it (craft it, place it)
static bool EnsureTable(McpBot& c, const std::string& stage)
{
    if (!FindBlocks(c, [](const Blockstate& b) { return b.GetName() == "minecraft:crafting_table"; }, 32, 1).empty())
    {
        return true;
    }
    // A known one, only for a bot that can't make one: a table is one log, a walk to one remembered 200 blocks
    // away (not even loaded) failed and failed until the bot gave up on its tools
    const bool can_make = CountItem(c, "minecraft:crafting_table") > 0 || !ItemLike(c, "_planks", 4).empty() || !ItemLike(c, "_log").empty();
    if (!can_make && GoToRemembered(c, "crafting_table"))
    {
        return false;
    }
    if (CountItem(c, "minecraft:crafting_table") == 0)
    {
        if (EnsurePlanks(c, 4, stage))
        {
            c.SetNote(stage + ": crafting a crafting table");
            const std::string planks = ItemLike(c, "_planks", 4);
            const Grid grid = { { { planks, planks, "" }, { planks, planks, "" }, { "", "", "" } } };
            std::string error;
            Check(c, CraftAt(c, grid, error));
        }
        return false;
    }
    c.SetNote(stage + ": placing the crafting table");
    Position placed;
    Check(c, PlaceNearby(c, "minecraft:crafting_table", placed) ? Status::Success : Status::Failure);
    return false;
}

struct ToolCost
{
    const char* kind;
    int material;
    int sticks;
};
static const ToolCost tool_costs[] = { { "pickaxe", 3, 2 }, { "axe", 3, 2 }, { "sword", 2, 1 }, { "shovel", 1, 2 } };

static bool HasTools(BehaviourClient& c, const int worst_tier)
{
    return std::all_of(std::begin(tool_costs), std::end(tool_costs), [&](const ToolCost& t) { return !HeldTool(c, t.kind, worst_tier).empty(); });
}

static bool IsStone(const Blockstate& b)
{
    return b.GetName() == "minecraft:stone" || b.GetName() == "minecraft:cobblestone";
}

/// @brief One action toward this tool, wooden or stone: crafting table, sticks, material, then the tool itself
static void MakeTool(McpBot& c, const ToolCost* next, const bool stone, const std::string& stage)
{
    if (!EnsureTable(c, stage))
    {
        return;
    }
    if (CountItem(c, "minecraft:stick") < next->sticks)
    {
        if (EnsurePlanks(c, 2, stage))
        {
            c.SetNote(stage + ": crafting sticks");
            const std::string planks = ItemLike(c, "_planks", 2);
            const Grid grid = { { { planks, "", "" }, { planks, "", "" }, { "", "", "" } } };
            std::string error;
            Check(c, CraftAt(c, grid, error));
        }
        return;
    }
    std::string m;
    if (stone)
    {
        if (CountItem(c, "minecraft:cobblestone") < next->material)
        {
            MineOne(c, IsStone, "stone", "pickaxe", stage, "stone");
            return;
        }
        m = "minecraft:cobblestone";
    }
    else
    {
        if (!EnsurePlanks(c, next->material, stage))
        {
            return;
        }
        m = ItemLike(c, "_planks", next->material);
    }
    const std::string kind = next->kind;
    const std::string stick = "minecraft:stick";
    const Grid grid =
        kind == "pickaxe" ? Grid{ { { m, m, m }, { "", stick, "" }, { "", stick, "" } } } :
        kind == "axe" ? Grid{ { { m, m, "" }, { m, stick, "" }, { "", stick, "" } } } :
        kind == "sword" ? Grid{ { { "", m, "" }, { "", m, "" }, { "", stick, "" } } } :
        Grid{ { { "", m, "" }, { "", stick, "" }, { "", stick, "" } } };
    c.SetNote(stage + ": crafting the " + kind);
    std::string error;
    Check(c, CraftAt(c, grid, error));
}

/// @brief One action toward the next missing tool, wooden or stone
static void ToolStep(McpBot& c, const bool stone, const std::string& stage)
{
    MakeTool(c, std::find_if(std::begin(tool_costs), std::end(tool_costs), [&](const ToolCost& t)
        {
            return HeldTool(c, t.kind, stone ? stone_tier : any_tier).empty();
        }), stone, stage);
}

/// @brief Decision tree condition "has no <tool>?": false too once making wooden tools was given up
static Status LacksTool(McpBot& c, const std::string& kind)
{
    return c.gear && !c.IsGearSkipped("wooden tools") && HeldTool(c, kind).empty() ? Status::Success : Status::Failure;
}

/// @brief Decision tree action "make <tool>": one step toward the wooden one, with the same safety nets as the other gear
static Status MakeToolStep(McpBot& c, const std::string& kind)
{
    c.SetState("making " + kind);
    const std::string stage = "wooden tools";
    if (c.gear_failures >= 5)
    {
        SkipStage(c, stage, "its actions kept failing");
    }
    else if (c.CountGearStep(stage) > 250)
    {
        SkipStage(c, stage, "taking too long");
    }
    else
    {
        // With the cobblestone for it, the stone tool straight away: better, and only 2 sticks of wood (a bot
        // with hundreds of cobblestone kept failing at the wooden one for lack of planks)
        const ToolCost* cost = std::find_if(std::begin(tool_costs), std::end(tool_costs), [&](const ToolCost& t) { return kind == t.kind; });
        MakeTool(c, cost, cost != std::end(tool_costs) && CountItem(c, "minecraft:cobblestone") >= cost->material, stage);
    }
    return Status::Success;
}

/// @brief Before a team build: the wooden tools, and with --chests a chest to store what the bot digs out
static bool ReadyToBuild(McpBot& c)
{
    return !c.gear || ((c.IsGearSkipped("wooden tools") || HasTools(c, any_tier)) &&
        (!auto_store || c.IsGearSkipped("chest") || CountItem(c, "minecraft:chest") > 0));
}

/// @brief Decision tree action "get ready to build": one step toward the next tool, or the chest. Out of wood,
/// the steps cut a tree, exploring to find one; given up for 5 minutes when it fails or takes too long
static Status ReadyStep(McpBot& c)
{
    c.SetState("getting ready to build");
    const bool tools = !c.IsGearSkipped("wooden tools") && !HasTools(c, any_tier);
    const std::string stage = tools ? "wooden tools" : "chest";
    if (c.gear_failures >= 5)
    {
        SkipStage(c, stage, "its actions kept failing");
    }
    else if (c.CountGearStep(stage) > 250)
    {
        SkipStage(c, stage, "taking too long");
    }
    else if (tools)
    {
        ToolStep(c, false, stage);
    }
    else if (EnsureTable(c, stage) && EnsurePlanks(c, 8, stage))
    {
        c.SetNote(stage + ": crafting a chest");
        const std::string p = ItemLike(c, "_planks", 8);
        const Grid grid = { { { p, p, p }, { p, "", p }, { p, p, p } } };
        std::string error;
        Check(c, CraftAt(c, grid, error));
    }
    return Status::Success;
}

static bool HasWood(BehaviourClient& c)
{
    return !ItemLike(c, "_log").empty() || !ItemLike(c, "_planks").empty();
}

static bool TakeFromChest(McpBot& c, const std::string& item);

/// @brief Decision tree action "get food": hunt the nearest farm animal, or explore to find some
static Status GetFoodStep(McpBot& c)
{
    c.SetState("getting food");
    for (const char* food : { "minecraft:bread", "minecraft:cooked_beef", "minecraft:cooked_porkchop", "minecraft:baked_potato", "minecraft:carrot", "minecraft:sweet_berries" })
    {
        if (TakeFromChest(c, food))
        {
            return Status::Success;
        }
    }
    // Wheat in the pack is bread (3 a loaf), no hunt needed; a bot carried 64 wheat while starving
    if (CountItem(c, "minecraft:wheat") >= 3)
    {
        std::string error;
        c.SetNote("no food: baking bread from its wheat");
        if (CraftOne(c, "minecraft:bread", error) > 0)
        {
            return Status::Success;
        }
    }
    for (const char* animal : { "cow", "pig", "sheep" })
    {
        if (!WildAnimals(c, animal, 32.0).empty())
        {
            c.explorations = 0;
            c.SetNote(std::string("no food: hunting a ") + animal);
            std::atomic<int> killed = 0;
            KillTask(c, animal, 1, 32.0, killed);
            if (killed == 0)
            {
                // In sight but out of reach (behind a wall, across a ravine): not again for three minutes
                c.food_retry_at = std::chrono::steady_clock::now() + std::chrono::minutes(3);
                c.SetNote(std::string("could not get to a ") + animal + " for food, will try again in 3 minutes");
            }
            return Status::Success;
        }
    }
    // Crop fields seen before (a village's): their wheat makes bread (next round), carrots and potatoes are
    // eaten as they are. Harvested and replanted. A field with nothing ripe now waits 10 minutes
    for (const char* kind : { "carrots", "potatoes", "wheat" })
    {
        const std::vector<std::pair<std::string, Position>> seen = Recall(c, kind, 1);
        if (seen.empty() || c.unreachable.count(seen[0].second) != 0)
        {
            continue;
        }
        const Position field = seen[0].second;
        const double d = std::sqrt(c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(field.x, field.y, field.z)));
        if (d > 400.0)
        {
            continue;
        }
        c.SetNote(std::string("no food: to the ") + kind + " seen at " + std::to_string(field.x) + " " + std::to_string(field.z));
        if (d > 24.0 && TravelTask(c, field.x, field.z) == Status::Failure)
        {
            c.unreachable[field] = std::chrono::steady_clock::now();
            return Status::Success;
        }
        std::atomic<int> harvested = 0, replanted = 0;
        FarmTask(c, 16, true, harvested, replanted);
        if (harvested == 0)
        {
            c.unreachable[field] = std::chrono::steady_clock::now();
        }
        c.SetNote(std::string("no food: harvested ") + std::to_string(harvested.load()) + " at the " + kind + " field");
        return Status::Success;
    }
    // Animals seen in the last half hour, not too far: there before exploring blindly
    for (const char* animal : { "cow", "pig", "sheep" })
    {
        const std::vector<std::pair<std::string, Position>> seen = Recall(c, std::string("mob:") + animal, 1);
        if (!seen.empty())
        {
            const Position& p = seen[0].second;
            const double d = std::sqrt(c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(p.x, p.y, p.z)));
            if (d > 24.0 && d < 200.0)
            {
                c.SetNote(std::string("no food: going where a ") + animal + " was seen, at " + std::to_string(p.x) + " " + std::to_string(p.z));
                if (TravelTask(c, p.x, p.z) == Status::Failure)
                {
                    Forget(c, p);
                }
                return Status::Success;
            }
        }
    }
    if (!Explore(c, "animals to hunt for food"))
    {
        c.food_retry_at = std::chrono::steady_clock::now() + std::chrono::minutes(5);
        c.SetNote("no animals found for food after exploring, will look again in 5 minutes");
    }
    return Status::Success;
}

struct ArmorPiece
{
    const char* item;
    int leather;
    short slot;
    Grid grid;
};
static const std::string L = "minecraft:leather";
// Cheapest first
static const ArmorPiece armor[] = {
    { "minecraft:leather_boots", 4, Window::INVENTORY_FEET_ARMOR, { { { L, "", L }, { L, "", L }, { "", "", "" } } } },
    { "minecraft:leather_helmet", 5, Window::INVENTORY_HEAD_ARMOR, { { { L, L, L }, { L, "", L }, { "", "", "" } } } },
    { "minecraft:leather_leggings", 7, Window::INVENTORY_LEGS_ARMOR, { { { L, L, L }, { L, "", L }, { L, "", L } } } },
    { "minecraft:leather_chestplate", 8, Window::INVENTORY_CHEST_ARMOR, { { { L, "", L }, { L, L, L }, { L, L, L } } } },
};

static bool IsSlotEmpty(BehaviourClient& c, const short index)
{
    const std::map<short, ProtocolCraft::Slot> slots = PlayerSlots(c);
    const auto slot = slots.find(index);
    return slot == slots.end() || slot->second.IsEmptySlot();
}

/// @brief No empty slot left in the main inventory and the hotbar
static bool InventoryFull(BehaviourClient& c)
{
    const std::map<short, ProtocolCraft::Slot> slots = PlayerSlots(c);
    for (short i = Window::INVENTORY_STORAGE_START; i < Window::INVENTORY_OFFHAND_INDEX; ++i)
    {
        const auto slot = slots.find(i);
        if (slot == slots.end() || slot->second.IsEmptySlot())
        {
            return false;
        }
    }
    return true;
}

/// @brief Move an item of the inventory to an armor or off hand slot
static Status Wear(BehaviourClient& c, const std::string& item, const short destination)
{
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (index >= Window::INVENTORY_STORAGE_START && index != destination && !slot.IsEmptySlot() &&
            AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() == item)
        {
            return SwapItemsInContainer(c, Window::PLAYER_INVENTORY_INDEX, index, destination);
        }
    }
    return Status::Failure;
}

static bool IsArmorWorn(BehaviourClient& c)
{
    return std::all_of(std::begin(armor), std::end(armor), [&](const ArmorPiece& piece) { return !IsSlotEmpty(c, piece.slot); });
}

/// @brief One action toward the next missing armor piece: wear it, craft it, or hunt for leather
static void ArmorStep(McpBot& c, const std::string& stage)
{
    for (const ArmorPiece& piece : armor)
    {
        if (!IsSlotEmpty(c, piece.slot))
        {
            continue;
        }
        if (CountItem(c, piece.item) > 0)
        {
            c.SetNote(stage + ": putting on " + std::string(piece.item).substr(10));
            Check(c, Wear(c, piece.item, piece.slot));
            return;
        }
        const int leather = CountItem(c, L);
        if (leather >= piece.leather)
        {
            if (EnsureTable(c, stage))
            {
                c.SetNote(stage + ": crafting " + std::string(piece.item).substr(10));
                std::string error;
                Check(c, CraftAt(c, piece.grid, error));
            }
            return;
        }
        if (WildAnimals(c, "cow", 32.0).empty())
        {
            if (!Explore(c, "cows"))
            {
                SkipStage(c, stage, "no cows found after exploring");
            }
            return;
        }
        c.explorations = 0;
        c.SetNote(stage + ": hunting cows for leather (" + std::to_string(leather) + "/" + std::to_string(piece.leather) + ")");
        std::atomic<int> killed = 0;
        Check(c, KillTask(c, "cow", 1, 32.0, killed));
        return;
    }
}

/// @brief Move the first stack of this item from the bot's inventory to a given (empty) slot of the open container
static void MoveToContainer(BehaviourClient& c, const short container_id, const std::string& item, const short destination)
{
    const std::shared_ptr<Window> window = c.GetInventoryManager()->GetWindow(container_id);
    for (const auto& [index, slot] : window->GetSlots())
    {
        if (index >= window->GetFirstPlayerInventorySlot() && !slot.IsEmptySlot() &&
            AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() == item)
        {
            MoveStack(c, container_id, index, destination);
            return;
        }
    }
}

/// @brief True (with its position) if a furnace is in range. Otherwise does one action toward one: mine the
/// cobblestone, craft it, place it
static bool EnsureFurnace(McpBot& c, const std::string& stage, Position& furnace)
{
    // Each bot its own furnace: one smelts a block every 10 seconds, a team sharing one would queue at it.
    // The bot's own if it is still standing and not too far; a new one where it works now otherwise
    Position mine;
    if (GetOwn(c, "furnace", mine) && c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(mine.x, mine.y, mine.z)) < 48.0 * 48.0)
    {
        const Blockstate* block = c.GetWorld()->GetBlock(mine);
        if (block != nullptr && block->GetName() == "minecraft:furnace")
        {
            furnace = mine;
            return true;
        }
    }
    if (CountItem(c, "minecraft:furnace") > 0)
    {
        c.SetNote(stage + ": placing the furnace");
        Position placed;
        const bool done = PlaceNearby(c, "minecraft:furnace", placed);
        if (done)
        {
            SetOwn(c, "furnace", placed);
        }
        Check(c, done ? Status::Success : Status::Failure);
    }
    else if (CountItem(c, "minecraft:cobblestone") < 8)
    {
        if (stage == "building")
        {
            if (c.GetGoal().second <= 0)
            {
                c.SetGoal("stone,cobblestone", 32);
                c.explorations = 0;
            }
            c.SetNote("building: out of cobblestone for a furnace, gathering stone");
            return false;
        }
        MineOne(c, IsStone, "stone", "pickaxe", stage, "stone");
    }
    else if (EnsureTable(c, stage))
    {
        c.SetNote(stage + ": crafting a furnace");
        const std::string m = "minecraft:cobblestone";
        const Grid grid = { { { m, m, m }, { m, "", m }, { m, m, m } } };
        std::string error;
        Check(c, CraftAt(c, grid, error));
    }
    return false;
}

/// @brief One visit to a furnace: take what is done, put a stack of `input` in if it is empty, feed it, and wait
/// a little for the first results. What is left inside keeps cooking and is collected at the next visit
static Status UseFurnace(McpBot& c, const Position& furnace, const std::string& input, const std::string& stage)
{
    // Coal burns longest; planks do; logs would be smelted into charcoal instead of burning
    std::string fuel;
    for (const std::string& candidate : { std::string("minecraft:coal"), std::string("minecraft:charcoal"), ItemLike(c, "_planks") })
    {
        if (fuel.empty() && !candidate.empty() && CountItem(c, candidate) > 0)
        {
            fuel = candidate;
        }
    }
    if (ReachBlock(c, furnace) == Status::Failure || OpenContainer(c, furnace) == Status::Failure)
    {
        return Status::Failure;
    }
    const short container_id = c.GetInventoryManager()->GetFirstOpenedWindowId();
    const std::shared_ptr<Window> window = c.GetInventoryManager()->GetWindow(container_id);
    constexpr short input_slot = 0;
    constexpr short fuel_slot = 1;
    constexpr short output_slot = 2;
    // A furnace window: input, fuel and output slots, then the player's 36
    const short first = window->GetFirstPlayerInventorySlot();
    const auto take_output = [&]() { return MoveStack(c, container_id, output_slot, EmptySlot(window, first, first + 36)); };
    bool collected = !window->GetSlot(output_slot).IsEmptySlot() && take_output();
    if (window->GetSlot(input_slot).IsEmptySlot() && CountItem(c, input) > 0)
    {
        MoveToContainer(c, container_id, input, input_slot);
    }
    if (window->GetSlot(fuel_slot).IsEmptySlot() && !fuel.empty())
    {
        MoveToContainer(c, container_id, fuel, fuel_slot);
    }
    c.SetNote(stage + ": at the furnace");
    {
        // 10 seconds per item. Standing still that long is not being stuck
        const WorkingInPlace at_the_furnace(c);
        Utilities::YieldForCondition([&]() { return !window->GetSlot(output_slot).IsEmptySlot(); }, c, 25000);
    }
    if (!window->GetSlot(output_slot).IsEmptySlot() && take_output())
    {
        collected = true;
    }
    const bool cooking = !window->GetSlot(input_slot).IsEmptySlot();
    CloseContainer(c, container_id);
    return collected || cooking ? Status::Success : Status::Failure;
}

/// @brief One action toward an iron ingot from raw iron: get a furnace (craft, place), then smelt
static void SmeltStep(McpBot& c, const std::string& stage)
{
    Position furnace;
    // Something to burn first: planks at least
    if (!EnsureFurnace(c, stage, furnace) || (CountItem(c, "minecraft:coal") == 0 && CountItem(c, "minecraft:charcoal") == 0 && !EnsurePlanks(c, 1, stage)))
    {
        return;
    }
    Check(c, UseFurnace(c, furnace, "minecraft:raw_iron", stage));
}

static bool IsShieldHeld(BehaviourClient& c)
{
    // A shield, not anything at all (sticks ended up there, and passed for one)
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (index == Window::INVENTORY_OFFHAND_INDEX && !slot.IsEmptySlot())
        {
            return AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() == "minecraft:shield";
        }
    }
    return false;
}

/// @brief One action toward a shield in the off hand
static void ShieldStep(McpBot& c, const std::string& stage)
{
    if (CountItem(c, "minecraft:shield") > 0)
    {
        c.SetNote(stage + ": taking the shield in the off hand");
        Check(c, Wear(c, "minecraft:shield", Window::INVENTORY_OFFHAND_INDEX));
    }
    else if (CountItem(c, "minecraft:iron_ingot") > 0)
    {
        if (EnsureTable(c, stage) && EnsurePlanks(c, 6, stage))
        {
            c.SetNote(stage + ": crafting the shield");
            const std::string p = ItemLike(c, "_planks", 6);
            const Grid grid = { { { p, "minecraft:iron_ingot", p }, { p, p, p }, { "", p, "" } } };
            std::string error;
            Check(c, CraftAt(c, grid, error));
        }
    }
    else if (CountItem(c, "minecraft:raw_iron") > 0)
    {
        SmeltStep(c, stage);
    }
    else if (HeldTool(c, "pickaxe", stone_tier).empty())
    {
        SkipStage(c, stage, "mining iron needs a stone pickaxe");
    }
    else
    {
        // ponytail: only ore the pathfinder can walk up to (surface, open caves); no tunnelling toward buried ore
        MineOne(c, [](const Blockstate& b) { return EndsWith(b.GetName(), ":iron_ore") || EndsWith(b.GetName(), ":deepslate_iron_ore"); }, "iron ore", "pickaxe", stage, "iron_ore");
    }
}

/// @brief One action toward an iron pickaxe (the only way to get gold, diamond, redstone): 3 ingots and 2 sticks
static void IronPickaxeStep(McpBot& c, const std::string& stage)
{
    const int ingots = CountItem(c, "minecraft:iron_ingot");
    const int raw = CountItem(c, "minecraft:raw_iron");
    if (ingots >= 3)
    {
        if (!EnsureTable(c, stage))
        {
            return;
        }
        if (CountItem(c, "minecraft:stick") < 2)
        {
            if (EnsurePlanks(c, 2, stage))
            {
                c.SetNote(stage + ": crafting sticks");
                const std::string planks = ItemLike(c, "_planks", 2);
                const Grid grid = { { { planks, "", "" }, { planks, "", "" }, { "", "", "" } } };
                std::string error;
                Check(c, CraftAt(c, grid, error));
            }
            return;
        }
        c.SetNote(stage + ": crafting the iron pickaxe");
        const std::string i = "minecraft:iron_ingot";
        const std::string stick = "minecraft:stick";
        const Grid grid = { { { i, i, i }, { "", stick, "" }, { "", stick, "" } } };
        std::string error;
        Check(c, CraftAt(c, grid, error));
    }
    else if (raw > 0 && raw + ingots >= 3)
    {
        SmeltStep(c, stage);
    }
    else
    {
        MineOne(c, [](const Blockstate& b) { return EndsWith(b.GetName(), ":iron_ore") || EndsWith(b.GetName(), ":deepslate_iron_ore"); }, "iron ore", "pickaxe", stage, "iron_ore");
    }
}

/// @brief The tool stage still to do, "" if none: these come before the objectives, mining needs them
static std::string ToolStage(McpBot& c)
{
    return !HasTools(c, any_tier) && !c.IsGearSkipped("wooden tools") ? "wooden tools" :
        !HasTools(c, stone_tier) && !c.IsGearSkipped("stone tools") ? "stone tools" :
        HeldTool(c, "pickaxe", iron_tier).empty() && !c.IsGearSkipped("iron pickaxe") ? "iron pickaxe" : "";
}

static bool OwnsBed(McpBot& c);
static void BedStep(McpBot& c, const std::string& stage);

/// @brief The comfort and protection stage still to do, "" if none: these come after the objectives
static std::string ArmorStage(McpBot& c)
{
    return !OwnsBed(c) && !c.IsGearSkipped("bed") ? "bed" :
        !IsArmorWorn(c) && !c.IsGearSkipped("leather armor") ? "leather armor" :
        !IsShieldHeld(c) && !c.IsGearSkipped("shield") ? "shield" : "";
}

/// @brief Do the next small action of a gear stage
static Status GearStep(McpBot& c, const std::string& stage)
{
    // Safety nets: a stage that makes no progress must not hold the bot forever
    if (c.gear_failures >= 5)
    {
        SkipStage(c, stage, "its actions kept failing");
    }
    else if (c.CountGearStep(stage) > 400)
    {
        SkipStage(c, stage, "taking too long");
    }
    else if (stage == "wooden tools")
    {
        ToolStep(c, false, stage);
    }
    else if (stage == "stone tools" || stage == "iron pickaxe")
    {
        if (HeldTool(c, "pickaxe").empty())
        {
            SkipStage(c, stage, "mining needs a pickaxe");
        }
        else if (stage == "stone tools")
        {
            ToolStep(c, true, stage);
        }
        else
        {
            IronPickaxeStep(c, stage);
        }
    }
    else if (stage == "bed")
    {
        BedStep(c, stage);
    }
    else if (stage == "leather armor")
    {
        ArmorStep(c, stage);
    }
    else
    {
        ShieldStep(c, stage);
    }
    return Status::Success;
}

// ---------------------------------------------------------------- team: travel together, shared objective

/// @brief Where the team leader is, if this bot has one to follow: seen directly, else as told by the main process
static bool LeaderPosition(McpBot& c, Vector3<double>& at)
{
    Team team;
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        team = c.team;
    }
    if (team.leader.empty() || team.leader == c.GetNetworkManager()->GetMyName())
    {
        return false;
    }
    {
        auto entities = c.GetEntityManager()->GetEntities();
        for (const auto& [id, e] : *entities)
        {
            if (e && e->IsRemotePlayer() && c.GetPlayerName(e->GetUUID()) == team.leader)
            {
                at = e->GetPosition();
                return true;
            }
        }
    }
    at = team.leader_at;
    return team.leader_located;
}

/// @brief Team attack objective: kill one more of the shared target, exploring to find some
static Status HuntStep(McpBot& c)
{
    c.SetState("hunting");
    std::string type;
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        type = c.team.hunt_type;
    }
    if (FindEntities(c, type, 32.0).empty())
    {
        // One seen in the last half hour (by this bot): there first
        const std::vector<std::pair<std::string, Position>> seen = Recall(c, "mob:" + type, 1);
        if (!seen.empty() && c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(seen[0].second.x, seen[0].second.y, seen[0].second.z)) > 16.0 * 16.0)
        {
            c.SetNote("hunting " + type + ": going where one was seen, at " + std::to_string(seen[0].second.x) + " " + std::to_string(seen[0].second.z));
            if (TravelTask(c, seen[0].second.x, seen[0].second.z) == Status::Failure || GoTo(c, seen[0].second, 4) == Status::Failure)
            {
                Forget(c, seen[0].second);
            }
            return Status::Success;
        }
        if (!Explore(c, type))
        {
            std::lock_guard<std::mutex> lock(c.team_mutex);
            c.team.hunt_left = 0;
            c.SetNote("gave up hunting " + type + ": none found after exploring");
        }
        return Status::Success;
    }
    c.explorations = 0;
    std::atomic<int> killed = 0;
    KillTask(c, type, 1, 32.0, killed);
    std::lock_guard<std::mutex> lock(c.team_mutex);
    c.team.hunt_left -= killed;
    c.SetNote("hunting " + type + ", " + std::to_string(std::max(c.team.hunt_left, 0)) + " left for this bot");
    return Status::Success;
}

/// @brief True if the bot holds something to burn in a furnace. Otherwise one step toward it: coal is what
/// it goes for (mined from coal ore), planks only if a search for coal came back empty
static bool EnsureFuel(McpBot& c, const std::string& stage, bool& gathering);

/// @brief Use the item in hand while looking at a point: how a bucket is filled (looking at water) and
/// emptied (looking at the face the water goes against). The server works out what is hit from where the
/// player looks, so the bot has to be within reach and facing it
static Status UseHeldAt(BehaviourClient& c, const std::string& item, const Vector3<double>& target)
{
    if (SetItemInHand(c, item) == Status::Failure)
    {
        return Status::Failure;
    }
    const std::shared_ptr<LocalPlayer> player = c.GetLocalPlayer();
    if (player->GetPosition().SqrDist(target) > 3.5 * 3.5 && GoTo(c, BlockPos(target), 3) == Status::Failure)
    {
        return Status::Failure;
    }
    LookAt(c, target, true, true);
    Wait(c, 150);
    std::shared_ptr<ProtocolCraft::ServerboundUseItemPacket> use = std::make_shared<ProtocolCraft::ServerboundUseItemPacket>();
    use->SetHand(static_cast<int>(Hand::Main));
    use->SetSequence(c.GetWorld()->GetNextWorldInteractionSequenceId());
    use->SetYRot(player->GetYaw());
    use->SetXRot(player->GetPitch());
    c.GetNetworkManager()->Send(use);
    Wait(c, 300);
    return Status::Success;
}

static int LookThroughRock(McpBot& c, const std::vector<std::string>& items, const int level);

/// @brief Fish: at the nearest open water within 32 blocks (water with air over it), cast the rod, wait for a fish
/// to bite (the hook's biting flag, from the server: a bite comes in 5 to 30 s), reel it in. casts times
static Status FishTask(BehaviourClient& c, const int casts, std::atomic<int>& caught, std::string& error)
{
    const auto fish = [&]() { return CountItem(c, "minecraft:cod") + CountItem(c, "minecraft:salmon") + CountItem(c, "minecraft:tropical_fish") + CountItem(c, "minecraft:pufferfish"); };
    const std::shared_ptr<World> world = c.GetWorld();
    std::optional<Position> water;
    for (const Position& w : FindBlocks(c, [](const Blockstate& b) { return b.IsWater(); }, 32, 400))
    {
        const Blockstate* up = world->GetBlock(w + Position(0, 1, 0));
        if (up != nullptr && up->IsAir())
        {
            water = w;
            break;
        }
    }
    if (!water)
    {
        error = "no open water within 32 blocks";
        return Status::Failure;
    }
    // On the shore, a few blocks off, so the hook flies out onto the water
    if (GoTo(c, *water, 4, 2) == Status::Failure)
    {
        error = "could not reach the water at " + std::to_string(water->x) + " " + std::to_string(water->y) + " " + std::to_string(water->z);
        return Status::Failure;
    }
    const Vector3<double> target(water->x + 0.5, water->y + 0.9, water->z + 0.5);
    const auto use = [&]()
        {
            const std::shared_ptr<LocalPlayer> player = c.GetLocalPlayer();
            LookAt(c, target, true, true);
            std::shared_ptr<ProtocolCraft::ServerboundUseItemPacket> packet = std::make_shared<ProtocolCraft::ServerboundUseItemPacket>();
            packet->SetHand(static_cast<int>(Hand::Main));
            packet->SetSequence(world->GetNextWorldInteractionSequenceId());
            packet->SetYRot(player->GetYaw());
            packet->SetXRot(player->GetPitch());
            c.GetNetworkManager()->Send(packet);
        };
    // ponytail: the hook nearest the spot cast at is taken for this bot's; two bots fishing the same water may
    // reel in on each other's bite
    const auto biting = [&]()
        {
            auto entities = c.GetEntityManager()->GetEntities();
            for (const auto& [id, e] : *entities)
            {
                if (e && e->GetType() == EntityType::FishingHook && e->GetPosition().SqrDist(target) < 8.0 * 8.0 &&
                    std::static_pointer_cast<FishingHookEntity>(e)->GetDataBiting())
                {
                    return true;
                }
            }
            return false;
        };
    const int before = fish();
    for (int i = 0; i < casts; ++i)
    {
        if (SetItemInHand(c, "minecraft:fishing_rod") == Status::Failure)
        {
            error = "no fishing rod in hand";
            return Status::Failure;
        }
        use();
        Wait(c, 1500);
        Utilities::YieldForCondition(biting, c, 40000);
        // Reeled in whether it bit or not: the hook comes back either way
        use();
        Wait(c, 1500);
    }
    caught = std::max(0, fish() - before);
    Count(c, "fished", caught);
    return Status::Success;
}

enum class Supply
{
    Have,
    // One step was done toward it (crafted something, visited the furnace): look again
    Working,
    // A raw material is missing: a gather goal is set, the gather branch of the tree takes over
    Gathering,
    // Cannot be obtained right now (no sheep in reach): try the other spots of the plan first
    Unavailable,
};
static Supply Obtain(McpBot& c, const std::string& item, const int depth = 0, const bool more = false);
static void ShareSurplus(McpBot& c, const std::string& item);
static Status PourStep(McpBot& c, const Position& p);
static bool TeleportNear(McpBot& c, const int x, const int y, const int z, const bool even_if_near = false);

/// @brief What a blueprint entry asks for: "item", "item[state=value,...]" (stairs facing a given way), and for
/// a sign "item|line 1|line 2|line 3|line 4"
struct Wanted
{
    std::string item;
    std::map<std::string, std::string> state;

    explicit Wanted(const std::string& entry)
    {
        const std::string block = entry.substr(0, entry.find('|'));
        const size_t open = block.find('[');
        item = block.substr(0, open);
        size_t start = open == std::string::npos ? std::string::npos : open + 1;
        while (start != std::string::npos && start < block.size())
        {
            const size_t end = block.find_first_of(",]", start);
            const std::string pair = block.substr(start, end - start);
            const size_t equal = pair.find('=');
            if (equal != std::string::npos)
            {
                state[pair.substr(0, equal)] = pair.substr(equal + 1);
            }
            start = end == std::string::npos || block[end] == ']' ? std::string::npos : end + 1;
        }
    }

    /// @brief True if this block is in the state asked for
    bool StateMatches(const Blockstate& block) const
    {
        if (state.empty())
        {
            return true;
        }
        const std::map<std::string, std::string> actual = block.GetVariables();
        return std::all_of(state.begin(), state.end(), [&](const auto& wanted)
            {
                const auto it = actual.find(wanted.first);
                return it != actual.end() && it->second == wanted.second;
            });
    }
};

/// @brief Build objective: one more step of the blueprint. Each entry says what must stand at a spot:
/// air (clear it), or a block (clear whatever else is there, then place it). Clearing goes first, from the top
/// down; placing follows, from the bottom up; nearest first within a layer.
/// @brief The item with its kind of wood replaced ("minecraft:oak_planks", "spruce" -> "minecraft:spruce_planks");
/// with kind "*", the item with its wood taken out, to compare blocks whatever their wood. "" if it is not made of wood
static std::string WithWood(const std::string& item, const std::string& kind)
{
    // dark_oak and pale_oak before oak, which they contain
    static const char* woods[] = { "dark_oak", "pale_oak", "oak", "spruce", "birch", "jungle", "acacia", "mangrove", "cherry", "crimson", "warped", "bamboo" };
    for (const char* w : woods)
    {
        const size_t at = item.find(w);
        if (at != std::string::npos)
        {
            return item.substr(0, at) + kind + item.substr(at + std::strlen(w));
        }
    }
    return "";
}

/// @brief The same block in the wood the bot carries the most of, for a plan asking for a wood it has none of
/// (oak houses in a spruce forest: the bots wandered for oak with spruce in their bags). "" if none
static std::string OtherWood(BehaviourClient& c, const std::string& item)
{
    std::string best;
    int most = 0;
    for (const char* w : { "oak", "spruce", "birch", "jungle", "acacia", "dark_oak", "mangrove", "cherry", "pale_oak", "crimson", "warped", "bamboo" })
    {
        // Counting the logs and planks it is made from too: spruce logs make spruce planks
        const std::string other = WithWood(item, w);
        const int n = other.empty() ? 0 : CountItem(c, other) + CountItem(c, std::string("minecraft:") + w + "_log") + CountItem(c, std::string("minecraft:") + w + "_planks");
        if (n > most)
        {
            best = other;
            most = n;
        }
    }
    return best;
}

static Status BuildStep(McpBot& c)
{
    c.SetState("building");
    // Placing block after block from one spot, waiting at a furnace: standing still is the work here.
    // (Taken for stuck, the bot would break a block beside it, the wall it just built as likely as not)
    const WorkingInPlace at_work(c);
    Team team;
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        team = c.team;
    }
    const std::shared_ptr<World> world = c.GetWorld();
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    // The workers stick together: each takes the spot nearest to the team leader (the leader, the one nearest
    // to itself), so they work the same part of the site instead of spreading over all of it
    Vector3<double> focus = me;
    {
        Vector3<double> leader;
        if (LeaderPosition(c, leader))
        {
            focus = leader;
        }
    }
    const std::pair<Position, std::string>* next = nullptr;
    const Position* out_of_sight = nullptr;
    std::tuple<int, int, double> best;
    int left = 0;
    const std::chrono::steady_clock::time_point now_time = std::chrono::steady_clock::now();
    for (const auto& entry : team.blueprint)
    {
        const Position& p = entry.first;
        const auto failed = team.build_failed.find(p);
        const bool resting = failed != team.build_failed.end() && !Construction::RetryReady(failed->second.first,
            std::chrono::duration_cast<std::chrono::seconds>(now_time - failed->second.second).count());
        const Blockstate* block = world->GetBlock(p);
        if (block == nullptr)
        {
            // Too far to see: still to do, the bot has to get closer
            ++left;
            if (!resting)
            {
                out_of_sight = out_of_sight == nullptr ? &p : out_of_sight;
            }
            continue;
        }
        const Wanted wanted(entry.second);
        const std::string& wanted_block = wanted.item;
        const bool clear = wanted_block == "minecraft:air";
        // A layer of snow counts as nothing: in a snowy place it comes back as fast as it is cleared
        const bool empty = block->IsAir() || block->IsFluid() || block->GetName() == "minecraft:snow";
        // (A torch that ended up on the side of a block is called wall_torch: it is the torch that was asked for)
        // (and a sign on the side of a block is a wall_sign)
        const bool is_it = (block->GetName() == wanted_block && wanted.StateMatches(*block)) || (wanted_block == "minecraft:water" && block->IsWater()) || (wanted_block == "minecraft:torch" && block->GetName() == "minecraft:wall_torch") ||
            (EndsWith(wanted_block, "_sign") && block->GetName() == wanted_block.substr(0, wanted_block.size() - 5) + "_wall_sign") ||
            // Another wood does as well (see OtherWood)
            (!WithWood(wanted_block, "*").empty() && WithWood(block->GetName(), "*") == WithWood(wanted_block, "*") && wanted.StateMatches(*block));
        if (clear ? empty : is_it)
        {
            team.build_failed.erase(p);
            continue;
        }
        ++left;
        if (resting)
        {
            // Still to do, but not right now
            continue;
        }
        const auto part = wanted.state.find("part");
        if (empty && EndsWith(wanted_block, "_bed") && part != wanted.state.end() && part->second == "head")
        {
            // The foot entry creates this half too; placing a second bed here would move it one block.
            continue;
        }
        const auto half = wanted.state.find("half");
        if (empty && EndsWith(wanted_block, "_door") && half != wanted.state.end() && half->second == "upper")
        {
            // The lower entry creates this half too.
            continue;
        }
        // Something stands where a block has to go: removing it is clearing too
        const bool clearing = !empty;
        // A chest, furnace, crafting table, barrel, bed or door goes on a floor, never in the air: not before the block
        // under it is there
        if (!clearing && (EndsWith(wanted_block, ":chest") || EndsWith(wanted_block, ":barrel") || EndsWith(wanted_block, "furnace") ||
            EndsWith(wanted_block, ":smoker") || EndsWith(wanted_block, ":crafting_table") || EndsWith(wanted_block, "_bed") ||
            EndsWith(wanted_block, "_door") || EndsWith(wanted_block, "_fence_gate") || EndsWith(wanted_block, ":campfire") || EndsWith(wanted_block, ":composter")))
        {
            const Blockstate* under = world->GetBlock(p + Position(0, -1, 0));
            if (under == nullptr || !under->IsSolid())
            {
                continue;
            }
        }
        const std::tuple<int, int, double> rank = { Construction::PlacementStage(clearing, wanted_block), clearing ? -p.y : p.y, focus.SqrDist(Vector3<double>(p.x + 0.5, p.y + 0.5, p.z + 0.5)) };
        if (next == nullptr || rank < best)
        {
            next = &entry;
            best = rank;
        }
    }
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        if (c.team.plan_id != team.plan_id)
        {
            // A new plan was handed over while this one was being looked at
            return Status::Success;
        }
        c.team.build_left = left;
        c.team.build_failed = team.build_failed;
        c.team.build_looked_at = std::chrono::steady_clock::now();
    }
    const auto give_up_on = [&](const Position& p)
        {
            std::lock_guard<std::mutex> lock(c.team_mutex);
            if (c.team.plan_id != team.plan_id) { return; }
            auto& tries = c.team.build_failed[p];
            tries.first = std::min(tries.first + 1, 8);
            tries.second = std::chrono::steady_clock::now();
        };
    // Where to land when teleporting to a spot: just above everything the plan has in that column
    // (the top of the tree to cut down, the top of the wall so far), so the bot never arrives inside a block
    const auto top_of = [&](const Position& spot)
        {
            int top = spot.y;
            for (const auto& entry : team.blueprint)
            {
                if (entry.first.x == spot.x && entry.first.z == spot.z)
                {
                    top = std::max(top, entry.first.y);
                }
            }
            return top + 1;
        };
    if (next == nullptr && out_of_sight != nullptr)
    {
        // Far away: a journey, not a walk. If there is no way for now, the spots are not given up on:
        // the rest of the tree runs for a minute, then the bot tries again
        c.SetNote("building: travelling to the part of the plan that is out of sight, at " + std::to_string(out_of_sight->x) + " " + std::to_string(out_of_sight->z));
        if (!TeleportNear(c, out_of_sight->x, top_of(*out_of_sight), out_of_sight->z) && TravelTask(c, out_of_sight->x, out_of_sight->z) == Status::Failure)
        {
            c.build_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        }
        return Status::Success;
    }
    if (next == nullptr && left > 0)
    {
        // Only spots that just failed are left: let the rest of the tree run, come back to them in a while
        c.build_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        c.SetNote("building: " + std::to_string(left) + " unfinished spots; retrying in a minute");
        return Status::Failure;
    }
    if (next == nullptr)
    {
        std::set<Construction::Cell> solid;
        for (const auto& entry : team.blueprint)
        {
            const Blockstate* block = world->GetBlock(entry.first);
            if (block != nullptr && block->IsSolid()) solid.insert({ entry.first.x, entry.first.y, entry.first.z });
        }
        const auto read = [&](const Construction::Cell& p)
        {
            const Blockstate* block = world->GetBlock(Position(p[0], p[1], p[2]));
            if (block == nullptr) return Construction::Space::Unknown;
            if (block->IsFluid()) return Construction::Space::Hazard;
            const std::string& name = block->GetName();
            const bool door = EndsWith(name, "_door");
            return block->IsAir() || door || (!block->IsFluid() && block->GetCollidersAtPos(Position(p[0], p[1], p[2])).empty()) ? Construction::Space::Open : Construction::Space::Solid;
        };
        const bool road = team.build_name == "road" || team.build_name.rfind("road ", 0) == 0 ||
            EndsWith(team.build_name, " road") || EndsWith(team.build_name, "_road");
        const int narrow = road ? Construction::NarrowRoadCells(solid) : 0;
        const bool sealed = !road && Construction::SealedBuilding(solid, read);
        std::lock_guard<std::mutex> lock(c.team_mutex);
        if (c.team.plan_id != team.plan_id) { return Status::Success; }
        Count(c, "construction_penalty", sealed ? 2 * static_cast<int>(team.blueprint.size()) : 10 * narrow);
        Count(c, "sealed_buildings", sealed ? 1 : 0);
        Count(c, "narrow_road_cells", narrow);
        c.team.building = false;
        c.SetNote(sealed ? "build finished: penalty for no entrance" : narrow > 0 ? "build finished: penalty for one-block road" : "build finished");
        return Status::Success;
    }
    const Position& p = next->first;
    // The block itself, without the text of a sign
    const Wanted wanted_next(next->second);
    std::string item = wanted_next.item;
    if (CountItem(c, item) == 0 && !OtherWood(c, item).empty())
    {
        item = OtherWood(c, item);
    }
    const bool far_sideways = (me.x - p.x) * (me.x - p.x) + (me.z - p.z) * (me.z - p.z) > 32.0 * 32.0;
    if (!far_sideways && std::abs(me.y - p.y) > 20.0)
    {
        // Right above or below, on a hillside: a teleport if the bot may, otherwise the pathfinder gets its chance
        TeleportNear(c, p.x, top_of(p), p.z, true);
    }
    else if (!far_sideways && me.y < p.y - 4 && world->GetSkyLight(BlockPos(me) + Position(0, 1, 0)) == 0 && !c.InBlueprint(BlockPos(me)))
    {
        if (!TeleportNear(c, p.x, top_of(p), p.z, true))
        {
            c.SetNote("building: underground beneath the site, digging up to the surface");
            DigUp(c);
            return Status::Success;
        }
    }
    if (far_sideways)
    {
        // Seen from afar: get to the site first, digging out of wherever the bot is if it has to
        c.SetNote("building: travelling to the site at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z));
        if (!TeleportNear(c, p.x, top_of(p), p.z) && TravelTask(c, p.x, p.z) == Status::Failure)
        {
            // No way there for now: let the rest of the tree run, try again in a minute
            c.build_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        }
        return Status::Success;
    }
    const Blockstate* block = world->GetBlock(p);
    if (block == nullptr)
    {
        // Its chunk went out of view meanwhile (a teleport, the bot walked off): look again next time
        return Status::Success;
    }
    if (!block->IsAir() && !block->IsFluid() && !(item != "minecraft:air" && block->GetName() == "minecraft:snow"))
    {
        // Never what the bots live on
        const std::string& name = block->GetName();
        if (EndsWith(name, ":chest") || EndsWith(name, ":barrel") || EndsWith(name, ":furnace") || EndsWith(name, ":crafting_table") || (EndsWith(name, "_bed") && !EndsWith(item, "_bed")) ||
            block->GetHardness() < 0.0f || SafeDig(c, p, true) == Status::Failure)
        {
            give_up_on(p);
        }
        else
        {
            // Clearing a spot of the plan is building too (the castle's levelling is nothing else)
            Count(c, "built");
        }
        return Status::Success;
    }
    if (item == "minecraft:water")
    {
        // Not a block to place: a bucket to make, fill and empty
        if (PourStep(c, p) == Status::Failure)
        {
            give_up_on(p);
        }
        return c.GetGoal().second > 0 && CountItem(c, "minecraft:bucket") + CountItem(c, "minecraft:water_bucket") == 0 ? Status::Failure : Status::Success;
    }
    // Keep the site lit where the bot works: every 8 seconds it checks the spot it stands on
    if (std::chrono::steady_clock::now() >= c.light_retry_at)
    {
        c.light_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(8);
        LightUp(c, BlockPos(me), true);
    }
    // On site with more than it needs: the surplus goes in the team's chests, for the bots that have none
    ShareSurplus(c, item);
    ShareSurplus(c, "minecraft:cobblestone");
    ShareSurplus(c, "minecraft:coal");
    if (CountItem(c, item) == 0)
    {
        // Out of material: make some (craft, smelt) or go and get it, then carry on here.
        // Failure hands over to the gather branch, which comes next in the tree
        const Supply supply = Obtain(c, item);
        if (supply == Supply::Unavailable)
        {
            give_up_on(p);
        }
        return supply == Supply::Gathering ? Status::Failure : Status::Success;
    }
    const auto facing = wanted_next.state.find("facing");
    if (facing != wanted_next.state.end())
    {
        // Stairs and the like face the way the player looks when placing them: stand on the opposite side,
        // two blocks back, so that looking at the spot is looking that way
        static const std::map<std::string, Position> ways = { { "north", Position(0, 0, -1) }, { "south", Position(0, 0, 1) }, { "west", Position(-1, 0, 0) }, { "east", Position(1, 0, 0) } };
        const auto way = ways.find(facing->second);
        if (way != ways.end())
        {
            GoTo(c, Position(p.x - way->second.x * 2, p.y, p.z - way->second.z * 2), 1, 0, 0);
            const Vector3<double> from = c.GetLocalPlayer()->GetPosition();
            const double along = (p.x + 0.5 - from.x) * way->second.x + (p.z + 0.5 - from.z) * way->second.z;
            const double across = std::abs((p.x + 0.5 - from.x) * way->second.z) + std::abs((p.z + 0.5 - from.z) * way->second.x);
            if (along <= across)
            {
                // Could not get to that side: another try later
                give_up_on(p);
                return Status::Success;
            }
        }
    }
    const std::optional<PlayerDiggingFace> face = (EndsWith(item, "_door") || EndsWith(item, "_fence_gate")) ? std::optional<PlayerDiggingFace>(PlayerDiggingFace::Up) : std::nullopt;
    if (PlaceCheckedBlock(c, item, p, face, true, false, facing == wanted_next.state.end()) == Status::Failure)
    {
        give_up_on(p);
        return Status::Success;
    }
    Count(c, "built");
    if (!wanted_next.state.empty())
    {
        // Did it end up the right way round? If not, take it out: the spot is tried again in a minute
        Wait(c, 300);
        const Blockstate* placed = world->GetBlock(p);
        if (placed != nullptr && placed->GetName() == item && !wanted_next.StateMatches(*placed))
        {
            SafeDig(c, p, true, PlayerDiggingFace::Up, false);
            give_up_on(p);
            return Status::Success;
        }
    }
    if (next->second.find('|') != std::string::npos)
    {
        // A sign: placing it opens it for writing, and the server takes the text from whoever placed it
        std::array<std::string, 4> lines;
        size_t start = next->second.find('|') + 1;
        for (size_t i = 0; i < 4 && start <= next->second.size(); ++i)
        {
            const size_t end = std::min(next->second.find('|', start), next->second.size());
            lines[i] = next->second.substr(start, end - start);
            start = end + 1;
        }
        Wait(c, 300);
        std::shared_ptr<ProtocolCraft::ServerboundSignUpdatePacket> text = std::make_shared<ProtocolCraft::ServerboundSignUpdatePacket>();
        text->SetPos(p.ToNetworkPosition());
        text->SetLines(lines);
        // The front of the sign
        text->SetSlot(0);
        c.GetNetworkManager()->Send(text);
    }
    return Status::Success;
}

/// @brief A filled box
static std::vector<std::pair<Position, std::string>> BoxPlan(const Position& a, const Position& b, const std::string& item)
{
    std::vector<std::pair<Position, std::string>> plan;
    for (int y = std::min(a.y, b.y); y <= std::max(a.y, b.y); ++y)
    {
        for (int x = std::min(a.x, b.x); x <= std::max(a.x, b.x); ++x)
        {
            for (int z = std::min(a.z, b.z); z <= std::max(a.z, b.z); ++z)
            {
                plan.push_back({ Position(x, y, z), item });
            }
        }
    }
    return plan;
}

/// @brief A first-night shelter: a square room, walls 3 high with a flat roof, and a doorway 1 wide and 2 high
/// in the middle of one side. corner is the lowest corner of the walls (ground level), size the outer width
static std::vector<std::pair<Position, std::string>> ShelterPlan(const Position& corner, const int size, const std::string& item)
{
    std::vector<std::pair<Position, std::string>> plan;
    for (int y = 0; y <= 3; ++y)
    {
        for (int x = 0; x < size; ++x)
        {
            for (int z = 0; z < size; ++z)
            {
                const bool wall = x == 0 || z == 0 || x == size - 1 || z == size - 1;
                const bool doorway = z == 0 && x == size / 2 && y <= 1;
                // The roof is the whole top layer, below it only the walls
                if ((y == 3 || wall) && !doorway)
                {
                    plan.push_back({ corner + Position(x, y, z), item });
                }
            }
        }
    }
    return plan;
}

// ---------------------------------------------------------------- recipe book (the game's own data)

// Every crafting recipe of this game version, read from data/ (extracted from the client jar by run.sh):
// what the craft_item and recipe tools use, so nobody has to spell a 3x3 grid by hand.
struct Recipe
{
    std::string result;
    int count = 1;
    // Shaped: rows of the pattern and the items each letter accepts. Shapeless: the items each ingredient accepts
    std::vector<std::string> pattern;
    std::map<char, std::vector<std::string>> key;
    std::vector<std::vector<std::string>> ingredients;
    // As written in the game data ("#minecraft:planks" = any planks), for messages
    std::map<char, std::string> key_text;
    std::vector<std::string> ingredient_text;
};
static std::multimap<std::string, Recipe> recipes;
// Item made in a furnace -> what it is smelted from
static std::map<std::string, std::string> smelted_from;
// The same, as the list of items that give it
static std::map<std::string, std::vector<std::string>> smelt_inputs;
static std::map<std::string, std::vector<std::string>> item_tags;

static void ExpandTag(const std::string& tag, std::vector<std::string>& items, const int depth = 0)
{
    const auto values = item_tags.find(tag);
    if (values == item_tags.end() || depth > 8)
    {
        return;
    }
    for (const std::string& value : values->second)
    {
        if (value.rfind('#', 0) == 0)
        {
            ExpandTag(value.substr(1), items, depth + 1);
        }
        else
        {
            items.push_back(value);
        }
    }
}

/// @brief The items an ingredient accepts: one id, a "#tag", or a list of those
static std::vector<std::string> ExpandIngredient(const Value& v, std::string& text)
{
    std::vector<std::string> items;
    if (v.is_string())
    {
        text += (text.empty() ? "" : " or ") + v.get_string();
        if (v.get_string().rfind('#', 0) == 0)
        {
            ExpandTag(v.get_string().substr(1), items);
        }
        else
        {
            items.push_back(v.get_string());
        }
    }
    else if (v.is_array())
    {
        for (size_t i = 0; i < v.size(); ++i)
        {
            const std::vector<std::string> more = ExpandIngredient(v[i], text);
            items.insert(items.end(), more.begin(), more.end());
        }
    }
    return items;
}

static Value ParseFile(const std::filesystem::path& path)
{
    std::ifstream file(path, std::ios::binary);
    return Json::Parse(std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>()));
}

static void LoadRecipes(const std::string& root)
{
    if (!std::filesystem::is_directory(root + "/recipe"))
    {
        LOG_WARNING("No recipe data in " << root << " (run.sh extracts it): craft_item and recipe are unavailable");
        return;
    }
    for (const auto& entry : std::filesystem::recursive_directory_iterator(root + "/tags/item"))
    {
        if (entry.path().extension() != ".json")
        {
            continue;
        }
        try
        {
            const std::string name = "minecraft:" + std::filesystem::relative(entry.path(), root + "/tags/item").replace_extension().generic_string();
            const Value tag = ParseFile(entry.path());
            for (size_t i = 0; i < tag["values"].size(); ++i)
            {
                const Value& value = tag["values"][i];
                item_tags[name].push_back(value.is_string() ? value.get_string() : value["id"].get_string());
            }
        }
        catch (const std::exception&)
        {
        }
    }
    for (const auto& entry : std::filesystem::directory_iterator(root + "/recipe"))
    {
        try
        {
            const Value v = ParseFile(entry.path());
            const std::string type = v["type"].get_string();
            Recipe recipe;
            recipe.result = v["result"]["id"].get_string();
            recipe.count = v["result"].contains("count") ? v["result"]["count"].get_number<int>() : 1;
            if (type == "minecraft:crafting_shaped")
            {
                for (size_t i = 0; i < v["pattern"].size(); ++i)
                {
                    recipe.pattern.push_back(v["pattern"][i].get_string());
                }
                for (const auto& [letter, ingredient] : v["key"].get_object())
                {
                    recipe.key[letter[0]] = ExpandIngredient(ingredient, recipe.key_text[letter[0]]);
                }
            }
            else if (type == "minecraft:crafting_shapeless")
            {
                for (size_t i = 0; i < v["ingredients"].size(); ++i)
                {
                    std::string text;
                    recipe.ingredients.push_back(ExpandIngredient(v["ingredients"][i], text));
                    recipe.ingredient_text.push_back(text);
                }
            }
            else if (type == "minecraft:smelting")
            {
                for (const std::string& input : ExpandIngredient(v["ingredient"], smelted_from[recipe.result]))
                {
                    smelt_inputs[recipe.result].push_back(input);
                }
                continue;
            }
            else
            {
                continue;
            }
            recipes.insert({ recipe.result, recipe });
        }
        catch (const std::exception&)
        {
            // Special recipes (banners, fireworks...) have no fixed result
        }
    }
    LOG_INFO("Recipe book: " << recipes.size() << " crafting recipes, " << smelted_from.size() << " smelting recipes");
}

/// @brief Lay a recipe out on the crafting grid with what the bot holds.
/// @return false if an ingredient is missing, described in `missing`
static bool FillGrid(BehaviourClient& c, const Recipe& recipe, Grid& grid, std::string& missing)
{
    grid = Grid();
    // Already promised to other cells of this craft
    std::map<std::string, int> used;
    const auto pick = [&](const std::vector<std::string>& accepted, const int needed, const std::string& text) -> std::string
        {
            for (const std::string& item : accepted)
            {
                if (CountItem(c, item) - used[item] >= needed)
                {
                    used[item] += needed;
                    return item;
                }
            }
            missing += (missing.empty() ? "" : ", ") + std::to_string(needed) + " x " + text;
            return "";
        };
    bool complete = true;
    if (!recipe.pattern.empty())
    {
        std::map<char, int> cells;
        for (const std::string& row : recipe.pattern)
        {
            for (const char letter : row)
            {
                cells[letter] += letter != ' ';
            }
        }
        std::map<char, std::string> chosen;
        for (const auto& [letter, accepted] : recipe.key)
        {
            chosen[letter] = pick(accepted, cells[letter], recipe.key_text.at(letter));
            complete = complete && !chosen[letter].empty();
        }
        for (size_t y = 0; y < recipe.pattern.size() && y < 3; ++y)
        {
            for (size_t x = 0; x < recipe.pattern[y].size() && x < 3; ++x)
            {
                grid[y][x] = recipe.pattern[y][x] == ' ' ? "" : chosen[recipe.pattern[y][x]];
            }
        }
        return complete;
    }
    // Shapeless: any arrangement works. The 2x2 corner first, so small recipes need no crafting table
    static const int order[9][2] = { { 0, 0 }, { 0, 1 }, { 1, 0 }, { 1, 1 }, { 0, 2 }, { 1, 2 }, { 2, 0 }, { 2, 1 }, { 2, 2 } };
    for (size_t i = 0; i < recipe.ingredients.size() && i < 9; ++i)
    {
        grid[order[i][0]][order[i][1]] = pick(recipe.ingredients[i], 1, recipe.ingredient_text[i]);
        complete = complete && !grid[order[i][0]][order[i][1]].empty();
    }
    return complete;
}

/// @brief Craft an item once, with whichever of its recipes the inventory can pay for.
/// @return How many were made; 0 with the reason in error
static int CraftOne(BehaviourClient& c, const std::string& item, std::string& error)
{
    Grid grid;
    const Recipe* recipe = nullptr;
    std::string missing;
    const auto range = recipes.equal_range(item);
    for (auto it = range.first; it != range.second && recipe == nullptr; ++it)
    {
        std::string lacking;
        if (FillGrid(c, it->second, grid, lacking))
        {
            recipe = &it->second;
        }
        else
        {
            // An item can have several recipes (sticks: planks, or bamboo): say what each lacks
            missing += (missing.empty() ? "" : "; or ") + lacking;
        }
    }
    if (recipe == nullptr)
    {
        error = range.first == range.second ? "there is no crafting recipe for " + item : "missing ingredients: " + missing;
        return 0;
    }
    return CraftAt(c, grid, error) == Status::Success ? recipe->count : 0;
}

/// @brief Too far from the building site to walk: teleport to x y z. A bot that is an operator does it itself;
/// one that is not asks in the team chat, and any teammate that can teleport brings it over.
/// @return true if the bot is there now
static bool TeleportNear(McpBot& c, const int x, const int y, const int z, const bool even_if_near)
{
    // To the spot itself: sideways and in height
    const auto distance = [&]()
        {
            const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
            return std::sqrt((me.x - x) * (me.x - x) + (me.z - z) * (me.z - z)) + std::abs(me.y - y) / 2.0;
        };
    if ((!even_if_near && distance() < 48.0) || std::chrono::steady_clock::now() < c.teleport_retry_at)
    {
        return false;
    }
    const std::string where = std::to_string(x) + " " + std::to_string(y) + " " + std::to_string(z);
    c.SendChatCommand("minecraft:tp @s " + where);
    if (!Utilities::YieldForCondition([&]() { return distance() < 16.0; }, c, 2500))
    {
        // Refused: this bot is not an operator. A teammate that is can do it
        c.Say("bring me to " + where);
        Utilities::YieldForCondition([&]() { return distance() < 16.0; }, c, 6000);
    }
    if (distance() < 16.0)
    {
        c.SetNote("teleported to the building site at " + where);
        return true;
    }
    // Nobody could: on foot from now on, ask again in two minutes
    c.teleport_retry_at = std::chrono::steady_clock::now() + std::chrono::minutes(2);
    return false;
}

/// @brief The team's chest for this item, if there is one within 96 blocks
static bool SharedChest(McpBot& c, const std::string& item, Position& chest)
{
    // One that a bot saw holding it, wherever that is (within 96 blocks)
    if (CountItem(c, item) == 0 && ChestHolding(c, item, chest))
    {
        return true;
    }
    if (CategoryChest(c, item, chest))
    {
        return true;
    }
    std::lock_guard<std::mutex> lock(chests_mutex);
    LoadChests();
    const auto known = chests.find(item);
    if (known == chests.end() || c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(known->second.x, known->second.y, known->second.z)) > 96.0 * 96.0)
    {
        return false;
    }
    chest = known->second;
    const Blockstate* block = c.GetWorld()->GetBlock(chest);
    return block == nullptr || block->GetName() == "minecraft:chest";
}

/// @brief Take a stack of this item from the team's chest, if it holds some.
/// @return true if the bot now has some
static bool TakeFromChest(McpBot& c, const std::string& item)
{
    Position chest;
    const auto retry = c.chest_retry_at.find(item);
    if ((retry != c.chest_retry_at.end() && std::chrono::steady_clock::now() < retry->second) || !SharedChest(c, item, chest))
    {
        return false;
    }
    // Empty or out of reach: not again for a minute
    c.chest_retry_at[item] = std::chrono::steady_clock::now() + std::chrono::seconds(60);
    if (ReachBlock(c, chest) == Status::Failure || OpenContainer(c, chest) == Status::Failure)
    {
        return false;
    }
    const short container_id = c.GetInventoryManager()->GetFirstOpenedWindowId();
    const std::shared_ptr<Window> window = c.GetInventoryManager()->GetWindow(container_id);
    for (const auto& [index, slot] : window->GetSlots())
    {
        // The chest's own slots come before the player's
        if (index < window->GetFirstPlayerInventorySlot() && !slot.IsEmptySlot() && AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() == item)
        {
            const short first = window->GetFirstPlayerInventorySlot();
            MoveStack(c, container_id, index, EmptySlot(window, first, first + 36));
            break;
        }
    }
    CloseChest(c, container_id, chest);
    const bool got = CountItem(c, item) > 0;
    if (got)
    {
        c.chest_retry_at.erase(item);
        c.SetNote("took " + item + " from the team's chest");
    }
    return got;
}

/// @brief Holding more than two stacks of this item: all but one stack goes in the team's chest for it
/// (placed here if the team has none yet), where the bots that have none will find it
static void ShareSurplus(McpBot& c, const std::string& item)
{
    if (CountItem(c, item) <= 128 || std::chrono::steady_clock::now() < c.share_retry_at)
    {
        return;
    }
    c.share_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(30);
    Position chest;
    std::string error;
    if (!SharedChest(c, item, chest))
    {
        // A new chest: from the inventory, or made from 8 planks
        if (CountItem(c, "minecraft:chest") == 0)
        {
            CraftOne(c, "minecraft:chest", error);
        }
        if (!ChestFor(c, item, chest, error))
        {
            return;
        }
    }
    if (ReachBlock(c, chest) == Status::Failure || OpenContainer(c, chest) == Status::Failure)
    {
        return;
    }
    const short container_id = c.GetInventoryManager()->GetFirstOpenedWindowId();
    const std::shared_ptr<Window> window = c.GetInventoryManager()->GetWindow(container_id);
    int kept = CountItem(c, item);
    int given = 0;
    for (const auto& [index, slot] : window->GetSlots())
    {
        if (index >= window->GetFirstPlayerInventorySlot() && !slot.IsEmptySlot() && kept - slot.GetItemCount() >= 64 &&
            AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() == item)
        {
            if (!MoveStack(c, container_id, index, EmptySlot(window, 0, window->GetFirstPlayerInventorySlot())))
            {
                // The chest is full
                break;
            }
            kept -= slot.GetItemCount();
            given += slot.GetItemCount();
        }
    }
    CloseChest(c, container_id, chest);
    if (given > 0)
    {
        c.SetNote("shared " + std::to_string(given) + " " + item + " in the team's chest");
    }
}

static bool EnsureFuel(McpBot& c, const std::string& stage, bool& gathering)
{
    gathering = false;
    if (CountItem(c, "minecraft:coal") > 0 || CountItem(c, "minecraft:charcoal") > 0 || TakeFromChest(c, "minecraft:coal"))
    {
        c.coal_wanted = false;
        return true;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    if (c.coal_wanted && c.GetGoal().second <= 0)
    {
        // It went for coal and came back with none (none found, or no pickaxe): wood will do for ten minutes
        c.coal_wanted = false;
        c.no_coal_until = now + std::chrono::minutes(10);
    }
    // Wood in the pack burns as well: no trip to a coal seam for it
    if (now < c.no_coal_until || !ItemLike(c, "_planks").empty() || !ItemLike(c, "_log").empty())
    {
        return EnsurePlanks(c, 1, stage);
    }
    if (c.GetGoal().second <= 0)
    {
        c.SetGoal("coal_ore", 8);
        c.explorations = 0;
        c.coal_wanted = true;
    }
    c.SetNote(stage + ": no fuel for the furnace, mining coal");
    gathering = true;
    return false;
}

/// @brief One step toward water standing at p: make a bucket (3 iron ingots, mined and smelted if need be),
/// fill it at the nearest water (found by looking around up to 96 blocks, and walked to), empty it at p.
/// @return Failure only when the pouring itself did not work
static Status PourStep(McpBot& c, const Position& p)
{
    const std::shared_ptr<World> world = c.GetWorld();
    if (CountItem(c, "minecraft:water_bucket") == 0)
    {
        if (CountItem(c, "minecraft:bucket") == 0)
        {
            c.SetNote("building: making a bucket (3 iron ingots)");
            Obtain(c, "minecraft:bucket");
            return Status::Success;
        }
        // Still water, not a stream: only a source fills a bucket
        const auto is_source = [](const Blockstate& b) { return b.IsWater() && !b.IsFluidFalling() && b.GetVariableValue("level") == "0"; };
        const std::vector<Position> sources = FindBlocks(c, is_source, 32, 1);
        if (!sources.empty())
        {
            c.SetNote("building: filling the bucket at " + std::to_string(sources[0].x) + " " + std::to_string(sources[0].y) + " " + std::to_string(sources[0].z));
            UseHeldAt(c, "minecraft:bucket", Vector3<double>(sources[0].x + 0.5, sources[0].y + 0.9, sources[0].z + 0.5));
            Utilities::YieldForCondition([&]() { return CountItem(c, "minecraft:water_bucket") > 0; }, c, 2000);
            return Status::Success;
        }
        // None close: one it remembers, or a look all around, then the walk
        std::vector<std::pair<std::string, Position>> known = Recall(c, "water", 1);
        if (known.empty() && std::chrono::steady_clock::now() >= c.look_retry_at)
        {
            c.look_retry_at = std::chrono::steady_clock::now() + std::chrono::minutes(2);
            LookThroughRock(c, { "water" }, BlockPos(c.GetLocalPlayer()->GetPosition()).y);
            known = Recall(c, "water", 1);
        }
        if (known.empty())
        {
            c.SetNote("building: no water within 96 blocks to fill a bucket");
            c.build_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            return Status::Success;
        }
        c.SetNote("building: going to the water at " + std::to_string(known[0].second.x) + " " + std::to_string(known[0].second.z) + " to fill the bucket");
        if (TravelTask(c, known[0].second.x, known[0].second.z) == Status::Failure)
        {
            Forget(c, known[0].second);
        }
        return Status::Success;
    }
    // Emptied against the top of the block under the spot
    c.SetNote("building: pouring water at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z));
    if (UseHeldAt(c, "minecraft:water_bucket", Vector3<double>(p.x + 0.5, p.y, p.z + 0.5)) == Status::Failure)
    {
        return Status::Failure;
    }
    const bool poured = Utilities::YieldForCondition([&]() { const Blockstate* b = world->GetBlock(p); return b != nullptr && b->IsWater(); }, c, 2000);
    return poured ? Status::Success : Status::Failure;
}

/// @brief One step toward having a building material: craft it, smelt it, make what its recipe lacks, or as a
/// last resort set a gather goal for it. Stone bricks, for example: mine stone, smelt the cobblestone back into
/// stone, craft four bricks from four stone
/// @param more One more round of making it even though the bot has some (a number to reach, not just "some")
static Supply Obtain(McpBot& c, const std::string& item, const int depth, const bool more)
{
    if (!more && CountItem(c, item) > 0)
    {
        return Supply::Have;
    }
    // What a teammate has to spare comes first: quicker than making or mining it
    if (TakeFromChest(c, item))
    {
        return Supply::Working;
    }
    // Then asking: a teammate close by with plenty walks over and drops some at the bot's feet.
    // Once a minute per item; the bot waits a little where it stands
    bool in_team = false;
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        in_team = c.team.building || c.team.members.size() > 1;
    }
    const auto asked = c.ask_retry_at.find(item);
    if (in_team && depth == 0 && !more && (asked == c.ask_retry_at.end() || std::chrono::steady_clock::now() >= asked->second))
    {
        c.ask_retry_at[item] = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        const Position here = BlockPos(c.GetLocalPlayer()->GetPosition());
        c.Say("need " + item + " at " + std::to_string(here.x) + " " + std::to_string(here.y) + " " + std::to_string(here.z));
        c.SetNote("building: out of " + item + ", asked the team for some");
        const WorkingInPlace waiting(c);
        if (Utilities::YieldForCondition([&]() { return CountItem(c, item) > 0; }, c, 12000))
        {
            return Supply::Working;
        }
    }
    const std::string stage = "building";
    std::string error;
    if (EndsWith(item, "_wool"))
    {
        if (item == "minecraft:white_wool" &&
            (CountItem(c, "minecraft:string") >= 4 || (TakeFromChest(c, "minecraft:string") && CountItem(c, "minecraft:string") >= 4)) &&
            CraftOne(c, item, error) > 0)
        {
            return Supply::Working;
        }
        std::atomic<int> obtained = 0;
        if (CountItem(c, "minecraft:shears") > 0 && ShearTask(c, 3, 32.0, obtained) == Status::Success)
        {
            return Supply::Working;
        }
        if (!WildAnimals(c, "sheep", 32.0).empty())
        {
            KillTask(c, "sheep", 1, 32.0, obtained);
            return obtained > 0 ? Supply::Working : Supply::Unavailable;
        }
        if (GoToRemembered(c, "mob:sheep"))
        {
            return Supply::Working;
        }
        Explore(c, "sheep for building wool");
        return Supply::Unavailable;
    }
    if (item.find("minecraft:stripped_") == 0)
    {
        const auto trees = FindBlocks(c, [&](const Blockstate& b) { return EndsWith(b.GetName(), "_log") && b.GetName().find("stripped_") == std::string::npos; }, 32, 1,
            [&](const Position& p) { return !InStoredSite(p) && !c.InSite(p); });
        if (trees.empty())
        {
            Explore(c, "logs to strip for building");
        }
        else if (HeldTool(c, "axe").empty())
        {
            ToolStep(c, false, stage);
        }
        else if (ReachBlock(c, trees[0]) == Status::Success && SetItemInHand(c, HeldTool(c, "axe")) == Status::Success &&
            InteractWithBlock(c, trees[0]) == Status::Success)
        {
            if (Utilities::YieldForCondition([&]() { const auto* b = c.GetWorld()->GetBlock(trees[0]); return b && b->GetName().find("minecraft:stripped_") == 0; }, c, 2000))
            {
                SafeDig(c, trees[0]);
            }
        }
        return Supply::Working;
    }
    if (item == "minecraft:carved_pumpkin")
    {
        if (CountItem(c, "minecraft:shears") == 0)
        {
            if (CraftOne(c, "minecraft:shears", error) == 0) { return Obtain(c, "minecraft:iron_ingot", depth + 1, true); }
            return Supply::Working;
        }
        const auto pumpkins = FindBlocks(c, [](const Blockstate& b) { return b.GetName() == "minecraft:pumpkin"; }, 32, 1,
            [&](const Position& p) { return !InStoredSite(p) && !c.InSite(p); });
        if (pumpkins.empty()) { Explore(c, "pumpkins for the shop"); }
        else if (ReachBlock(c, pumpkins[0]) == Status::Success && SetItemInHand(c, "minecraft:shears") == Status::Success &&
            InteractWithBlock(c, pumpkins[0]) == Status::Success)
        {
            if (Utilities::YieldForCondition([&]() { const auto* b = c.GetWorld()->GetBlock(pumpkins[0]); return b && b->GetName() == item; }, c, 2000))
            {
                SafeDig(c, pumpkins[0]);
            }
        }
        return Supply::Working;
    }
    // An item can have several recipes (sticks: from planks, or from bamboo). The one the bot holds the most
    // ingredients of; failing that, the one made of wood, which is everywhere
    auto recipe = recipes.end();
    {
        int best = -1;
        const auto range = recipes.equal_range(item);
        for (auto it = range.first; it != range.second; ++it)
        {
            std::vector<std::vector<std::string>> needs = it->second.ingredients;
            for (const auto& [letter, accepted] : it->second.key)
            {
                needs.push_back(accepted);
            }
            int score = 0;
            for (const std::vector<std::string>& accepted : needs)
            {
                score += 4 * std::any_of(accepted.begin(), accepted.end(), [&](const std::string& a) { return CountItem(c, a) > 0; });
                score += std::find(accepted.begin(), accepted.end(), "minecraft:oak_planks") != accepted.end();
            }
            if (score > best)
            {
                best = score;
                recipe = it;
            }
        }
    }
    if (recipe != recipes.end())
    {
        // Bigger than 2x2: at a crafting table, made and placed first if none is around
        const bool big = recipe->second.pattern.size() > 2 || recipe->second.ingredients.size() > 4 ||
            std::any_of(recipe->second.pattern.begin(), recipe->second.pattern.end(), [](const std::string& row) { return row.size() > 2; });
        if (big && !EnsureTable(c, stage))
        {
            return c.GetGoal().second > 0 ? Supply::Gathering : Supply::Working;
        }
        if (CraftOne(c, item, error) > 0)
        {
            return Supply::Working;
        }
    }
    const auto sources = smelt_inputs.find(item);
    if (sources != smelt_inputs.end() && item != "minecraft:coal")
    {
        const auto in_hand = std::find_if(sources->second.begin(), sources->second.end(), [&](const std::string& source) { return CountItem(c, source) > 0; });
        Position furnace;
        const bool has_furnace = !FindBlocks(c, [](const Blockstate& b) { return b.GetName() == "minecraft:furnace"; }, 32, 1).empty();
        // With something to smelt, or a furnace that may hold a finished batch (asked once every 20 seconds)
        if (in_hand != sources->second.end() || (has_furnace && std::chrono::steady_clock::now() >= c.furnace_retry_at))
        {
            c.furnace_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            bool for_fuel = false;
            if (EnsureFurnace(c, stage, furnace) && EnsureFuel(c, stage, for_fuel))
            {
                UseFurnace(c, furnace, in_hand != sources->second.end() ? *in_hand : sources->second.front(), stage);
            }
            return for_fuel || c.GetGoal().second > 0 ? Supply::Gathering : Supply::Working;
        }
        if (depth < 3)
        {
            const auto oak = std::find(sources->second.begin(), sources->second.end(), "minecraft:oak_log");
            return Obtain(c, oak != sources->second.end() ? *oak : sources->second.front(), depth + 1, true) == Supply::Gathering ? Supply::Gathering : Supply::Working;
        }
    }
    if (recipe != recipes.end() && depth < 3)
    {
        // Count how many of each ingredient the recipe requires, and obtain more of the first one that is short
        std::vector<std::pair<std::vector<std::string>, int>> wanted;
        for (const std::vector<std::string>& accepted : recipe->second.ingredients)
        {
            auto it = std::find_if(wanted.begin(), wanted.end(), [&](const auto& w) { return w.first == accepted; });
            if (it != wanted.end())
            {
                ++it->second;
            }
            else
            {
                wanted.push_back({ accepted, 1 });
            }
        }
        for (const auto& [letter, accepted] : recipe->second.key)
        {
            int needed = 0;
            for (const std::string& row : recipe->second.pattern)
            {
                needed += static_cast<int>(std::count(row.begin(), row.end(), letter));
            }
            if (needed > 0)
            {
                wanted.push_back({ accepted, needed });
            }
        }
        for (const auto& [accepted, needed] : wanted)
        {
            if (accepted.empty() || std::any_of(accepted.begin(), accepted.end(), [&](const std::string& a) { return CountItem(c, a) >= needed; }))
            {
                continue;
            }
            if (std::find(accepted.begin(), accepted.end(), "minecraft:oak_planks") != accepted.end() && accepted.size() > 1)
            {
                EnsurePlanks(c, std::max(needed, 8), stage);
                return c.GetGoal().second > 0 ? Supply::Gathering : Supply::Working;
            }
            if (std::find(accepted.begin(), accepted.end(), "minecraft:charcoal") != accepted.end() &&
                (!ItemLike(c, "_log").empty() || !ItemLike(c, "_planks").empty()))
            {
                return Obtain(c, "minecraft:charcoal", depth + 1, true) == Supply::Gathering ? Supply::Gathering : Supply::Working;
            }
            const auto held = std::find_if(accepted.begin(), accepted.end(), [&](const std::string& a) { return CountItem(c, a) > 0; });
            const auto oak = std::find(accepted.begin(), accepted.end(), "minecraft:oak_log");
            return Obtain(c, held != accepted.end() ? *held : oak != accepted.end() ? *oak : accepted.front(), depth + 1, true) == Supply::Gathering ? Supply::Gathering : Supply::Working;
        }
    }
    // A raw material: mine it
    if (c.GetGoal().second <= 0)
    {
        const std::string block = item.substr(item.find(':') + 1);
        // Any tree will do for logs: the plan takes another wood (see OtherWood)
        c.SetGoal(block == "cobblestone" ? "stone,cobblestone" : block == "dirt" ? "dirt,grass_block" : block == "coal" ? "coal_ore,deepslate_coal_ore" :
            EndsWith(block, "_log") ? "oak_log,spruce_log,birch_log,jungle_log,acacia_log,dark_oak_log,mangrove_log,cherry_log,pale_oak_log" : block, 32);
        c.explorations = 0;
    }
    c.SetNote("building: out of " + item + ", gathering more");
    return Supply::Gathering;
}

/// @brief Put a torch at this spot (or right beside it) if it is dark there: monsters spawn where no light reaches.
/// Makes torches when it has none: coal or charcoal over a stick gives four
static void LightUp(BehaviourClient& c, const Position& where, const bool on_the_surface_too)
{
    const std::shared_ptr<World> world = c.GetWorld();
    // A light level above 7 means a torch is close already. Underground it is always dark; a working area
    // in the open is lit too, at night (when it matters and when the dark spots show)
    const bool open_sky = world->GetSkyLight(where) > 0;
    if (world->GetBlockLight(where) > 7 || (open_sky && !(on_the_surface_too && IsNightTime(c) == Status::Success)))
    {
        return;
    }
    const std::string torch = "minecraft:torch";
    if (CountItem(c, torch) == 0)
    {
        if (CountItem(c, "minecraft:coal") == 0 && CountItem(c, "minecraft:charcoal") == 0)
        {
            return;
        }
        std::string error;
        if (CountItem(c, "minecraft:stick") == 0)
        {
            CraftOne(c, "minecraft:stick", error);
        }
        if (CraftOne(c, torch, error) == 0)
        {
            return;
        }
    }
    const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
    for (const Position& spot : { where, where + Position(1, 0, 0), where + Position(-1, 0, 0), where + Position(0, 0, 1), where + Position(0, 0, -1) })
    {
        const Blockstate* block = world->GetBlock(spot);
        const Blockstate* floor = world->GetBlock(spot + Position(0, -1, 0));
        // On a floor, and not in the bot's own spot
        McpBot* builder = dynamic_cast<McpBot*>(&c);
        if (spot == feet || (builder && builder->InBlueprint(spot)) || block == nullptr || !block->IsAir() || floor == nullptr || !floor->IsSolid())
        {
            continue;
        }
        if (PlaceBlock(c, torch, spot, std::nullopt, true, false, false) == Status::Success)
        {
            break;
        }
    }
    // Placing put the torches in the hand: back to the pickaxe for the digging that follows
    const std::string pickaxe = HeldTool(c, "pickaxe");
    if (!pickaxe.empty())
    {
        SetItemInHand(c, pickaxe);
    }
}

// ---------------------------------------------------------------- beds: making one, sleeping through the night

static bool IsBed(const Blockstate& b)
{
    return EndsWith(b.GetName(), "_bed");
}

/// @brief True if the bot has a bed to go to: one in sight, one it remembers, or one in its inventory
static bool HasBed(McpBot& c)
{
    // Its own bed (its house's, in the village) counts wherever it is: it goes home to sleep. Unless it is in
    // sight and no bed stands there (a house not built yet): forgotten, and the bot works on through the night
    Position mine;
    if (GetOwn(c, "bed", mine))
    {
        const Blockstate* block = c.GetWorld()->GetBlock(mine);
        if (block != nullptr && !IsBed(*block))
        {
            ForgetOwn(c, "bed");
        }
    }
    if (!ItemLike(c, "_bed").empty() || GetOwn(c, "bed", mine) || !FindBlocks(c, IsBed, 32, 1).empty())
    {
        return true;
    }
    const std::vector<std::pair<std::string, Position>> remembered = Recall(c, "_bed", 1);
    return !remembered.empty() && c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(remembered[0].second.x, remembered[0].second.y, remembered[0].second.z)) < 150.0 * 150.0;
}

/// @brief True if the bot has a bed of its own: the one it placed (still standing), or one in its inventory.
/// Every bot makes its own, a bed takes one sleeper
static bool OwnsBed(McpBot& c)
{
    if (!ItemLike(c, "_bed").empty())
    {
        return true;
    }
    Position mine;
    if (!GetOwn(c, "bed", mine))
    {
        return false;
    }
    // Out of sight it is assumed to still be there
    const Blockstate* block = c.GetWorld()->GetBlock(mine);
    return block == nullptr || IsBed(*block);
}

/// @brief Put the bot's bed item down and remember the bed as its own
static bool PlaceOwnBed(McpBot& c)
{
    Position placed;
    if (!PlaceNearby(c, ItemLike(c, "_bed"), placed))
    {
        return false;
    }
    SetOwn(c, "bed", placed);
    return true;
}

// --no-sleep-rule: leave the server's sleeping rule alone
static bool set_sleep_rule = true;

/// @brief One action toward a bed of the bot's own: 3 wool of one colour from sheep, 3 planks, craft, place
static void BedStep(McpBot& c, const std::string& stage)
{
    const std::string bed_item = ItemLike(c, "_bed");
    if (!bed_item.empty())
    {
        c.SetNote(stage + ": placing the bed");
        Check(c, PlaceOwnBed(c) ? Status::Success : Status::Failure);
        return;
    }
    const std::string wool = ItemLike(c, "_wool", 3);
    if (!wool.empty())
    {
        if (!EnsureTable(c, stage) || !EnsurePlanks(c, 3, stage))
        {
            return;
        }
        // white_wool makes a white_bed: the recipe book knows the layout
        const std::string bed = wool.substr(0, wool.size() - 5) + "_bed";
        c.SetNote(stage + ": crafting a " + bed.substr(bed.find(':') + 1));
        Grid grid;
        std::string missing;
        const auto range = recipes.equal_range(bed);
        if (range.first == range.second || !FillGrid(c, range.first->second, grid, missing))
        {
            Check(c, Status::Failure);
            return;
        }
        std::string error;
        Check(c, CraftAt(c, grid, error));
        return;
    }
    // Wool by shearing when the bot can: shears take 2 iron ingots and leave the sheep alive
    std::string ignored;
    if (CountItem(c, "minecraft:shears") == 0 && CountItem(c, "minecraft:iron_ingot") >= 2)
    {
        c.SetNote(stage + ": crafting shears");
        Check(c, CraftOne(c, "minecraft:shears", ignored) > 0 ? Status::Success : Status::Failure);
        return;
    }
    if (CountItem(c, "minecraft:shears") > 0 && !FindEntities(c, "sheep", 32.0).empty())
    {
        c.explorations = 0;
        c.SetNote(stage + ": shearing sheep for wool");
        std::atomic<int> sheared = 0;
        if (ShearTask(c, 3, 32.0, sheared) == Status::Success)
        {
            return;
        }
        // All shorn already: look for others
    }
    if (WildAnimals(c, "sheep", 32.0).empty())
    {
        if (!Explore(c, "sheep (for the wool of a bed)"))
        {
            SkipStage(c, stage, "no sheep found after exploring");
        }
        return;
    }
    c.explorations = 0;
    c.SetNote(stage + ": hunting sheep for wool");
    std::atomic<int> killed = 0;
    Check(c, KillTask(c, "sheep", 1, 32.0, killed));
}

/// @brief Into bed: armor, the shield and the weapon in hand go into the bag, each remembered with its place
static void Undress(McpBot& c)
{
    std::vector<short> from = { Window::INVENTORY_HEAD_ARMOR, Window::INVENTORY_HEAD_ARMOR + 1, Window::INVENTORY_HEAD_ARMOR + 2,
        Window::INVENTORY_FEET_ARMOR, Window::INVENTORY_OFFHAND_INDEX };
    const short held = Window::INVENTORY_HOTBAR_START + c.GetInventoryManager()->GetIndexHotbarSelected();
    const ProtocolCraft::Slot hand = c.GetInventoryManager()->GetHotbarSelected();
    if (!hand.IsEmptySlot())
    {
        const std::string name = AssetsManager::getInstance().GetItem(hand.GetItemId())->GetName();
        if (EndsWith(name, "_sword") || EndsWith(name, "_axe") || name == "minecraft:trident" || name == "minecraft:mace")
        {
            from.push_back(held);
        }
    }
    for (const short f : from)
    {
        if (IsSlotEmpty(c, f))
        {
            continue;
        }
        short to = -1;
        for (const auto& [index, slot] : PlayerSlots(c))
        {
            if (index >= Window::INVENTORY_STORAGE_START && index < Window::INVENTORY_HOTBAR_START && slot.IsEmptySlot())
            {
                to = index;
                break;
            }
        }
        if (to < 0)
        {
            break;      // A full bag: the rest stays on
        }
        if (SwapItemsInContainer(c, Window::PLAYER_INVENTORY_INDEX, f, to) == Status::Success)
        {
            c.undressed.push_back({ to, f });
        }
    }
}

/// @brief Up again: everything it took off back where it was (the last off, the first on)
static void Dress(McpBot& c)
{
    for (auto it = c.undressed.rbegin(); it != c.undressed.rend(); ++it)
    {
        if (!IsSlotEmpty(c, it->first) && IsSlotEmpty(c, it->second))
        {
            SwapItemsInContainer(c, Window::PLAYER_INVENTORY_INDEX, it->first, it->second);
        }
    }
    c.undressed.clear();
}

/// @brief Night: lie down in a free bed and stay there until morning
static Status SleepStep(McpBot& c)
{
    c.SetState("going to bed");
    std::vector<Position> beds = FindBlocks(c, [](const Blockstate& b) { return IsBed(b) && b.GetVariableValue("occupied") == "false"; }, 32, 64);
    // Its own bed first, wherever it is; any free one otherwise
    Position mine;
    if (GetOwn(c, "bed", mine))
    {
        if (std::find(beds.begin(), beds.end(), mine) == beds.end() && c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(mine.x, mine.y, mine.z)) > 24.0 * 24.0)
        {
            c.SetNote("night: going back to its bed at " + std::to_string(mine.x) + " " + std::to_string(mine.y) + " " + std::to_string(mine.z));
            if (GoTo(c, mine, 8) == Status::Failure && TravelTask(c, mine.x, mine.z) == Status::Failure)
            {
                c.sleep_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
            }
            return Status::Success;
        }
        std::stable_partition(beds.begin(), beds.end(), [&](const Position& p) { return p == mine; });
    }
    const auto bed = std::find_if(beds.begin(), beds.end(), [&](const Position& p) { return c.beds_refused.count(p) == 0; });
    if (bed == beds.end())
    {
        // None free in sight: walk back to one it remembers, or put down its own
        const std::string bed_item = ItemLike(c, "_bed");
        if (beds.empty() && bed_item.empty() && GoToRemembered(c, "_bed"))
        {
            // If that walk leads nowhere, don't try again at once
            c.sleep_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(30);
            return Status::Success;
        }
        if (bed_item.empty() || !PlaceOwnBed(c))
        {
            c.SetNote("night, but no free bed for this bot");
            c.sleep_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(60);
        }
        return Status::Success;
    }
    // The server says why when it refuses: monsters nearby, bed taken, too far
    const long long before = c.GetSystemCount();
    if (ReachBlock(c, *bed) == Status::Failure)
    {
        c.beds_refused.insert(*bed);
        return Status::Success;
    }
    // At the bed: armor, shield and weapon off (back on in the morning, or as soon as it is out of bed: see the
    // first branch of the tree)
    Undress(c);
    if (InteractWithBlock(c, *bed, PlayerDiggingFace::Up) == Status::Failure)
    {
        Dress(c);
        c.beds_refused.insert(*bed);
        return Status::Success;
    }
    Wait(c, 1000);
    for (const std::string& line : c.GetSystemLinesAfter(before))
    {
        if (line.find("block.minecraft.bed.") != std::string::npos || line.find("sleep.not_possible") != std::string::npos)
        {
            Dress(c);
            c.SetNote("could not sleep in the bed at " + std::to_string(bed->x) + " " + std::to_string(bed->y) + " " + std::to_string(bed->z) + ": " + line);
            // Monsters around clear up (the bot fights them); a taken bed stays taken tonight
            if (line.find("not_safe") == std::string::npos)
            {
                c.beds_refused.insert(*bed);
            }
            c.sleep_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(20);
            return Status::Success;
        }
    }
    c.SetState("sleeping");
    c.SetNote("sleeping in the bed at " + std::to_string(bed->x) + " " + std::to_string(bed->y) + " " + std::to_string(bed->z));
    // By default the night is only skipped when every player sleeps: with the rule at 1 percent, one sleeper is
    // enough. Needs operator rights; said once per bot, in both spellings of the rule (it was renamed)
    static bool rule_set = false;
    if (set_sleep_rule && !rule_set)
    {
        rule_set = true;
        c.SendChatCommand("gamerule playersSleepingPercentage 1");
        c.SendChatCommand("gamerule minecraft:players_sleeping_percentage 1");
    }
    // The server wakes everybody when the night is over, or skipped once enough players sleep.
    // Lying still all night is not being stuck
    {
        const WorkingInPlace in_bed(c);
        Utilities::YieldForCondition([&]() { return IsNightTime(c) == Status::Failure; }, c, 10 * 60 * 1000);
    }
    Dress(c);
    c.beds_refused.clear();
    return Status::Success;
}

/// @brief Walk up to another player and drop items at its feet: it picks them up at once. item: an item id,
/// or "food" (anything edible but chicken) or "wood" (logs and planks). to_at: where it was last known to be
static Status GiveTask(McpBot& c, const std::string& to, const Position& to_at, const std::string& item, const int count, std::atomic<int>& given)
{
    c.SetState("giving");
    c.SetNote("taking " + item + " to " + to);
    const auto where = [&]()
        {
            auto entities = c.GetEntityManager()->GetEntities();
            for (const auto& [id, e] : *entities)
            {
                if (e && e->IsRemotePlayer() && c.GetPlayerName(e->GetUUID()) == to)
                {
                    return std::optional<Vector3<double>>(e->GetPosition());
                }
            }
            return std::optional<Vector3<double>>();
        };
    std::optional<Vector3<double>> target = where();
    if (!target)
    {
        // Out of sight: to where it was, then look again
        if (TravelTask(c, to_at.x, to_at.z) == Status::Failure)
        {
            return Status::Failure;
        }
        target = where();
        if (!target)
        {
            return Status::Failure;
        }
    }
    if (GoTo(c, BlockPos(*target), 2) == Status::Failure)
    {
        return Status::Failure;
    }
    const std::optional<Vector3<double>> now = where();
    LookAt(c, now.value_or(*target));
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (given >= count || index < Window::INVENTORY_STORAGE_START || slot.IsEmptySlot())
        {
            continue;
        }
        const std::string name = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName();
        const bool match = item == "food" ? foods.count(name) != 0 : item == "wood" ? EndsWith(name, "_log") || EndsWith(name, "_planks") : name == Id(item);
        if (match)
        {
            const int n = std::min<int>(slot.GetItemCount(), count - given);
            // (the last argument is how many stay in the slot, not how many go)
            if (DropItemsFromContainer(c, Window::PLAYER_INVENTORY_INDEX, index, static_cast<short>(slot.GetItemCount() - n)) == Status::Success)
            {
                given += n;
                Count(c, "given", n);
            }
        }
    }
    c.SetNote("gave " + std::to_string(given.load()) + " " + item + " to " + to);
    return given > 0 ? Status::Success : Status::Failure;
}

/// @brief Throw everything but armor and tools (toward the player `to` if in sight), then stand still for 10 s
static Status DropTask(McpBot& c, const std::string& to, std::atomic<int>& dropped)
{
    static const std::set<std::string> keep = { "minecraft:shears", "minecraft:fishing_rod", "minecraft:flint_and_steel", "minecraft:bow",
        "minecraft:crossbow", "minecraft:trident", "minecraft:mace", "minecraft:elytra" };
    c.SetState("dropping items");
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    Vector3<double> aim = me + Vector3<double>(0.0, 3.0, 2.0);
    auto entities = c.GetEntityManager()->GetEntities();
    for (const auto& [id, e] : *entities)
    {
        if (e && e->IsRemotePlayer() && c.GetPlayerName(e->GetUUID()) == to)
        {
            aim = e->GetPosition() + Vector3<double>(0.0, 1.0, 0.0);
        }
    }
    // Upward and away, or it picks its own items back up
    LookAt(c, aim);
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        if (index < Window::INVENTORY_STORAGE_START || index > Window::INVENTORY_OFFHAND_INDEX || slot.IsEmptySlot())
        {
            continue;
        }
        const std::string item = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName();
        if (!IsGear(item) && keep.count(item) == 0 && DropItemsFromContainer(c, Window::PLAYER_INVENTORY_INDEX, index, 0) == Status::Success)
        {
            ++dropped;
        }
    }
    c.SetNote("dropped " + std::to_string(dropped.load()) + " stacks, standing still for 10 s");
    Wait(c, 10000);
    return Status::Success;
}

/// @brief Answer a call to fight: go where the mobs were seen and take them on, the others doing the same
static Status AssistStep(McpBot& c)
{
    std::string type;
    int count;
    Position at;
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        type = c.assist_type;
        count = c.assist_count;
        at = c.assist_at;
        // One go at it: called again if the fight goes on
        c.assist_count = 0;
    }
    c.SetState("fighting with the others");
    c.SetNote("called to fight " + std::to_string(count) + " " + type + " at " + std::to_string(at.x) + " " + std::to_string(at.y) + " " + std::to_string(at.z));
    // From afar (a pillager calls from 100 blocks): the long way first
    const double away = std::sqrt(c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(at.x, at.y, at.z)));
    if (away > 32.0)
    {
        TravelTask(c, at.x, at.z);
    }
    // It moves: looked for around where it was seen, wider for a pillager (it keeps its distance to shoot)
    const double search = type == "pillager" ? 24.0 : 16.0;
    if (c.GetLocalPlayer()->GetPosition().SqrDist(Vector3<double>(at.x, at.y, at.z)) > 12.0 * 12.0 && GoTo(c, at, 6) == Status::Failure &&
        FindEntities(c, type, search).empty())
    {
        return Status::Failure;
    }
    // Cooperation, for the brain's rewards: it came to the call, and what it killed there
    Count(c, "answered");
    std::atomic<int> killed = 0;
    KillTask(c, type, count, search, killed);
    Count(c, "assisted", killed.load());
    c.SetNote("fought " + type + " with the others: " + std::to_string(killed.load()) + " killed");
    return Status::Success;
}

/// @brief A bot died close by: pick up what it dropped, if anything lies there (nothing does when the server
/// keeps inventories) and it can be walked to. Not next to lava, and nothing is dug or climbed for it
static Status SalvageStep(McpBot& c)
{
    c.salvage = false;
    Position spot;
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        spot = c.salvage_at;
    }
    // Drops take a moment to show up
    Wait(c, 1000);
    bool drops = false;
    {
        auto entities = c.GetEntityManager()->GetEntities();
        for (const auto& [id, e] : *entities)
        {
            drops = drops || (e && e->GetType() == EntityType::ItemEntity && e->GetPosition().SqrDist(Vector3<double>(spot.x + 0.5, spot.y, spot.z + 0.5)) < 36.0);
        }
    }
    const Blockstate* there = c.GetWorld()->GetBlock(spot);
    if (!drops || (there != nullptr && there->IsLava()) || LavaNear(c.GetWorld(), spot))
    {
        return Status::Failure;
    }
    c.SetState("picking up a teammate's items");
    c.SetNote("a bot died at " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z) + ": picking up what it dropped");
    if (GoTo(c, spot, 1) == Status::Failure)
    {
        c.SetNote("can't walk to where a bot died, at " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z));
        return Status::Failure;
    }
    for (const Position& step : { Position(2, 0, 0), Position(0, 0, 2), Position(-2, 0, 0), Position(0, 0, -2), Position(0, 0, 0) })
    {
        GoTo(c, spot + step, 1);
        Wait(c, 300);
    }
    Count(c, "salvaged");
    return Status::Success;
}

/// @brief After a death: back to where it happened, to pick up what the bot dropped. Things on the ground
/// vanish after five minutes, so it teleports if it may and walks otherwise
static Status RecoverStep(McpBot& c)
{
    c.SetState("recovering items");
    const McpBot::Death death = c.GetDeath();
    const Position spot = BlockPos(death.at);
    c.SetNote("died at " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z) + ": going back for what it dropped");
    // One try: whatever happens, the bot does not keep running back
    c.recover_items = false;
    if (!TeleportNear(c, spot.x, spot.y, spot.z, true) && TravelTask(c, death.at.x, death.at.z) == Status::Failure)
    {
        c.SetNote("could not get back to where it died, at " + std::to_string(spot.x) + " " + std::to_string(spot.y) + " " + std::to_string(spot.z));
        return Status::Success;
    }
    // Things scatter a little when they drop: walk over the spot and around it
    for (const Position& step : { Position(0, 0, 0), Position(2, 0, 0), Position(0, 0, 2), Position(-2, 0, 0), Position(0, 0, -2), Position(0, 0, 0) })
    {
        GoTo(c, spot + step, 1);
        Wait(c, 300);
    }
    int carrying = 0;
    for (const auto& [index, slot] : PlayerSlots(c))
    {
        carrying += !slot.IsEmptySlot();
    }
    c.SetNote("back where it died: carrying " + std::to_string(carrying) + " stacks now (" + std::to_string(death.carrying) + " when it died)");
    return Status::Success;
}

// --spacing N: bots keep at least this many blocks between each other (0 turns it off)
static double min_spacing = 3.0;

/// @brief The nearest other bot closer than the minimum spacing: its teammates when in a team, any other player otherwise
static bool CrowdingNeighbour(McpBot& c, Vector3<double>& neighbour)
{
    std::vector<std::string> members;
    {
        std::lock_guard<std::mutex> lock(c.team_mutex);
        members = c.team.members;
    }
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    bool found = false;
    double nearest = min_spacing * min_spacing;
    auto entities = c.GetEntityManager()->GetEntities();
    for (const auto& [id, e] : *entities)
    {
        if (!e || !e->IsRemotePlayer() || (!members.empty() && std::find(members.begin(), members.end(), c.GetPlayerName(e->GetUUID())) == members.end()))
        {
            continue;
        }
        const double distance = me.SqrDist(e->GetPosition());
        if (distance < nearest)
        {
            found = true;
            nearest = distance;
            neighbour = e->GetPosition();
        }
    }
    return found;
}

/// @brief Walk away from a neighbour that is too close, to just beyond the minimum spacing
static Status SpaceOutStep(McpBot& c)
{
    c.SetState("spacing out");
    // Whatever happens, not again for a while: bots working side by side must get work done between two steps
    c.spacing_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    Vector3<double> neighbour;
    if (!CrowdingNeighbour(c, neighbour))
    {
        return Status::Success;
    }
    const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
    double dx = me.x - neighbour.x;
    double dz = me.z - neighbour.z;
    if (dx * dx + dz * dz < 0.01)
    {
        // Standing in the same spot: each bot picks its own way out, from its name
        const double angle = static_cast<double>(std::hash<std::string>()(c.GetNetworkManager()->GetMyName()) % 628) / 100.0;
        dx = std::cos(angle);
        dz = std::sin(angle);
    }
    const double length = std::sqrt(dx * dx + dz * dz);
    const double step = min_spacing + 1.0;
    // Straight away from it, else to either side
    for (const auto& [sx, sz] : { std::pair<double, double>{ dx, dz }, { -dz, dx }, { dz, -dx } })
    {
        if (GoTo(c, BlockPos(Vector3<double>(me.x + sx / length * step, me.y, me.z + sz / length * step)), 1) == Status::Success)
        {
            break;
        }
    }
    return Status::Success;
}

/// @brief Stuck: break one block beside the bot, at head or feet level, starting with the side it faces.
/// Never the block above or below (falling sand, lava, or a pit under the feet), nor something the bots use
static Status UnstickStep(McpBot& c)
{
    c.unstick = false;
    c.SetState("unsticking");
    const std::shared_ptr<World> world = c.GetWorld();
    const std::shared_ptr<LocalPlayer> player = c.GetLocalPlayer();
    const Position feet = BlockPos(player->GetPosition());
    const Vector3<double> front = player->GetFrontVector();
    // Stuck again within two minutes, walled in on 3 sides or more: one block won't do, the bot is in a hole. Dig
    // stairs up and out, by hand if it has no pickaxe (slow, but it gets there). (Not "anything solid overhead": a
    // tree, a roof or a house left up in the air sent bots standing in the open digging stairs)
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const bool again = now - c.last_unstick < std::chrono::minutes(2);
    c.last_unstick = now;
    int walls = 0;
    for (const Position& side : { Position(1, 0, 0), Position(-1, 0, 0), Position(0, 0, 1), Position(0, 0, -1) })
    {
        const Blockstate* foot = world->GetBlock(feet + side);
        const Blockstate* head = world->GetBlock(feet + side + Position(0, 1, 0));
        walls += (foot != nullptr && foot->IsSolid()) || (head != nullptr && head->IsSolid());
    }
    if (again && walls >= 3)
    {
        c.SetNote("stuck in a hole: digging stairs up and out");
        // Each bot its own way (they may all be in the same hole)
        const int way = static_cast<int>(std::hash<std::string>()(c.GetNetworkManager()->GetMyName()) % 4);
        const Position ways[] = { Position(1, 0, 0), Position(0, 0, 1), Position(-1, 0, 0), Position(0, 0, -1) };
        TunnelToward(c, feet + Position(ways[way].x * 12, 12, ways[way].z * 12), 6, true);
        return Status::Success;
    }
    std::array<Position, 4> sides = { Position(1, 0, 0), Position(-1, 0, 0), Position(0, 0, 1), Position(0, 0, -1) };
    std::sort(sides.begin(), sides.end(), [&](const Position& a, const Position& b)
        {
            return a.x * front.x + a.z * front.z > b.x * front.x + b.z * front.z;
        });
    for (const Position& side : sides)
    {
        for (const int dy : { 1, 0 })
        {
            const Position p = feet + side + Position(0, dy, 0);
            const Blockstate* block = world->GetBlock(p);
            if (block == nullptr || !block->IsSolid() || block->GetHardness() < 0.0f)
            {
                continue;
            }
            const std::string& name = block->GetName();
            if (EndsWith(name, ":chest") || EndsWith(name, ":barrel") || EndsWith(name, ":furnace") || EndsWith(name, ":crafting_table") || EndsWith(name, "_bed") ||
                c.IsPlanned(p))
            {
                continue;
            }
            c.SetNote("stuck: breaking the " + name.substr(name.find(':') + 1) + " beside at " + std::to_string(p.x) + " " + std::to_string(p.y) + " " + std::to_string(p.z));
            SafeDig(c, p, true, PlayerDiggingFace::Up, false);
            return Status::Success;
        }
    }
    c.SetNote("stuck, but there is no block beside to break");
    return Status::Success;
}

// ---------------------------------------------------------------- decision tree (autopilot)

// Each branch of the root selector is "condition? -> action", tried in priority order every tick.
// Action leaves publish the bot state; the --gui window shows this tree live with the active node.
static std::shared_ptr<BehaviourTree<McpBot>> MakeAutopilotTree()
{
    return Builder<McpBot>("autopilot")
        .selector("decide next action")
            .leaf("pace", [](McpBot& c)
                {
                    // At most 5 decisions a second. An action that fails instantly (a craft with a missing
                    // ingredient, a block that can't be placed) would otherwise be retried a hundred times a
                    // second, each try sending packets: servers kick clients for that
                    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                    if (now - c.last_decision < std::chrono::milliseconds(200))
                    {
                        Wait(c, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::milliseconds(200) - (now - c.last_decision)).count()));
                    }
                    c.last_decision = std::chrono::steady_clock::now();
                    return Status::Failure;
                })
            .leaf("out of bed with its gear off? back on", [](McpBot& c)
                {
                    // Woken in the night (a monster, the watchdog): it is not sleeping when the tree runs from the top
                    if (!c.undressed.empty())
                    {
                        Dress(c);
                    }
                    return Status::Failure;
                })
            .sequence("unstick")
                .leaf("busy but not moving for 20 seconds?", [](McpBot& c)
                    {
                        return c.unstick ? Status::Success : Status::Failure;
                    })
                .leaf("break a block beside", UnstickStep)
            .end()
            .sequence("survive")
                .leaf("hungry and has food?", [](McpBot& c)
                    {
                        return c.GetLocalPlayer()->GetFood() < 14 && !FindFood(c).empty() ? Status::Success : Status::Failure;
                    })
                .leaf("eat", [](McpBot& c)
                    {
                        c.SetState("eating");
                        return Eat(c, FindFood(c));
                    })
            .end()
            .sequence("defend")
                // 10 blocks: time to put the sword in hand (KillTask does) before the first hit, not after
                // (an enderman right on the bot is one that attacks: it is fought too)
                .leaf("hostile within 10 blocks?", [](McpBot& c)
                    {
                        return FindEntities(c, "hostile", 10.0).empty() && FindEntities(c, "enderman", 2.5).empty() ? Status::Failure : Status::Success;
                    })
                .leaf("fight", [](McpBot& c)
                    {
                        c.SetState("fighting");
                        std::atomic<int> killed = 0;
                        if (!FindEntities(c, "enderman", 2.5).empty())
                        {
                            return KillTask(c, "enderman", 1, 6.0, killed);
                        }
                        return KillTask(c, "hostile", 1, 10.0, killed);
                    })
            .end()
            .sequence("urgent food")
                .leaf("hungry with no food?", [](McpBot& c)
                    {
                        return c.GetLocalPlayer()->GetFood() < 8 && FindFood(c).empty() &&
                            std::chrono::steady_clock::now() >= c.food_retry_at ? Status::Success : Status::Failure;
                    })
                .leaf("get food", GetFoodStep)
            .end()
            .sequence("avenge")
                .leaf("a player attacked one of the bots?", [](McpBot& c)
                    {
                        std::lock_guard<std::mutex> lock(c.team_mutex);
                        return Construction::CanPursuePlayer(c.team.building, c.GetLocalPlayer()->GetFood(), c.GetLocalPlayer()->GetHealth()) &&
                            !c.avenge.empty() && std::chrono::steady_clock::now() < c.avenge_until ? Status::Success : Status::Failure;
                    })
                .leaf("go after that player", [](McpBot& c)
                    {
                        std::string who;
                        Position at;
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            who = c.avenge;
                            at = c.avenge_at;
                        }
                        c.SetState("avenging");
                        c.SetNote("going after " + who + ", who attacked a bot");
                        std::atomic<int> killed = 0;
                        if (KillTask(c, "", 1, 48.0, killed, who) == Status::Success)
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            c.avenge.clear();
                            return Status::Success;
                        }
                        // Not in sight: to where the bot was hit
                        const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
                        if (std::abs(me.x - at.x) > 8.0 || std::abs(me.z - at.z) > 8.0)
                        {
                            return TravelTask(c, at.x, at.z);
                        }
                        return Status::Failure;
                    })
            .end()
            .sequence("assist")
                .leaf("a bot nearby called for help against mobs, and this one is fit to fight?", [](McpBot& c)
                    {
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            if (c.assist_count <= 0 || std::chrono::steady_clock::now() > c.assist_until)
                            {
                                return Status::Failure;
                            }
                        }
                        // Hurt, or with nothing to fight with: it stays out of it
                        // (a pillager: everyone who can still stand a few arrows, 8 health; a builder answers only
                        // that call, the others took it away from the castle all the time)
                        float fit = 12.0f;
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            fit = c.assist_type == "pillager" ? 8.0f : 12.0f;
                            if (c.team.building && c.assist_type != "pillager")
                            {
                                return Status::Failure;
                            }
                        }
                        return c.GetLocalPlayer()->GetHealth() >= fit && (!HeldTool(c, "sword").empty() || !HeldTool(c, "axe").empty()) ? Status::Success : Status::Failure;
                    })
                .leaf("fight them together", AssistStep)
            .end()
            .sequence("recover")
                .leaf("died less than 5 minutes ago and lost what it carried?", [](McpBot& c)
                    {
                        if (!c.recover_items)
                        {
                            return Status::Failure;
                        }
                        // With the server keeping inventories on death there is nothing to go back for:
                        // the bot still has its things. And after five minutes they are gone
                        int carrying = 0;
                        for (const auto& [index, slot] : PlayerSlots(c))
                        {
                            carrying += !slot.IsEmptySlot();
                        }
                        const McpBot::Death death = c.GetDeath();
                        if (carrying >= death.carrying || std::chrono::steady_clock::now() - death.when > std::chrono::minutes(5))
                        {
                            c.recover_items = false;
                            return Status::Failure;
                        }
                        return Status::Success;
                    })
                .leaf("go back for the items", RecoverStep)
            .end()
            .sequence("salvage")
                .leaf("a bot just died close by?", [](McpBot& c) { return c.salvage ? Status::Success : Status::Failure; })
                .leaf("pick up what it dropped", SalvageStep)
            .end()
            .sequence("sleep")
                .leaf("night, and a bed to sleep in?", [](McpBot& c)
                    {
                        if (IsNightTime(c) == Status::Failure)
                        {
                            c.beds_refused.clear();
                            return Status::Failure;
                        }
                        bool building = false;
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            building = c.team.building;
                        }
                        if (building && !OwnsBed(c))
                        {
                            return Status::Failure;
                        }
                        return std::chrono::steady_clock::now() >= c.sleep_retry_at && HasBed(c) ? Status::Success : Status::Failure;
                    })
                .leaf("sleep until morning", SleepStep)
            .end()
            .sequence("spacing")
                .leaf("another bot closer than the minimum distance?", [](McpBot& c)
                    {
                        Vector3<double> neighbour;
                        return min_spacing > 0.0 && std::chrono::steady_clock::now() >= c.spacing_retry_at && CrowdingNeighbour(c, neighbour) ? Status::Success : Status::Failure;
                    })
                .leaf("step away", SpaceOutStep)
            .end()
            .sequence("team")
                .leaf("more than 16 blocks from the team leader?", [](McpBot& c)
                    {
                        Vector3<double> leader;
                        if (!LeaderPosition(c, leader) || std::chrono::steady_clock::now() < c.follow_retry_at)
                        {
                            return Status::Failure;
                        }
                        double allowed = 16.0;
                        {
                            // An order from the leader sends the bot where it has to be
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            if (c.team.has_order)
                            {
                                return Status::Failure;
                            }
                            allowed = c.team.follow_distance;
                        }
                        // Sideways only: a leader mining 30 blocks below is still "with" the bot above
                        const Vector3<double> me = c.GetLocalPlayer()->GetPosition();
                        return (me.x - leader.x) * (me.x - leader.x) + (me.z - leader.z) * (me.z - leader.z) > allowed * allowed ? Status::Success : Status::Failure;
                    })
                .leaf("rejoin the leader", [](McpBot& c)
                    {
                        c.SetState("following leader");
                        Vector3<double> leader;
                        if (LeaderPosition(c, leader) && GoTo(c, BlockPos(leader), 4) == Status::Failure &&
                            TunnelToward(c, BlockPos(leader), 24) == Status::Failure)
                        {
                            // No way to the leader right now: get on with the objective, try again in 30 seconds
                            c.follow_retry_at = std::chrono::steady_clock::now() + std::chrono::seconds(30);
                        }
                        return Status::Success;
                    })
            .end()
            .sequence("give")
                .leaf("teammate nearby asked for a block this bot has plenty of?", [](McpBot& c)
                    {
                        std::lock_guard<std::mutex> lock(c.team_mutex);
                        return c.team.has_request ? Status::Success : Status::Failure;
                    })
                .leaf("bring half a stack over", [](McpBot& c)
                    {
                        c.SetState("giving");
                        std::string item;
                        Position at;
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            c.team.has_request = false;
                            item = c.team.request_item;
                            at = c.team.request_at;
                        }
                        // Right up to the teammate: what is dropped at its feet, it picks up at once
                        if (GoTo(c, at, 1) == Status::Failure)
                        {
                            return Status::Success;
                        }
                        LookAt(c, Vector3<double>(at.x + 0.5, at.y, at.z + 0.5));
                        for (const auto& [index, slot] : PlayerSlots(c))
                        {
                            if (index >= Window::INVENTORY_STORAGE_START && !slot.IsEmptySlot() && slot.GetItemCount() >= 32 &&
                                AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName() == item)
                            {
                                DropItemsFromContainer(c, Window::PLAYER_INVENTORY_INDEX, index, static_cast<short>(slot.GetItemCount() - 32));
                                c.SetNote("gave 32 " + item + " to a teammate");
                                break;
                            }
                        }
                        return Status::Success;
                    })
            .end()
            // Before any work that picks things up (orders, hunting, building, gathering): what is mined with
            // a full inventory is lost
            .sequence("store when full")
                .leaf("chests on and inventory full?", [](McpBot& c)
                    {
                        return auto_store && InventoryFull(c) && std::chrono::steady_clock::now() >= c.store_retry_at ? Status::Success : Status::Failure;
                    })
                .leaf("store in chests", [](McpBot& c)
                    {
                        // A builder full of what it dug (levelling is mostly dirt and stone) throws the junk away
                        // and builds on: walking to a chest per kind of item took the builders all hour, and
                        // the castle never moved
                        bool builder = false;
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            builder = c.team.building;
                        }
                        if (builder)
                        {
                            MakeRoom(c);
                            if (!InventoryFull(c))
                            {
                                return Status::Success;
                            }
                        }
                        c.SetState("storing");
                        std::atomic<int> stored = 0;
                        std::string error;
                        // Nothing that could be stored (all gear, no chest, chests out of reach): go on working
                        // for 5 minutes rather than try again every tick
                        if (StoreTask(c, false, stored, error) == Status::Success && stored > 0)
                        {
                            return Status::Success;
                        }
                        c.store_retry_at = std::chrono::steady_clock::now() + std::chrono::minutes(5);
                        // Still full: room for what it picks up and crafts meanwhile, the junk goes
                        MakeRoom(c);
                        c.SetNote("could not store" + (error.empty() ? std::string() : ": " + error) + "; trying again in 5 minutes");
                        return Status::Failure;
                    })
            .end()
            .sequence("orders")
                .leaf("order from the team leader?", [](McpBot& c)
                    {
                        std::lock_guard<std::mutex> lock(c.team_mutex);
                        return c.team.has_order ? Status::Success : Status::Failure;
                    })
                .leaf("go where the leader said", [](McpBot& c)
                    {
                        c.SetState("following orders");
                        Position at;
                        std::string what;
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            at = c.team.order_at;
                            what = c.team.order_what;
                        }
                        c.SetNote("leader's order: " + (what.empty() ? std::string("come") : "mine " + what) + " at " + std::to_string(at.x) + " " + std::to_string(at.y) + " " + std::to_string(at.z));
                        // On foot, then by tunnel if it has a pickaxe. Once there (or if there is no way), the order is done:
                        // the place is in the bot's memory, its own search takes over
                        if (GoTo(c, at, 6) == Status::Failure)
                        {
                            TunnelToward(c, at, 48);
                        }
                        std::lock_guard<std::mutex> lock(c.team_mutex);
                        c.team.has_order = false;
                        return Status::Success;
                    })
            .end()
            .sequence("hunt")
                .leaf("team attack objective set?", [](McpBot& c)
                    {
                        std::lock_guard<std::mutex> lock(c.team_mutex);
                        return c.team.hunt_left > 0 ? Status::Success : Status::Failure;
                    })
                .leaf("hunt one", HuntStep)
            .end()
            .sequence("ready to build")
                .leaf("team build objective set, and missing its tools or a chest?", [](McpBot& c)
                    {
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            if (!c.team.building)
                            {
                                return Status::Failure;
                            }
                        }
                        return ReadyToBuild(c) ? Status::Failure : Status::Success;
                    })
                .leaf("get ready to build", ReadyStep)
            .end()
            .sequence("build")
                .leaf("team build objective set?", [](McpBot& c)
                    {
                        std::lock_guard<std::mutex> lock(c.team_mutex);
                        if (Construction::ShouldFinishGatherFirst(c.team.building, c.GetGoal().second))
                        {
                            return Status::Failure;
                        }
                        return c.team.building && std::chrono::steady_clock::now() >= c.build_retry_at ? Status::Success : Status::Failure;
                    })
                .leaf("place one block", BuildStep)
            .end()
            .sequence("make")
                .leaf("asked to make something it does not have enough of yet?", [](McpBot& c)
                    {
                        const auto [item, count] = c.GetMake();
                        if (count <= 0)
                        {
                            return Status::Failure;
                        }
                        if (CountItem(c, item) >= count)
                        {
                            c.SetNote("made: has " + std::to_string(CountItem(c, item)) + " " + item);
                            c.SetMake("", 0);
                            return Status::Failure;
                        }
                        return Status::Success;
                    })
                .leaf("one step toward it", [](McpBot& c)
                    {
                        c.SetState("making");
                        const auto [item, count] = c.GetMake();
                        // Craft, smelt, or make what the recipe lacks. A raw material missing: the gather
                        // branch, next in the tree, goes for it
                        return Obtain(c, item, 0, true) == Supply::Gathering ? Status::Failure : Status::Success;
                    })
            .end()
            .sequence("gather")
                .leaf("resource goal set?", [](McpBot& c)
                    {
                        return c.GetGoal().second > 0 ? Status::Success : Status::Failure;
                    })
                .leaf("collect one", [](McpBot& c)
                    {
                        c.SetState("gathering");
                        const auto [spec, left] = c.GetGoal();
                        const auto give_up = [&](const std::string& why)
                            {
                                c.SetNote("gave up gathering " + spec + ": " + why);
                                c.SetGoal(spec, 0);
                                c.needs_store = true;
                                return Status::Failure;
                            };
                        std::vector<std::string> wanted = SplitSpec(spec);
                        const bool ore = std::any_of(wanted.begin(), wanted.end(), [](const std::string& item) { return EndsWith(item, "_ore"); });
                        if (ore)
                        {
                            if (HeldTool(c, "pickaxe").empty())
                            {
                                // Not cancelled: failing here lets the tool branches further down make one, then back here
                                c.SetNote("gathering " + spec + " needs a pickaxe: making one first");
                                return Status::Failure;
                            }
                            // Gold, diamond, redstone drop nothing without an iron pickaxe: leave them for when the bot has one
                            if (HeldTool(c, "pickaxe", iron_tier).empty())
                            {
                                wanted.erase(std::remove_if(wanted.begin(), wanted.end(), NeedsIronPickaxe), wanted.end());
                                if (wanted.empty())
                                {
                                    c.SetNote("gathering " + spec + " needs an iron pickaxe: making one first");
                                    return Status::Failure;
                                }
                            }
                            SetItemInHand(c, HeldTool(c, "pickaxe"));
                        }
                        else if (EndsWith(wanted.front(), "_log"))
                        {
                            if (!HeldTool(c, "axe").empty())
                            {
                                SetItemInHand(c, HeldTool(c, "axe"));
                            }
                            const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
                            if (feet.y < 62 && c.GetWorld()->GetSkyLight(feet + Position(0, 1, 0)) == 0)
                            {
                                c.SetNote("gathering " + spec + ": underground, digging up to the surface");
                                DigUp(c);
                                return Status::Success;
                            }
                        }
                        else if (wanted.front() == "stone" || wanted.front() == "cobblestone")
                        {
                            // By hand stone breaks slowly and drops nothing
                            if (HeldTool(c, "pickaxe").empty())
                            {
                                c.SetNote("gathering " + spec + " needs a pickaxe: making one first");
                                return Status::Failure;
                            }
                            SetItemInHand(c, HeldTool(c, "pickaxe"));
                        }
                        const auto match = [&](const Blockstate& b) { return MatchesAny(b.GetName(), wanted); };
                        const auto gatherable = [&](const Position& p) { return !InStoredSite(p) && !c.InSite(p) && c.unreachable.count(p) == 0; };
                        if (FindBlocks(c, match, 32, 1, gatherable).empty())
                        {
                            if (ore)
                            {
                                return SeekOre(c, wanted) ? Status::Success : give_up("none found after digging for it");
                            }
                            const std::string remembered = EndsWith(wanted.front(), "_log") && wanted.size() > 1 ? "_log" : wanted.front();
                            if (GoToRemembered(c, remembered))
                            {
                                return Status::Success;
                            }
                            return Explore(c, wanted.front()) ? Status::Success : give_up("none found after exploring. Is the block id right?");
                        }
                        c.explorations = 0;
                        std::atomic<int> collected = 0;
                        const bool tunnel = ore || ((wanted.front() == "stone" || wanted.front() == "cobblestone") && !HeldTool(c, "pickaxe").empty());
                        // Found but not reachable: drop the goal instead of retrying forever
                        if (CollectTask(c, match, 1, 32, collected, tunnel) == Status::Failure)
                        {
                            if (!FindBlocks(c, match, 32, 1, gatherable).empty())
                            {
                                return Status::Success;
                            }
                            return give_up("the ones in range can't be reached or mined");
                        }
                        const int got = std::max(1, collected.load());
                        const int remaining = std::max(0, left - got);
                        c.SetNote("gathering " + spec + ", " + std::to_string(remaining) + " left");
                        c.SetGoal(spec, remaining);
                        // Store once the whole goal is done, not after every block
                        c.needs_store = c.needs_store || remaining == 0;
                        return Status::Success;
                    })
            .end()
            .sequence("food")
                .leaf("has no food?", [](McpBot& c)
                    {
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            if (c.team.building)
                            {
                                return Status::Failure;
                            }
                        }
                        return FindFood(c).empty() && std::chrono::steady_clock::now() >= c.food_retry_at ? Status::Success : Status::Failure;
                    })
                .leaf("get food", GetFoodStep)
            .end()
            .sequence("out of the mine")
                .leaf("underground with no pickaxe nor wood to make one?", [](McpBot& c)
                    {
                        const Position feet = BlockPos(c.GetLocalPlayer()->GetPosition());
                        return HeldTool(c, "pickaxe").empty() && ItemLike(c, "_log").empty() && ItemLike(c, "_planks", 5).empty() &&
                            feet.y < 62 && c.GetWorld()->GetSkyLight(feet + Position(0, 1, 0)) == 0 ? Status::Success : Status::Failure;
                    })
                .leaf("dig stairs up to the surface", [](McpBot& c)
                    {
                        c.SetState("digging out of the mine");
                        c.SetNote("underground with no pickaxe: digging stairs up to the surface");
                        DigUp(c);
                        return Status::Success;
                    })
            .end()
            .sequence("wood")
                .leaf("has no wood?", [](McpBot& c)
                    {
                        return c.gear && !c.IsGearSkipped("wood") && !HasWood(c) ? Status::Success : Status::Failure;
                    })
                .leaf("cut a tree", [](McpBot& c)
                    {
                        c.SetState("getting wood");
                        MineOne(c, [](const Blockstate& b) { return EndsWith(b.GetName(), "_log"); }, "trees", "axe", "wood", "_log");
                        return Status::Success;
                    })
            .end()
            .sequence("pickaxe")
                .leaf("has no pickaxe?", LacksTool, "pickaxe")
                .leaf("make pickaxe", MakeToolStep, "pickaxe")
            .end()
            .sequence("axe")
                .leaf("has no axe?", LacksTool, "axe")
                .leaf("make axe", MakeToolStep, "axe")
            .end()
            .sequence("sword")
                .leaf("has no sword?", LacksTool, "sword")
                .leaf("make sword", MakeToolStep, "sword")
            .end()
            .sequence("shovel")
                .leaf("has no shovel?", LacksTool, "shovel")
                .leaf("make shovel", MakeToolStep, "shovel")
            .end()
            .sequence("better tools")
                .leaf("still missing stone tools or an iron pickaxe?", [](McpBot& c)
                    {
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            if (c.team.building)
                            {
                                return Status::Failure;
                            }
                        }
                        return c.gear && !ToolStage(c).empty() ? Status::Success : Status::Failure;
                    })
                .leaf("next tool step", [](McpBot& c)
                    {
                        c.SetState("gearing up");
                        return GearStep(c, ToolStage(c));
                    })
            .end()
            .sequence("farm")
                .leaf("ripe crops within 16 blocks?", [](McpBot& c)
                    {
                        return RipeCrops(c, 16, 1).empty() ? Status::Failure : Status::Success;
                    })
                .leaf("harvest and replant", [](McpBot& c)
                    {
                        c.SetState("farming");
                        std::atomic<int> harvested = 0, replanted = 0;
                        const Status result = FarmTask(c, 16, true, harvested, replanted);
                        c.needs_store = c.needs_store || harvested > 0;
                        return result;
                    })
            .end()
            .sequence("store")
                .leaf("chests on and something to store?", [](McpBot& c)
                    {
                        // (not a builder: what it gathered is for the build)
                        bool builder = false;
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            builder = c.team.building;
                        }
                        if (builder)
                        {
                            c.needs_store = false;
                        }
                        return auto_store && c.needs_store ? Status::Success : Status::Failure;
                    })
                .leaf("store in chests", [](McpBot& c)
                    {
                        c.SetState("storing");
                        c.needs_store = false;
                        std::atomic<int> stored = 0;
                        std::string error;
                        return StoreTask(c, false, stored, error);
                    })
            .end()
            .sequence("armor")
                .leaf("still missing a bed, leather armor or a shield?", [](McpBot& c)
                    {
                        {
                            std::lock_guard<std::mutex> lock(c.team_mutex);
                            if (c.team.building)
                            {
                                return Status::Failure;
                            }
                        }
                        if (c.gear && ToolStage(c).empty() && ArmorStage(c).empty() && c.GetGearSkipped().empty())
                        {
                            // Everything is made. (Stages given up on are tried again after five minutes:
                            // while there are some, gearing up stays on)
                            c.gear = false;
                        }
                        return c.gear && !ArmorStage(c).empty() ? Status::Success : Status::Failure;
                    })
                .leaf("next armor step", [](McpBot& c)
                    {
                        c.SetState("getting armor");
                        return GearStep(c, ArmorStage(c));
                    })
            .end()
            .leaf("idle", [](McpBot& c)
                {
                    return IdleTask(c);
                })
        .end();
}

void McpBot::ResumeDefault()
{
    static const std::shared_ptr<BehaviourTree<McpBot>> autopilot_tree = MakeAutopilotTree();
    static const std::shared_ptr<BehaviourTree<McpBot>> idle_tree = Builder<McpBot>("waiting for commands")
        .leaf("idle", [](McpBot& c)
            {
                return IdleTask(c);
            });
    SetBehaviourTree(autopilot ? autopilot_tree : idle_tree);
}

/// @brief Run a task as a one-shot tree on the bot's behaviour thread and block until it is done,
/// then hand the bot back to its default tree
template<typename F>
static bool Run(McpBot& bot, const int timeout_s, const std::string& what, F&& f)
{
    std::lock_guard<std::mutex> lock(bot.run_mutex);
    const auto success = std::make_shared<std::atomic<bool>>(false);
    // 0: running, 1: success, 2: failure. Published by the very last leaf, after the tree swap
    // is requested, so a following Run can't have its tree overwritten by this one
    const auto result = std::make_shared<std::atomic<int>>(0);
    bot.SetBehaviourTree(Builder<McpBot>("tool: " + what)
        .sequence()
            .leaf("set state", [what](McpBot& c) { c.SetState("tool: " + what); return Status::Success; })
            .selector()
                .sequence()
                    .leaf(what, std::forward<F>(f))
                    .leaf("succeeded", [success](McpBot&) { *success = true; return Status::Success; })
                .end()
                .leaf("failed", [](McpBot&) { return Status::Success; })
            .end()
            .leaf("back to default behaviour", [success, result](McpBot& c)
                {
                    c.ResumeDefault();
                    *result = *success ? 1 : 2;
                    return Status::Success;
                })
        .end());
    Utilities::WaitForCondition([&]() { return *result != 0 || bot.GetShouldBeClosed(); }, timeout_s * 1000);
    if (*result == 0)
    {
        bot.ResumeDefault();
    }
    return *result == 1;
}

template<typename F>
static Value Act(McpBot& bot, const int timeout_s, const std::string& what, F&& f)
{
    if (!Run(bot, timeout_s, what, std::forward<F>(f)))
    {
        throw std::runtime_error(what + " failed (unreachable, missing item, or timed out)");
    }
    return what + " done";
}

// ---------------------------------------------------------------- tools

static const char* tools_json = R"JSON([
{"name":"status","description":"Bot position, health, food, game mode, dimension, time of day, held item, online players, current state, autopilot settings and the recent flow of state transitions.","inputSchema":{"type":"object","properties":{}}},
{"name":"chat","description":"Send a chat message. A message starting with / is sent as a server command.","inputSchema":{"type":"object","properties":{"message":{"type":"string"}},"required":["message"]}},
{"name":"read_chat","description":"Return chat/system messages received since the last call.","inputSchema":{"type":"object","properties":{}}},
{"name":"inventory","description":"List the bot's inventory slots (item name and count).","inputSchema":{"type":"object","properties":{}}},
{"name":"entities","description":"List nearby entities, nearest first. type: empty for all, an entity name like zombie or cow, or a group: hostile, animal, player.","inputSchema":{"type":"object","properties":{"type":{"type":"string"},"radius":{"type":"number","default":32}}}},
{"name":"get_block","description":"Name and state of the block at a position. Optional y_min scans down to the first solid block, returning its y.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"y_min":{"type":"number"}},"required":["x","y","z"]}},
{"name":"find_blocks","description":"Find the nearest loaded blocks with an exact block id (e.g. oak_log, diamond_ore, crafting_table), or every id ending in a suffix that starts with an underscore (e.g. _log for all logs). Radius is capped at 32.","inputSchema":{"type":"object","properties":{"block":{"type":"string"},"radius":{"type":"number","default":32},"max":{"type":"number","default":10}},"required":["block"]}},
{"name":"goto","description":"Pathfind and walk to a block position. tolerance = how many blocks away is close enough.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"tolerance":{"type":"number","default":0}},"required":["x","y","z"]}},
{"name":"look_at","description":"Turn the bot's head toward a position.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"]}},
{"name":"dig","description":"Break the block at a position (walks there if needed).","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"]}},
{"name":"place_block","description":"Place a block item from the inventory at a position (walks there if needed).","inputSchema":{"type":"object","properties":{"item":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["item","x","y","z"]}},
{"name":"interact_block","description":"Right-click the block at a position (button, lever, door, chest...). Walks there if needed.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["x","y","z"]}},
{"name":"use_item","description":"Use an item while looking at a point (walks within reach first): how a bucket is filled (x y z of a still water block, y its surface) and emptied (x z of the spot, y the top of the block the water goes on). item: what to use, default what is in hand. For buttons, doors and chests use interact_block instead.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"item":{"type":"string"}},"required":["x","y","z"]}},
{"name":"equip","description":"Put an inventory item in the bot's main hand, or wear it with slot=armor.","inputSchema":{"type":"object","properties":{"item":{"type":"string"},"slot":{"type":"string","enum":["hand","armor"]}},"required":["item"]}},
{"name":"chests","description":"What the bots know is in the chests: every bot that opens a chest (to store, take or look for room) tells the others what it holds, and chests and barrels seen while scanning are reported as finds (recall kind chest). With item: the chests holding it, fullest first; without: every known chest, newest first, with its contents and who saw it last. The bots themselves go to a chest known to hold what they need.","inputSchema":{"type":"object","properties":{"item":{"type":"string"},"max":{"type":"number","default":20}}}},
{"name":"home","description":"Give the bot a bed of its own (the one in its house): at night it goes back there to sleep, from wherever it is. forget=true drops it (the bot is moving away).","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"forget":{"type":"boolean"}}}},
{"name":"eat","description":"Eat a food item from the inventory.","inputSchema":{"type":"object","properties":{"item":{"type":"string"}},"required":["item"]}},
{"name":"collect","description":"Collect resources: repeatedly find the nearest block with this exact id, mine it and pick up the drop, until count is reached. Equip a suitable tool first. For a log the whole tree is cut (every connected log within reach, so the result can exceed count), and a sapling of that kind is replanted if the bot has one.","inputSchema":{"type":"object","properties":{"block":{"type":"string"},"count":{"type":"number","default":1},"radius":{"type":"number","default":32}},"required":["block"]}},
{"name":"kill","description":"Hunt and attack entities until count are dead. type: an entity name like zombie or cow, or a group: hostile, animal. Equip a weapon first. Penned animals (a fence, gate, wall or iron bars within 8 blocks) are never attacked.","inputSchema":{"type":"object","properties":{"type":{"type":"string"},"count":{"type":"number","default":1},"radius":{"type":"number","default":24}},"required":["type"]}},
{"name":"make","description":"Have the bot make an item until it holds count of it, by whatever it takes: craft it, smelt it, make what the recipe lacks, mine or cut the raw materials, build a furnace or a crafting table if needed. Works for anything with a recipe or a smelting source. Glass: make glass (sand is dug, a furnace built, coal mined for fuel); glass panes: make glass_pane (6 glass give 16). Also stone, stone_bricks, bricks, charcoal, iron_ingot, bucket, torch, chest... Returns at once: the autopilot does the work, status shows the progress. count 0 drops it.","inputSchema":{"type":"object","properties":{"item":{"type":"string"},"count":{"type":"number","default":1}},"required":["item"]}},
{"name":"fish","description":"Fish with a rod at the nearest open water within 32 blocks: cast, wait for a bite, reel in, count times (default 5, each up to 40 s). Makes a fishing rod (3 sticks, 2 string) if the bot has none. Returns how many fish it caught.","inputSchema":{"type":"object","properties":{"count":{"type":"number","default":5}}}},
{"name":"milk","description":"Milk cows: an empty bucket on up to count cows within radius, a milk bucket from each; the cows are unharmed. Makes a bucket from 3 iron ingots if the bot has none.","inputSchema":{"type":"object","properties":{"count":{"type":"number","default":1},"radius":{"type":"number","default":24}}}},
{"name":"shear","description":"Shear sheep for wool without killing them: up to count sheep within radius, 1 to 3 wool each, picked up. Needs shears; makes them from 2 iron ingots if the bot has none. The sheep grow their wool back by grazing, so a pen of sheep is a wool farm.","inputSchema":{"type":"object","properties":{"count":{"type":"number","default":4},"radius":{"type":"number","default":32}}}},
{"name":"dye","description":"Dye wool. color: white, orange, magenta, light_blue, yellow, lime, pink, gray, light_gray, cyan, purple, blue, brown, green, red or black. By default crafts count dyed wool from wool the bot carries (one dye each). sheep=true dyes count sheep instead: each then gives wool of that colour every time it is shorn. The dye comes from the inventory, or is made from what the bot carries (red from a poppy, yellow from a dandelion, white from bone meal...).","inputSchema":{"type":"object","properties":{"color":{"type":"string"},"count":{"type":"number","default":1},"sheep":{"type":"boolean","default":false}},"required":["color"]}},
{"name":"lure","description":"Lead animals to a place, for a pen or a farm: with their food in hand (cow, sheep, goat: wheat; pig, rabbit: carrot; chicken: wheat_seeds; or food=...), the bot goes up to the nearest ones of this type within 48 blocks and walks to x y z in short hops, waiting for them and going back for them when they lose interest. Returns how many arrived. Then close the pen and use breed.","inputSchema":{"type":"object","properties":{"type":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"food":{"type":"string"}},"required":["type","x","y","z"]}},
{"name":"breed","description":"Feed the two nearest animals of a type with a food item from the inventory so they breed (e.g. cow + wheat, pig + carrot, chicken + wheat_seeds).","inputSchema":{"type":"object","properties":{"type":{"type":"string"},"food":{"type":"string"},"radius":{"type":"number","default":16}},"required":["type","food"]}},
{"name":"farm","description":"Harvest every fully grown crop nearby (wheat, carrots, potatoes, beetroots, nether_wart), pick up the drops and replant.","inputSchema":{"type":"object","properties":{"radius":{"type":"number","default":16},"replant":{"type":"boolean","default":true}}}},
{"name":"craft","description":"Craft one recipe. grid is 3 rows of 3 item names, \"\" for empty cells, e.g. sticks: [[\"oak_planks\",\"\",\"\"],[\"oak_planks\",\"\",\"\"],[\"\",\"\",\"\"]]. Recipes that fit in 2x2 use the inventory grid; larger ones use the nearest crafting_table.","inputSchema":{"type":"object","properties":{"grid":{"type":"array","items":{"type":"array","items":{"type":"string"}}}},"required":["grid"]}},
{"name":"autopilot","description":"Turn the bot's decision tree on or off. When on, the bot picks its next action itself, in priority order: eat when hungry, fight hostile mobs within 8 blocks, hunt when it has no food, cut a tree when it has no wood, make a pickaxe / axe / sword / shovel when it has none (planks, crafting table and sticks as needed), then gear up further (mine stone for stone tools, hunt cows for leather armor, mine and smelt iron for a shield), harvest and replant ripe crops within 16 blocks, gather the requested resource, otherwise idle. Other tools still work: they interrupt it, then it resumes. gather: optional block id to collect, or several separated by commas (the bot explores to find it, and for ores digs down and tunnels), count: how many. gear: false to skip gearing up, true to (re)start it; see last_note in status for its progress.","inputSchema":{"type":"object","properties":{"enabled":{"type":"boolean"},"gear":{"type":"boolean"},"gather":{"type":"string"},"count":{"type":"number","default":16}},"required":["enabled"]}},
{"name":"snapshot","description":"Image of what the bot sees right now (first-person view). Only available when the server was started with --snapshot FILE.","inputSchema":{"type":"object","properties":{}}},
{"name":"team","description":"Make all the connected bots work as a team: they turn their autopilot on, share one objective and travel together (each bot rejoins the leader when more than 16 blocks away). objective: gather (block: one id or several separated by commas, e.g. iron_ore,gold_ore,diamond_ore,redstone_ore; count: total, split between the bots; for ores the bots first make stone tools and an iron pickaxe, then tunnel down to the ore, deepslate variants included, lighting their tunnels and mining spots with torches made from the coal they find), attack (type: entity name or hostile/animal, count: total), build (plan=box: fill the box x1 y1 z1 - x2 y2 z2 with the block item; plan=blocks: any structure given block by block, see blocks; a bot that runs out of a block goes and gathers more of it; plan=shelter: a first-night shelter of outer width size (default 5) with its lowest corner at x1 y1 z1, walls 3 high, flat roof, doorway in the middle of the north side; each bot uses what it carries), follow (just stay with the leader), disband. leader: bot name, defaults to the first bot. The bots talk in the game chat with lines starting with [team]: members report what they find, the leader assigns two of them to each find and asks everyone for a status every minute (see team_reports in the leader's status). Bots still eat, defend themselves and gear up first; pass gear=false to autopilot to skip gearing up.","inputSchema":{"type":"object","properties":{"objective":{"type":"string","enum":["gather","attack","build","follow","disband"]},"leader":{"type":"string"},"block":{"type":"string"},"type":{"type":"string"},"item":{"type":"string"},"plan":{"type":"string","enum":["box","shelter","blocks"]},"site":{"type":"array","description":"the building site as a box [x1, y1, z1, x2, y2, z2]: the bots never mine material inside it","items":{"type":"number"}},"blocks":{"type":"array","description":"plan=blocks: [[x, y, z, item], ...], up to 4096 entries; the item air clears the spot, any other block replaces what stands there","items":{"type":"array"}},"size":{"type":"number","default":5},"count":{"type":"number","default":16},"x1":{"type":"number"},"y1":{"type":"number"},"z1":{"type":"number"},"x2":{"type":"number"},"y2":{"type":"number"},"z2":{"type":"number"}},"required":["objective"]}},
{"name":"recipe","description":"How an item is made, from the game's own recipe data: every crafting recipe (pattern and key, or ingredient list; #minecraft:planks means any item of that tag), how many it makes, whether it needs a crafting table, and what it is smelted from if it comes from a furnace.","inputSchema":{"type":"object","properties":{"item":{"type":"string"}},"required":["item"]}},
{"name":"craft_item","description":"Craft an item by name, count times: finds the recipe, lays it out with what the bot holds (any accepted variant, e.g. any planks), in the inventory grid or at the nearest crafting table. Fails with the list of missing ingredients. Prefer this to craft, which needs the 3x3 grid spelled out.","inputSchema":{"type":"object","properties":{"item":{"type":"string"},"count":{"type":"number","default":1}},"required":["item"]}},
{"name":"make_farm","description":"Start a crop plot: till the dirt and grass within radius (max 4, the reach of water) of the nearest water that has soil beside it, and plant the seeds the bot carries (wheat_seeds, carrot, potato, beetroot_seeds). Needs a hoe (craft_item wooden_hoe). One water block in the middle keeps a 9x9 plot hydrated. The farm tool harvests and replants later.","inputSchema":{"type":"object","properties":{"radius":{"type":"number","default":4}}}},
{"name":"locate","description":"Ask the server where the nearest biome, structure or point of interest is, with its /locate command (the bot must be a server operator). kind: biome, structure or poi. name: its id, e.g. forest, plains, village_plains, mineshaft, or a #tag like #minecraft:village. go=true also walks there. Good for going straight to resources: forest for wood, village for food and beds, mineshaft for ores and rails.","inputSchema":{"type":"object","properties":{"kind":{"type":"string","enum":["biome","structure","poi"]},"name":{"type":"string"},"go":{"type":"boolean","default":false}},"required":["kind","name"]}},
{"name":"travel","description":"Walk a long distance to x z (any height), in hops, for places too far for goto. Fails if the way is blocked three hops in a row.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"z":{"type":"number"}},"required":["x","z"]}},
{"name":"recall","description":"Places the bot remembers having seen (kept in its own SQLite database across restarts): ores, logs, crafting tables, furnaces, chests, barrels, beds, crops, berry bushes and animals (mob:cow), with what the other bots reported (ores, logs, crops, berry bushes, animals). kind is the end of a block id: iron_ore (also finds deepslate_iron_ore), _log, bed, crafting_table, chest... Nearest first. status lists how many of each kind are remembered.","inputSchema":{"type":"object","properties":{"kind":{"type":"string"},"max":{"type":"number","default":5}},"required":["kind"]}},
{"name":"schematic","description":"Materials and building plan of a structure from createmod.com. url: the link of a schematic page (https://createmod.com/schematics/<name>): returns its title, size, required mods and material list, and which materials this game does not have (blocks of other mods). The site gives the block-by-block file to players only, so the user downloads the .nbt from that page into bot/schematics/; then file=<name>.nbt returns the materials and the number of parts, and with x y z (the corner with the lowest coordinates) the team builds part `part` (default 1, 4000 blocks each, bottom up) there, gathering and crafting what it needs; blocks of other mods are left out.","inputSchema":{"type":"object","properties":{"url":{"type":"string"},"file":{"type":"string"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"part":{"type":"number"}}}},
{"name":"give","description":"The bot walks up to another player (a bot) and drops items at its feet, which it picks up. to: the player's name; item: an item id, food (anything edible) or wood (logs and planks), default food; count (default 16); x y z: where the player is, if out of sight.","inputSchema":{"type":"object","properties":{"to":{"type":"string"},"item":{"type":"string"},"count":{"type":"number"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"}},"required":["to"]}},
{"name":"drop","description":"Throw every item on the ground except armor and tools (weapons, shears, rods...), toward the player `to` if in sight, then stand still for 10 seconds before going back to its work. Returns how many stacks were dropped.","inputSchema":{"type":"object","properties":{"to":{"type":"string"}}}},
{"name":"reconnect_all","description":"Every bot stores everything and disconnects, then the same bots join again 10 seconds apart, each from the binary on disk (a new build without stopping the program). Returns at once; it runs in the background.","inputSchema":{"type":"object","properties":{}}},
{"name":"disconnect_all","description":"Every bot stores everything in the chests and disconnects, all at once. The program keeps running: connect brings bots back. None rejoins by itself.","inputSchema":{"type":"object","properties":{}}},
{"name":"leader","description":"The leader the bots elected (every 10 Minecraft days, see the brain tool): it leads the team (the team tool's leader when none is given). With name, sets it.","inputSchema":{"type":"object","properties":{"name":{"type":"string"}}}},
{"name":"feeders","description":"Bots whose whole job is to feed the others: each hunts (cows, pigs, sheep in turn, from memory first), cuts wood, and takes food to the hungriest bot (food 8 or less), and logs to bots that have no wood. bots: their names, or count (default 3) to take the best fed ones; stop=true gives them back; with no argument, who they are and what they did.","inputSchema":{"type":"object","properties":{"bots":{"type":"array","items":{"type":"string"}},"count":{"type":"number"},"stop":{"type":"boolean"}}}},
{"name":"sites","description":"The stored construction boxes: every build (team build, castle stages) records the box around it in the shared database, and from then on no bot breaks a block inside one (gathering, tunnelling, getting unstuck, the dig tool), except a build whose own plan covers the spot. Lists them; with x1 y1 z1 x2 y2 z2 (and name) adds one by hand; remove=n forgets the n-th of the list (from 1).","inputSchema":{"type":"object","properties":{"x1":{"type":"number"},"y1":{"type":"number"},"z1":{"type":"number"},"x2":{"type":"number"},"y2":{"type":"number"},"z2":{"type":"number"},"name":{"type":"string"},"remove":{"type":"number"}}}},
{"name":"mines","description":"The entrances of the team's mines: the places where a bot started digging down from the surface for ore (recorded automatically, one per 24 blocks). With x y z, declares one by hand. The castle plugin builds roads to them.","inputSchema":{"type":"object","properties":{"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"resources":{"type":"string","description":"what the mine gives, e.g. iron_ore,coal_ore"}}}},
{"name":"vault","description":"The vault: one chest per category of item (food, weapons, armor, equipment, valuables, building, materials). With category and x y z, declares the chest of a category; with nothing, lists them. Once declared, bots within 96 blocks store by category there instead of in a chest per item, and look there first for what they need. The result says full when a bot found a category's chests full on every floor: the castle plugin then digs another floor underneath.","inputSchema":{"type":"object","properties":{"category":{"type":"string","description":"food, weapons, armor, equipment, valuables, materials or building; with #2, #3... for the same category on a lower floor"},"x":{"type":"number"},"y":{"type":"number"},"z":{"type":"number"},"full":{"type":"boolean","description":"false clears the 'vault is full' mark, once a floor has been added"}}}},
{"name":"store","description":"Put the inventory in chests, one chest per item type. A missing chest is placed next to the bot from a chest item of its inventory, and remembered. By default the hotbar and the chest items are kept; everything=true empties the whole inventory.","inputSchema":{"type":"object","properties":{"everything":{"type":"boolean","default":false}}}},
{"name":"bots","description":"List the connected bots (name and state). Every other tool accepts an optional bot argument with one of these names and defaults to the first bot.","inputSchema":{"type":"object","properties":{}}},
{"name":"spawn","description":"Queue 1 to 100 additional bots with unused numbered names (base_name1, base_name2, ...), spaced by the configured join delay. Returns the queued names; bots shows progress.","inputSchema":{"type":"object","properties":{"count":{"type":"integer","minimum":1,"maximum":100},"base_name":{"type":"string","minLength":1,"maxLength":15,"pattern":"^[A-Za-z0-9_]+$","default":"Bot"}},"required":["count"]}},
{"name":"connect","description":"Connect one more bot to the server under this player name.","inputSchema":{"type":"object","properties":{"name":{"type":"string"}},"required":["name"]}},
{"name":"disconnect","description":"Make a bot store its whole inventory in chests, then disconnect it from the server.","inputSchema":{"type":"object","properties":{}}}
])JSON";

// Where the first bot's view is saved every 5 seconds, empty if snapshots are off
static std::string snapshot_path;

static std::string Base64(const std::string& data)
{
    static const char* alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string output;
    output.reserve((data.size() + 2) / 3 * 4);
    for (size_t i = 0; i < data.size(); i += 3)
    {
        const unsigned int n = (static_cast<unsigned char>(data[i]) << 16) |
            (i + 1 < data.size() ? static_cast<unsigned char>(data[i + 1]) << 8 : 0) |
            (i + 2 < data.size() ? static_cast<unsigned char>(data[i + 2]) : 0);
        output += alphabet[(n >> 18) & 63];
        output += alphabet[(n >> 12) & 63];
        output += i + 1 < data.size() ? alphabet[(n >> 6) & 63] : '=';
        output += i + 2 < data.size() ? alphabet[n & 63] : '=';
    }
    return output;
}

// Botcraft allows one renderer (window, view) per process. So that every bot has its own view,
// only the first bot lives in this process: each other one is a child bot_mcp process, and the
// tool calls that target it are forwarded to it over its stdin/stdout, in MCP like any client would.
struct BotSlot
{
    std::shared_ptr<McpBot> bot;
    // Ticks the bot's behaviour tree
    std::thread stepper;
};
struct Child
{
    std::string name;
    pid_t pid = -1;
    FILE* to = nullptr;
    FILE* from = nullptr;
    // One request at a time on the pipe
    std::mutex mutex;
    // What the panel shows while the child is busy running a tool. Read and written by several threads
    // (web requests, rejoin), some of them while the pipe mutex is held by another: it has its own lock
    Value last_status;
    std::mutex status_mutex;

    Value GetLastStatus()
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        return last_status;
    }

    void SetLastStatus(const Value& status)
    {
        std::lock_guard<std::mutex> lock(status_mutex);
        last_status = status;
    }
};
// The local bots, then the children, oldest first. With a window or views each bot needs its own process (one
// renderer per process): one local bot, the others children. Headless, every bot is local and they all share
// one world (Botcraft's shared world, behind its own lock): one copy of the chunks and of the game assets
static std::vector<BotSlot> bots;
static std::vector<std::shared_ptr<Child>> children;
static std::mutex bots_mutex;
static std::shared_ptr<World> shared_world;

/// @brief The local bot of this name, nullptr if none. The caller holds bots_mutex
static std::shared_ptr<McpBot> LocalBot(const std::string& name)
{
    for (const BotSlot& slot : bots)
    {
        if (slot.bot->GetNetworkManager() && slot.bot->GetNetworkManager()->GetMyName() == name)
        {
            return slot.bot;
        }
    }
    return nullptr;
}

/// @brief Take a local bot out of the game: its threads stopped, its memory closed, its chunks let go of
static void LeaveGame(BotSlot& slot)
{
    const std::thread::id loader = slot.bot->GetNetworkManager() ? slot.bot->GetNetworkManager()->GetProcessingThreadId() : std::thread::id();
    slot.bot->SetShouldBeClosed(true);
    if (slot.stepper.joinable())
    {
        slot.stepper.join();
    }
    slot.bot->Disconnect();
    CloseMemory(*slot.bot);
    if (shared_world)
    {
        // The chunks only this bot had loaded: a shared world keeps every other bot's
        shared_world->UnloadAllChunks(loader);
    }
}

/// @brief Chat is read from one local bot: another one when it leaves. The caller holds bots_mutex
static void NewChatSource()
{
    chat_source = bots.empty() ? nullptr : bots.back().bot.get();
}
static std::string server_address = "192.168.100.78:25565";
static bool start_autopilot = false;
static bool show_gui = false;
// No window, no views, no MCP on stdin: runs as a service until SIGINT or SIGTERM
static bool headless = false;
static volatile std::sig_atomic_t quit_signal = 0;
// The program is stopping: the plugins' calls fail at once, so a plugin halfway through a round over every bot
// lets go instead of holding the stop up for minutes
static std::atomic<bool> stopping = false;
// A second stop signal: at once. The main program takes its children along (the same process group), a child
// only itself
static bool QuitSignalChild();
static void QuitSignal(int)
{
    if (quit_signal)
    {
        kill(QuitSignalChild() ? getpid() : 0, SIGKILL);
    }
    quit_signal = 1;
}
// --child: this process is one of the extra bots of another bot_mcp
static bool is_child = false;
static bool QuitSignalChild() { return is_child; }
// Our executable, to start the processes of the other bots
static std::string self_path;
// Bots that were kicked or whose process died, waiting to join again: name -> when, and what to restore
struct Rejoin
{
    std::chrono::steady_clock::time_point at;
    // Arguments of the autopilot tool that put the bot back to what it was doing
    Value restore;
};
static std::map<std::string, Rejoin> rejoining;
static int spawn_delay_seconds = 5;
constexpr std::chrono::seconds rejoin_delay(60);
// Name of the leader of the current team, "" if the bots are not teamed up (main process only)
static std::string team_leader;

/// @brief Where a bot's view is saved: the --snapshot file for the first bot, FILE.NAME.png for the others
static std::string SnapshotPathFor(const std::string& child_name)
{
    if (snapshot_path.empty() || child_name.empty())
    {
        return snapshot_path;
    }
    const std::filesystem::path path(snapshot_path);
    return (path.parent_path() / (path.stem().string() + "." + child_name + path.extension().string())).string();
}

/// @brief Run a tool in a child process. The caller holds child.mutex
static Value ForwardLocked(Child& child, const std::string& name, const Value& a)
{
    Value request;
    request["jsonrpc"] = "2.0";
    request["id"] = 1;
    request["method"] = "tools/call";
    request["params"]["name"] = name;
    request["params"]["arguments"] = a.is_object() ? a : Value(Json::Object());
    request["params"]["arguments"].get_object().erase("bot");
    char* line = nullptr;
    size_t capacity = 0;
    // Closed by a goodbye while this caller (a /state poll) still held a copy of the child: those FILEs are freed
    if (child.to == nullptr || fprintf(child.to, "%s\n", request.Dump().c_str()) < 0 || fflush(child.to) != 0 || getline(&line, &capacity, child.from) < 0)
    {
        free(line);
        throw std::runtime_error("the process of bot " + child.name + " is gone (kicked or crashed): disconnect it, then connect it again");
    }
    const Value response = Json::Parse(std::string(line));
    free(line);
    const Value& content = response["result"]["content"][0];
    if (content["type"].get_string() == "image")
    {
        Value image;
        image["__image"] = content["data"];
        return image;
    }
    const std::string& text = content["text"].get_string();
    if (response["result"]["isError"].get<bool>())
    {
        throw std::runtime_error(text);
    }
    // Results are JSON dumps, or plain sentences
    try
    {
        return Json::Parse(text);
    }
    catch (const std::exception&)
    {
        return text;
    }
}

static Value Forward(Child& child, const std::string& name, const Value& a)
{
    std::lock_guard<std::mutex> lock(child.mutex);
    return ForwardLocked(child, name, a);
}

/// @brief A child's status without waiting behind the tool it may be running
static Value ChildStatus(Child& child)
{
    std::unique_lock<std::mutex> lock(child.mutex, std::try_to_lock);
    if (!lock)
    {
        Value busy = child.GetLastStatus();
        if (!busy.is_object())
        {
            busy = Value(Json::Object());
            busy["name"] = child.name;
        }
        busy["state"] = "busy (running a tool)";
        return busy;
    }
    Value status;
    try
    {
        status = ForwardLocked(child, "status", Value(Json::Object()));
    }
    catch (const std::exception&)
    {
        status = Value(Json::Object());
        status["name"] = child.name;
        status["state"] = "kicked or crashed, rejoins after 60 seconds";
    }
    child.SetLastStatus(status);
    return status;
}

/// @brief The child a tool call targets, nullptr if it targets the local bot
static std::shared_ptr<Child> TargetChild(const Value& a)
{
    std::lock_guard<std::mutex> lock(bots_mutex);
    if (a.is_object() && a.contains("bot") && a["bot"].is_string() && !a["bot"].get_string().empty())
    {
        for (const std::shared_ptr<Child>& child : children)
        {
            if (child->name == a["bot"].get_string())
            {
                return child;
            }
        }
        return nullptr;
    }
    return bots.empty() && !children.empty() ? children.front() : nullptr;
}

/// @brief The local bot, if the tool call targets it
static std::shared_ptr<McpBot> TargetBot(const Value& a)
{
    std::lock_guard<std::mutex> lock(bots_mutex);
    const bool named = a.is_object() && a.contains("bot") && a["bot"].is_string() && !a["bot"].get_string().empty();
    const std::shared_ptr<McpBot> found = named ? LocalBot(a["bot"].get_string()) : (bots.empty() ? nullptr : bots.front().bot);
    if (!found)
    {
        throw std::runtime_error(named ? "no bot named " + a["bot"].get_string() + ", see the bots tool" : "no bot is connected, use the connect tool");
    }
    return found;
}

/// @brief A tree leaf (walking, mining, hunting...) runs until it is done, so the decision tree can't react meanwhile.
/// This restarts the tree from its root when a monster comes close, so the bot fights back instead of walking
/// on while being hit, and when an action has been running for too long (stuck pathfinding...)
static void Watchdog(McpBot& bot)
{
    if (!bot.autopilot || bot.GetNetworkManager()->GetConnectionState() != ProtocolCraft::ConnectionState::Play)
    {
        return;
    }
    // Hurt with a pillager close by: it is attacking. Every bot within 100 blocks is called to it at once (and
    // this one takes it on too); once it is dead each goes back to what it was doing
    {
        const float health = bot.GetLocalPlayer()->GetHealth();
        const auto now = std::chrono::steady_clock::now();
        if (health < bot.last_health && now >= bot.pillager_call_at)
        {
            const std::vector<Ent> pillagers = FindEntities(bot, "pillager", 24.0);
            if (!pillagers.empty())
            {
                const Position at = BlockPos(pillagers.front().pos);
                bot.pillager_call_at = now + std::chrono::seconds(15);
                bot.Say("mob pillager " + std::to_string(pillagers.size()) + " at " + std::to_string(at.x) + " " + std::to_string(at.y) + " " + std::to_string(at.z));
                std::lock_guard<std::mutex> team_lock(bot.team_mutex);
                bot.assist_type = "pillager";
                bot.assist_count = static_cast<int>(pillagers.size());
                bot.assist_at = at;
                bot.assist_until = now + std::chrono::seconds(180);
                bot.assist_interrupted = false;
            }
        }
        bot.last_health = health;
    }
    // A tool call owns the tree: leave it alone
    std::unique_lock<std::mutex> lock(bot.run_mutex, std::try_to_lock);
    const std::string state = bot.GetState();
    if (!lock || state.rfind("tool:", 0) == 0)
    {
        return;
    }
    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
    const bool danger = state != "fighting" && state != "eating" && now >= bot.danger_retry_at && !FindEntities(bot, "hostile", 10.0).empty();
    const bool in_place = bot.working_in_place || now - bot.worked_in_place_at.load() < std::chrono::seconds(5);
    const bool stuck = state != "idle" && !in_place && now - bot.GetStateSince() > std::chrono::seconds(120);
    // Busy but still within 1.5 blocks of the same spot after 20 seconds: walled in, or the pathfinder is bumping into something
    const Vector3<double> position = bot.GetLocalPlayer()->GetPosition();
    if (position.SqrDist(bot.watch_position) > 1.5 * 1.5 || state == "idle" || state == "eating" || state == "starting" || state == "unsticking" || in_place)
    {
        bot.watch_position = position;
        bot.moved_at = now;
    }
    const bool blocked = now - bot.moved_at > std::chrono::seconds(20);
    if (blocked)
    {
        bot.unstick = true;
        ++bot.times_stuck;
        bot.moved_at = now;
    }
    // A call to fight: drop what it is doing now, not when its current action ends (minutes, for a walk)
    bool called = false;
    if (state != "fighting" && state != "fighting with the others" && state != "eating")
    {
        std::lock_guard<std::mutex> team_lock(bot.team_mutex);
        if (bot.assist_count > 0 && now < bot.assist_until && !bot.assist_interrupted &&
            (!bot.team.building || bot.assist_type == "pillager") &&
            bot.GetLocalPlayer()->GetHealth() >= (bot.assist_type == "pillager" ? 8.0f : 12.0f))
        {
            bot.assist_interrupted = true;
            called = true;
        }
    }
    if (danger || stuck || blocked || called)
    {
        LOG_INFO("Watchdog: interrupting \"" << state << "\": " << (called ? "called to a fight" : danger ? "monster nearby" : blocked ? "not moving, breaking a block beside" : "running for too long"));
        // If the monster can't be reached the fight ends at once: don't interrupt again right away
        bot.danger_retry_at = now + std::chrono::seconds(10);
        bot.RestartStateClock();
        bot.ResumeDefault();
    }
}

static void SpawnChild(const std::string& login)
{
    std::vector<std::string> arguments = { self_path, "--child", "--address", server_address, "--login", login };
    if (start_autopilot) { arguments.push_back("--autopilot"); }
    if (auto_store) { arguments.push_back("--chests"); }
    if (!set_sleep_rule) { arguments.push_back("--no-sleep-rule"); }
    arguments.push_back("--spacing");
    arguments.push_back(std::to_string(min_spacing));
    if (show_gui) { arguments.push_back("--gui"); }
    if (!snapshot_path.empty())
    {
        arguments.push_back("--snapshot");
        arguments.push_back(SnapshotPathFor(login));
    }
    std::vector<char*> argv;
    for (std::string& argument : arguments)
    {
        argv.push_back(argument.data());
    }
    argv.push_back(nullptr);

    int to_child[2];
    int from_child[2];
    if (pipe(to_child) != 0 || pipe(from_child) != 0)
    {
        throw std::runtime_error("can't create the pipes of bot " + login);
    }
    const pid_t pid = fork();
    if (pid == 0)
    {
        // Our MCP channel was moved away from fd 1 at startup: the child gets its own on 0 and 1, logs stay on 2
        dup2(to_child[0], STDIN_FILENO);
        dup2(from_child[1], STDOUT_FILENO);
        for (int fd = 3; fd < 1024; ++fd)
        {
            close(fd);
        }
        execv(argv[0], argv.data());
        _exit(127);
    }
    close(to_child[0]);
    close(from_child[1]);
    std::shared_ptr<Child> child = std::make_shared<Child>();
    child->name = login;
    child->pid = pid;
    child->to = fdopen(to_child[1], "w");
    child->from = fdopen(from_child[0], "r");
    // It only answers once it has joined the server; it exits if it can't
    try
    {
        for (int attempt = 0;; ++attempt)
        {
            try
            {
                child->SetLastStatus(Forward(*child, "status", Value(Json::Object())));
                break;
            }
            catch (const std::exception& e)
            {
                // Joined but still receiving the world: ask again. A dead process is another story
                if (attempt >= 20 || std::string(e.what()).find("not in the world yet") == std::string::npos)
                {
                    throw;
                }
                Utilities::SleepFor(std::chrono::milliseconds(500));
            }
        }
    }
    catch (const std::exception&)
    {
        fclose(child->to);
        fclose(child->from);
        waitpid(pid, nullptr, 0);
        throw std::runtime_error("could not join the server as " + login + " (name already in use, server unreachable, or connecting too soon after another bot: servers throttle joins, 4 seconds by default)");
    }
    std::lock_guard<std::mutex> lock(bots_mutex);
    children.push_back(child);
}

static void ServeSocket(const std::string& path, const std::string& bot);
static Value CallTool(const std::string& name, const Value& a);

// The bots to start next time when no --bots or --login is given: bot/bots.txt, one name a line. Bots added
// (spawn, connect) go in, a bot disconnected on its own goes out; leaving all at once (quitting, disconnect_all,
// reconnect_all) keeps the list
static std::mutex roster_mutex;
static const char* const ROSTER_FILE = "../bots.txt";

static std::vector<std::string> LoadRoster()
{
    std::vector<std::string> names;
    std::ifstream file(ROSTER_FILE);
    for (std::string line; std::getline(file, line);)
    {
        if (!line.empty()) { names.push_back(line); }
    }
    return names;
}

static void SaveRoster(const std::vector<std::string>& names)
{
    std::ofstream file(ROSTER_FILE, std::ios::trunc);
    for (const std::string& n : names) { file << n << '\n'; }
}

static void RosterChange(const std::string& name, const bool add)
{
    if (is_child || name.empty()) { return; }
    std::lock_guard<std::mutex> lock(roster_mutex);
    std::vector<std::string> names = LoadRoster();
    const auto it = std::find(names.begin(), names.end(), name);
    if (add == (it != names.end())) { return; }
    if (add) { names.push_back(name); } else { names.erase(it); }
    SaveRoster(names);
}

static void SpawnBot(const std::string& login)
{
    static std::mutex spawn_mutex;
    std::lock_guard<std::mutex> spawning(spawn_mutex);
    {
        std::lock_guard<std::mutex> lock(bots_mutex);
        const bool local_name = LocalBot(login) != nullptr;
        const bool child_name = std::any_of(children.begin(), children.end(), [&](const auto& child) { return child->name == login; });
        if (local_name || child_name) { throw std::runtime_error(login + " is already connected"); }
    }
    {
        // A child process holds a single bot
        std::lock_guard<std::mutex> lock(bots_mutex);
        if (!bots.empty())
        {
            if (is_child)
            {
                throw std::runtime_error("a child bot process holds a single bot");
            }
        }
    }
    bool local;
    {
        std::lock_guard<std::mutex> lock(bots_mutex);
        local = bots.empty() || shared_world;
    }
    if (!local)
    {
        SpawnChild(login);
        return;
    }
    std::shared_ptr<McpBot> bot = std::make_shared<McpBot>();
    if (shared_world)
    {
        bot->SetSharedWorld(shared_world);
    }
    bot->SetAutoRespawn(true);
    bot->autopilot = start_autopilot;
    bot->Connect(server_address, login);
    if (!Utilities::WaitForCondition([&]()
        {
            return bot->GetShouldBeClosed() || bot->GetNetworkManager()->GetConnectionState() == ProtocolCraft::ConnectionState::Play;
        }, 15000) || bot->GetShouldBeClosed())
    {
        bot->Disconnect();
        throw std::runtime_error("could not join the server as " + login + " (name already in use, server unreachable, or connecting too soon after another bot: servers throttle joins, 4 seconds by default)");
    }
    // In the world for real: player, world and inventory received. Tools are answered only from then on
    Utilities::WaitForCondition([&]()
        {
            return bot->GetLocalPlayer() && bot->GetWorld() && bot->GetEntityManager() && bot->GetInventoryManager() && bot->GetInventoryManager()->GetPlayerInventory() != nullptr;
        }, 10000);
    bot->ResumeDefault();
    OpenMemory(*bot, login.empty() ? bot->GetNetworkManager()->GetMyName() : login);
    ServeSocket("../run/" + bot->GetNetworkManager()->GetMyName() + ".sock", bot->GetNetworkManager()->GetMyName());
    // Back to what it was doing when it left (its memory): autopilot and gear-up, what it was gathering or
    // making, its team order (a job of its own, a hunt). A castle build is handed over again by the castle
    {
        const std::map<std::string, std::string> doing = LoadDoing(*bot);
        if (doing.count("state"))
        {
            bot->was_doing = doing.at("state");
        }
        const std::string name = bot->GetNetworkManager()->GetMyName();
        std::thread([doing, name]()
            {
                // Once it is listed among the bots
                std::this_thread::sleep_for(std::chrono::seconds(3));
                const auto call = [&](const std::string& tool, Value args)
                    {
                        args["bot"] = name;
                        try
                        {
                            CallTool(tool, args);
                        }
                        catch (const std::exception& e)
                        {
                            LOG_WARNING(name << ": could not take up " << tool << " again: " << e.what());
                        }
                    };
                const auto pair = [](const std::string& text) { const size_t space = text.rfind(' '); return std::make_pair(text.substr(0, space), std::atoi(text.c_str() + space + 1)); };
                if (doing.count("autopilot") && doing.at("autopilot").size() == 2)
                {
                    Value args;
                    args["enabled"] = doing.at("autopilot")[0] == '1';
                    args["gear"] = doing.at("autopilot")[1] == '1';
                    call("autopilot", args);
                }
                if (doing.count("goal"))
                {
                    Value args;
                    args["enabled"] = true;
                    args["gather"] = pair(doing.at("goal")).first;
                    args["count"] = pair(doing.at("goal")).second;
                    call("autopilot", args);
                }
                if (doing.count("make"))
                {
                    Value args;
                    args["item"] = pair(doing.at("make")).first;
                    args["count"] = pair(doing.at("make")).second;
                    call("make", args);
                }
                if (doing.count("team"))
                {
                    try
                    {
                        call("team_member", Json::Parse(doing.at("team")));
                    }
                    catch (const std::exception& e)
                    {
                        LOG_WARNING(name << ": bad team order in memory: " << e.what());
                    }
                }
                if (!doing.empty())
                {
                    LOG_INFO(name << " takes up what it was doing" << (doing.count("state") ? ": " + doing.at("state") : ""));
                }
            }).detach();
    }
    std::lock_guard<std::mutex> lock(bots_mutex);
    bots.push_back({ bot, std::thread([bot]()
        {
            bot->StartBehaviour();
            std::chrono::steady_clock::time_point next_watch = std::chrono::steady_clock::now();
            int watch_count = 0;
            while (!bot->GetShouldBeClosed())
            {
                const std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now() + std::chrono::milliseconds(20);
                bot->BehaviourStep();
                if (std::chrono::steady_clock::now() >= next_watch)
                try
                {
                    next_watch = std::chrono::steady_clock::now() + std::chrono::milliseconds(500);
                    Watchdog(*bot);
                    // Once, a few seconds in: teleport to where it stands. Harmless, and the answer tells the
                    // bot whether it is an operator (see can_teleport)
                    if (watch_count == 10)
                    {
                        bot->SendChatCommand("minecraft:tp @s ~ ~ ~");
                    }
                    // Every 10 seconds, write what is around in the bot's memory
                    if (++watch_count % 20 == 0 && bot->GetNetworkManager()->GetConnectionState() == ProtocolCraft::ConnectionState::Play)
                    {
                        Remember(*bot);
                        // What it is doing, for the next time it joins
                        const auto [goal, goal_left] = bot->GetGoal();
                        const auto [make, make_count] = bot->GetMake();
                        {
                            std::lock_guard<std::mutex> lock(memory_mutex);
                            if (MemoryOf(*bot) != nullptr)
                            {
                                sqlite3_exec(MemoryOf(*bot), "BEGIN", nullptr, nullptr, nullptr);
                            }
                        }
                        // (Not a passing tool action, like storing before leaving: what it was doing on its own)
                        if (bot->GetState().rfind("tool:", 0) != 0)
                        {
                            SaveDoing(*bot, "state", bot->GetState() + ": " + bot->GetNote());
                        }
                        SaveDoing(*bot, "autopilot", std::string(bot->autopilot ? "1" : "0") + (bot->gear ? "1" : "0"));
                        SaveDoing(*bot, "goal", goal_left > 0 ? goal + " " + std::to_string(goal_left) : "");
                        SaveDoing(*bot, "make", make_count > 0 ? make + " " + std::to_string(make_count) : "");
                        {
                            std::lock_guard<std::mutex> lock(memory_mutex);
                            if (MemoryOf(*bot) != nullptr)
                            {
                                sqlite3_exec(MemoryOf(*bot), "COMMIT", nullptr, nullptr, nullptr);
                            }
                        }
                    }
                    // Team talk: one chat message per second at most, and the leader asks for news every minute
                    std::string line;
                    if (watch_count % 2 == 0 && bot->PopOutgoing(line))
                    {
                        bot->SendChatMessage(line);
                    }
                    if (watch_count % 120 == 0)
                    {
                        bool leads = false;
                        {
                            std::lock_guard<std::mutex> lock(bot->team_mutex);
                            leads = bot->team.leader == bot->GetNetworkManager()->GetMyName() && bot->team.members.size() > 1;
                        }
                        if (leads)
                        {
                            bot->Say("all report");
                        }
                    }
                }
                catch (const std::exception& e)
                {
                    // The periodic chores (watchdog, memory scan, team talk) must not take the bot down
                    LOG_ERROR("Bot chores failed: " << e.what());
                }
                Utilities::SleepUntil(end);
            }
        }) });
    chat_source = bot.get();
}

/// @brief Make a child store its inventory and leave, then end its process
static Value DisconnectChild(const std::shared_ptr<Child>& child)
{
    {
        std::lock_guard<std::mutex> lock(bots_mutex);
        const auto it = std::find(children.begin(), children.end(), child);
        if (it == children.end())
        {
            throw std::runtime_error("this bot is already disconnected");
        }
        children.erase(it);
    }
    Value result;
    result["disconnected"] = child->name;
    // The pipe may be taken by a long call (a journey, a build step) for a while: up to 30 seconds for it
    std::unique_lock<std::mutex> lock(child->mutex, std::defer_lock);
    for (int i = 0; i < 300 && !lock.try_lock(); ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (lock.owns_lock())
    {
        // A child that never answers (two hung there for half an hour, holding up every other goodbye): killed after
        // 3 minutes, which ends the call below
        std::atomic<bool> answered = false;
        const pid_t pid = child->pid;
        std::thread watchdog([&answered, pid]()
            {
                for (int i = 0; i < 1800 && !answered; ++i)
                {
                    std::this_thread::sleep_for(std::chrono::milliseconds(100));
                }
                if (!answered)
                {
                    kill(pid, SIGKILL);
                }
            });
        try
        {
            result = ForwardLocked(*child, "disconnect", Value(Json::Object()));
        }
        catch (const std::exception& e)
        {
            result["note"] = std::string(e.what());
        }
        answered = true;
        watchdog.join();
        // End of input: the child process exits
        fclose(child->to);
        fclose(child->from);
        child->to = child->from = nullptr;
    }
    else
    {
        // Still busy: it is told to stop (SIGHUP: it stores and leaves once that call is over), the pipe left to the call
        result["note"] = "busy: asked to stop";
        kill(child->pid, SIGHUP);
    }
    // And it has 3 minutes to go (storing takes up to 2); after that it is killed (it keeps its items on the server)
    for (int i = 0; i < 1800 && waitpid(child->pid, nullptr, WNOHANG) == 0; ++i)
    {
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    if (waitpid(child->pid, nullptr, WNOHANG) == 0)
    {
        kill(child->pid, SIGKILL);
        waitpid(child->pid, nullptr, 0);
        result["note"] = "killed after 3 minutes";
    }
    return result;
}

/// @brief Store the bot's whole inventory in chests, then disconnect it
static Value DisconnectBot(const std::shared_ptr<McpBot>& bot)
{
    const auto stored = std::make_shared<std::atomic<int>>(0);
    const auto error = std::make_shared<std::string>();
    if (!bot->GetShouldBeClosed())
    {
        Run(*bot, 120, "store before disconnecting", [=](McpBot& c) { return StoreTask(c, true, *stored, *error); });
    }
    BotSlot slot;
    {
        std::lock_guard<std::mutex> lock(bots_mutex);
        const auto it = std::find_if(bots.begin(), bots.end(), [&](const BotSlot& s) { return s.bot == bot; });
        if (it == bots.end())
        {
            throw std::runtime_error("this bot is already disconnected");
        }
        slot = std::move(*it);
        bots.erase(it);
        NewChatSource();
    }
    // Disconnect() frees the network manager, and the name with it
    const std::string name = slot.bot->GetNetworkManager()->GetMyName();
    LeaveGame(slot);
    Value o;
    o["disconnected"] = name;
    o["stored"] = stored->load();
    o["note"] = error->empty() ? "inventory stored" : *error;
    return o;
}

static Value CallLuaTool(const std::string& name, const Value& a, bool& found);
static Value CallTool(const std::string& name, const Value& a);

// Bots with a job of their own (team_member with solo=true): the team tool leaves them out
static std::set<std::string> solo_bots;
// The leader the bots elected (the brain plugin's vote): the team tool's leader when none is given
static std::string elected_leader;

/// @brief The jobs the main program keeps track of ("solo", "feeder"), in the shared database: they outlive a restart
static void SaveRole(const std::string& bot, const std::string& role, const bool on)
{
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(SharedDb(), on ? "INSERT OR IGNORE INTO roles (bot, role) VALUES (?1, ?2)" : "DELETE FROM roles WHERE bot = ?1 AND role = ?2", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, bot.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_bind_text(statement, 2, role.c_str(), -1, SQLITE_TRANSIENT);
    sqlite3_step(statement);
    sqlite3_finalize(statement);
}

static std::vector<std::string> LoadRole(const std::string& role)
{
    std::vector<std::string> names;
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(SharedDb(), "SELECT bot FROM roles WHERE role = ?1 ORDER BY rowid", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, role.c_str(), -1, SQLITE_TRANSIENT);
    while (sqlite3_step(statement) == SQLITE_ROW)
    {
        names.push_back(reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)));
    }
    sqlite3_finalize(statement);
    return names;
}

/// @brief The bot's trade (the groups plugin's: bakers, miners...), "" if none: a "trade:..." role in the shared
/// database, so each bot's own process reads it for its status (the panels show it)
static std::string TradeOf(const std::string& bot)
{
    std::string trade;
    sqlite3_stmt* statement = nullptr;
    sqlite3_prepare_v2(SharedDb(), "SELECT role FROM roles WHERE bot = ?1 AND role LIKE 'trade:%'", -1, &statement, nullptr);
    sqlite3_bind_text(statement, 1, bot.c_str(), -1, SQLITE_TRANSIENT);
    if (sqlite3_step(statement) == SQLITE_ROW)
    {
        trade = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0)) + 6;
    }
    sqlite3_finalize(statement);
    return trade;
}

static Value CallTool(const std::string& name, const Value& a)
{
    // What the bots know is in the chests (chest_contents): all of them, or those holding an item, fullest first
    if (name == "chests")
    {
        sqlite3* db = SharedDb();
        const std::string item = a.is_object() && a.contains("item") ? Id(Str(a, "item")) : "";
        const int max = static_cast<int>(Num(a, "max", 20));
        Value list = Json::Array();
        sqlite3_stmt* statement = nullptr;
        if (db == nullptr || sqlite3_prepare_v2(db, item.empty()
            ? "SELECT x, y, z, dimension, MAX(seen), MAX(by) FROM chest_contents GROUP BY dimension, x, y, z ORDER BY MAX(seen) DESC LIMIT ?2"
            : "SELECT x, y, z, dimension, seen, by FROM chest_contents WHERE item = ?1 AND count > 0 ORDER BY count DESC LIMIT ?2", -1, &statement, nullptr) != SQLITE_OK)
        {
            return list;
        }
        sqlite3_bind_text(statement, 1, item.c_str(), -1, SQLITE_TRANSIENT);
        sqlite3_bind_int(statement, 2, max);
        while (sqlite3_step(statement) == SQLITE_ROW)
        {
            Value o;
            o["x"] = sqlite3_column_int(statement, 0);
            o["y"] = sqlite3_column_int(statement, 1);
            o["z"] = sqlite3_column_int(statement, 2);
            o["dimension"] = reinterpret_cast<const char*>(sqlite3_column_text(statement, 3));
            o["seen_minutes_ago"] = static_cast<int>((std::time(nullptr) - sqlite3_column_int64(statement, 4)) / 60);
            o["by"] = reinterpret_cast<const char*>(sqlite3_column_text(statement, 5));
            list.push_back(o);
        }
        sqlite3_finalize(statement);
        for (size_t i = 0; i < list.size(); ++i)
        {
            Value contents = Json::Object();
            sqlite3_prepare_v2(db, "SELECT item, count FROM chest_contents WHERE dimension = ?1 AND x = ?2 AND y = ?3 AND z = ?4 AND item <> ''", -1, &statement, nullptr);
            sqlite3_bind_text(statement, 1, list[i]["dimension"].get_string().c_str(), -1, SQLITE_TRANSIENT);
            sqlite3_bind_int(statement, 2, list[i]["x"].get_number<int>());
            sqlite3_bind_int(statement, 3, list[i]["y"].get_number<int>());
            sqlite3_bind_int(statement, 4, list[i]["z"].get_number<int>());
            while (sqlite3_step(statement) == SQLITE_ROW)
            {
                const std::string name = reinterpret_cast<const char*>(sqlite3_column_text(statement, 0));
                contents[name.substr(name.find(':') + 1)] = sqlite3_column_int(statement, 1);
            }
            sqlite3_finalize(statement);
            list[i]["contents"] = contents;
        }
        return list;
    }
    // The groups plugin telling each bot's trade (trade "" for none)
    if (name == "trade" && a.is_object() && a.contains("bot") && a["bot"].is_string())
    {
        const std::string bot = a["bot"].get_string();
        const std::string old = TradeOf(bot);
        if (!old.empty())
        {
            SaveRole(bot, "trade:" + old, false);
        }
        const std::string trade = Str(a, "trade");
        if (!trade.empty())
        {
            SaveRole(bot, "trade:" + trade, true);
        }
        return trade;
    }
    if (name == "team_member" && a.is_object() && a.contains("solo") && a["solo"].is_bool() && a.contains("bot") && a["bot"].is_string())
    {
        std::lock_guard<std::mutex> lock(bots_mutex);
        if (a["solo"].get<bool>())
        {
            solo_bots.insert(a["bot"].get_string());
        }
        else
        {
            solo_bots.erase(a["bot"].get_string());
        }
        SaveRole(a["bot"].get_string(), "solo", a["solo"].get<bool>());
    }
    if (name == "bots")
    {
        Value list = Json::Array();
        std::vector<std::shared_ptr<Child>> remote;
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            for (const BotSlot& slot : bots)
            {
                Value o;
                o["name"] = slot.bot->GetNetworkManager()->GetMyName();
                o["state"] = slot.bot->GetShouldBeClosed() ? "dropped by the server" : slot.bot->GetState();
                list.push_back(o);
            }
            remote = children;
        }
        for (const std::shared_ptr<Child>& child : remote)
        {
            Value o;
            o["name"] = child->name;
            o["state"] = ChildStatus(*child)["state"];
            list.push_back(o);
        }
        std::lock_guard<std::mutex> lock(bots_mutex);
        for (const auto& [waiting, rejoin] : rejoining)
        {
            Value o;
            o["name"] = waiting;
            o["state"] = "kicked or crashed, rejoining soon";
            list.push_back(o);
        }
        return list;
    }
    // A bot waiting to rejoin: disconnect means don't
    if (name == "disconnect" && a.is_object() && a.contains("bot") && a["bot"].is_string())
    {
        if (!a.contains("everyone")) { RosterChange(a["bot"].get_string(), false); }
        std::lock_guard<std::mutex> lock(bots_mutex);
        if (rejoining.erase(a["bot"].get_string()) != 0)
        {
            return a["bot"].get_string() + " will not rejoin";
        }
    }
    if (name == "reconnect_all")
    {
        // In the background (storing everything takes minutes): every bot leaves, then the same names join
        // again 10 seconds apart (the server throttles joins). Each starts from the binary on disk: a new build
        // without stopping the program
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            for (const BotSlot& slot : bots)
            {
                names.push_back(slot.bot->GetNetworkManager()->GetMyName());
            }
            for (const std::shared_ptr<Child>& child : children)
            {
                names.push_back(child->name);
            }
        }
        // Each one back as soon as it has left (one that hangs holds up only itself), 10 seconds after the last join
        std::thread([names]()
            {
                std::mutex turn;
                auto next = std::chrono::steady_clock::now();
                std::vector<std::thread> each;
                for (const std::string& who : names)
                {
                    each.emplace_back([&, who]()
                        {
                            Value args;
                            args["bot"] = who;
                            args["everyone"] = true;
                            try
                            {
                                CallTool("disconnect", args);
                            }
                            catch (const std::exception& e)
                            {
                                LOG_WARNING("reconnect_all: " << who << ": " << e.what());
                            }
                            std::chrono::steady_clock::time_point at;
                            {
                                std::lock_guard<std::mutex> lock(turn);
                                at = std::max(next, std::chrono::steady_clock::now());
                                next = at + std::chrono::seconds(10);
                            }
                            std::this_thread::sleep_until(at);
                            if (stopping)
                            {
                                return;
                            }
                            Value again;
                            again["name"] = who;
                            try
                            {
                                CallTool("connect", again);
                            }
                            catch (const std::exception& e)
                            {
                                LOG_WARNING("reconnect_all: " << who << ": " << e.what());
                            }
                        });
                }
                for (std::thread& t : each)
                {
                    t.join();
                }
            }).detach();
        return "every bot stores everything, leaves and joins again, 10 seconds apart";
    }
    if (name == "disconnect_all")
    {
        // Every bot at once: each stores everything in the chests, then leaves. The program keeps running
        // (connect brings them back); none rejoins by itself
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            for (const BotSlot& slot : bots)
            {
                names.push_back(slot.bot->GetNetworkManager()->GetMyName());
            }
            for (const std::shared_ptr<Child>& child : children)
            {
                names.push_back(child->name);
            }
            rejoining.clear();
        }
        std::vector<std::thread> goodbyes;
        std::vector<Value> results(names.size());
        for (size_t i = 0; i < names.size(); ++i)
        {
            goodbyes.emplace_back([&, i]()
                {
                    Value args;
                    args["bot"] = names[i];
                    args["everyone"] = true;
                    try
                    {
                        results[i] = CallTool("disconnect", args);
                    }
                    catch (const std::exception& e)
                    {
                        results[i] = Value(names[i] + ": " + e.what());
                    }
                });
        }
        for (std::thread& t : goodbyes)
        {
            t.join();
        }
        Value list = Json::Array();
        for (const Value& r : results)
        {
            list.push_back(r);
        }
        return list;
    }
    if (name == "team")
    {
        const std::string objective = Str(a, "objective");
        if (objective != "gather" && objective != "attack" && objective != "build" && objective != "follow" && objective != "disband")
        {
            throw std::runtime_error("objective must be gather, attack, build, follow or disband");
        }
        std::vector<std::string> names;
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            for (const BotSlot& slot : bots)
            {
                names.push_back(slot.bot->GetNetworkManager()->GetMyName());
            }
            for (const std::shared_ptr<Child>& child : children)
            {
                names.push_back(child->name);
            }
            names.erase(std::remove_if(names.begin(), names.end(), [](const std::string& n) { return solo_bots.count(n) != 0; }), names.end());
        }
        if (names.empty())
        {
            throw std::runtime_error("no bot is connected, use the connect tool");
        }
        std::string leader = a.contains("leader") && a["leader"].is_string() && !a["leader"].get_string().empty() ? a["leader"].get_string() : names.front();
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            if (!(a.contains("leader") && a["leader"].is_string() && !a["leader"].get_string().empty()) &&
                std::find(names.begin(), names.end(), elected_leader) != names.end())
            {
                leader = elected_leader;
            }
        }
        if (std::find(names.begin(), names.end(), leader) == names.end())
        {
            throw std::runtime_error("no bot named " + leader + ", see the bots tool");
        }
        // The total is split between the bots
        const int share = static_cast<int>(std::ceil(Num(a, "count", 16) / static_cast<double>(names.size())));
        Value members = Json::Array();
        for (const std::string& member : names)
        {
            members.push_back(member);
        }
        for (const std::string& member : names)
        {
            Value order = a;
            order["members"] = members;
            order["bot"] = member;
            order["leader"] = objective == "disband" ? "" : leader;
            order["count"] = share;
            CallTool("team_member", order);
        }
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            team_leader = objective == "disband" ? "" : leader;
        }
        Value o;
        o["objective"] = objective;
        o["leader"] = objective == "disband" ? "" : leader;
        o["members"] = members;
        return o;
    }
    if (name == "schematic")
    {
        const auto run = [](const std::string& command)
            {
                std::string out;
                FILE* pipe = popen(command.c_str(), "r");
                if (pipe == nullptr)
                {
                    throw std::runtime_error("can't run " + command.substr(0, command.find(' ')));
                }
                char buffer[65536];
                size_t n;
                // ponytail: 64 MB cap on what is read, more than any schematic unpacks to
                while ((n = fread(buffer, 1, sizeof(buffer), pipe)) > 0 && out.size() < (64u << 20))
                {
                    out.append(buffer, n);
                }
                pclose(pipe);
                return out;
            };
        const auto vanilla = [](const std::string& id) { return id.rfind("minecraft:", 0) == 0; };
        Value o;
        if (a.is_object() && a.contains("url"))
        {
            // The page of a schematic, as markdown: the site gives agents the material list this way. The
            // schematic file itself is for players only (createmod.com/auth.md): never fetched from here.
            // The link ends up in a shell command: nothing but a plain schematic page gets through
            const std::string url = Str(a, "url");
            const std::string prefix = "https://createmod.com/schematics/";
            if (url.rfind(prefix, 0) != 0 || url.size() == prefix.size() || url.size() > 200 ||
                url.find_first_not_of("abcdefghijklmnopqrstuvwxyz0123456789-", prefix.size()) != std::string::npos)
            {
                throw std::runtime_error("url must be a schematic page: https://createmod.com/schematics/<name>");
            }
            const std::string page = run("curl -sL -m 30 -A 'Mozilla/5.0 (botcraft bot)' -H 'Accept: text/markdown' '" + url + "'");
            if (page.rfind("# ", 0) != 0)
            {
                throw std::runtime_error("no such schematic (or the site did not answer)");
            }
            Value materials = Json::Array();
            Value missing = Json::Array();
            std::istringstream lines(page);
            std::string line;
            bool in_materials = false;
            while (std::getline(lines, line))
            {
                if (line.rfind("# ", 0) == 0)
                {
                    o["title"] = line.substr(2);
                }
                else if (line.rfind("## ", 0) == 0)
                {
                    in_materials = line == "## Material list";
                }
                else if (line.rfind("- ", 0) == 0)
                {
                    const size_t colon = in_materials ? line.rfind(": ") : line.find(": ");
                    if (colon == std::string::npos)
                    {
                        continue;
                    }
                    const std::string key = line.substr(2, colon - 2), value = line.substr(colon + 2);
                    if (!in_materials)
                    {
                        // Author, Dimensions, Block count, Required mods, Minecraft version...
                        std::string field = key;
                        std::transform(field.begin(), field.end(), field.begin(), [](const unsigned char ch) { return ch == ' ' ? '_' : std::tolower(ch); });
                        o[field] = value;
                    }
                    else
                    {
                        Value m;
                        m["item"] = key;
                        m["count"] = std::atoi(value.c_str());
                        materials.push_back(m);
                        if (!vanilla(key))
                        {
                            missing.push_back(key);
                        }
                    }
                }
            }
            o["materials"] = materials;
            // Blocks of other mods: this server does not have them, the bots leave those spots empty
            o["not_in_this_game"] = missing;
            o["instructions"] = "The block-by-block plan is in the schematic file, which the site gives to players only: download it from the page (" + url +
                "), put it in bot/schematics/, then call schematic with file=<name>.nbt (materials and size), and with x y z as well to have the team build it there.";
            return o;
        }
        // A structure file the user downloaded into bot/schematics/: gzipped NBT with size, palette and blocks
        const std::string file = Str(a, "file");
        if (file.empty() || file[0] == '.' || file.size() > 120 ||
            file.find_first_not_of("abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_.-") != std::string::npos)
        {
            throw std::runtime_error("file must be the plain name of a .nbt file in bot/schematics/");
        }
        const std::string raw = run("gzip -dc '../schematics/" + file + "' 2>/dev/null");
        if (raw.empty())
        {
            throw std::runtime_error("can't read bot/schematics/" + file);
        }
        const std::vector<unsigned char> bytes(raw.begin(), raw.end());
        ProtocolCraft::ReadIterator iter = bytes.begin();
        size_t length = bytes.size();
        ProtocolCraft::NBT::Value nbt;
        nbt.Read(iter, length);
        std::vector<std::string> palette;
        for (const auto& state : nbt["palette"].as_list_of<ProtocolCraft::NBT::TagCompound>())
        {
            std::string block = state["Name"].get<std::string>();
            // ponytail: only the way a block faces is reproduced (stairs, doors, chests); slab halves, axes and
            // the rest come out as the game places them. Add them to PlaceBlock's handling if it shows
            if (state.contains("Properties") && state["Properties"].contains("facing"))
            {
                block += "[facing=" + state["Properties"]["facing"].get<std::string>() + "]";
            }
            palette.push_back(block);
        }
        std::vector<std::pair<Position, std::string>> plan;
        std::map<std::string, int> counts;
        int skipped = 0;
        for (const auto& block : nbt["blocks"].as_list_of<ProtocolCraft::NBT::TagCompound>())
        {
            const std::vector<int>& pos = block["pos"].as_list_of<int>();
            const std::string& what = palette.at(block["state"].get<int>());
            const std::string id = what.substr(0, what.find('['));
            if (id == "minecraft:air" || id == "minecraft:structure_void")
            {
                continue;
            }
            if (!vanilla(id))
            {
                ++skipped;
                continue;
            }
            ++counts[id];
            plan.push_back({ Position(pos.at(0), pos.at(1), pos.at(2)), what });
        }
        // Bottom up, so that the parts are layers
        std::stable_sort(plan.begin(), plan.end(), [](const auto& l, const auto& r) { return l.first.y < r.first.y; });
        constexpr size_t part_size = 4000;
        const int parts = static_cast<int>((plan.size() + part_size - 1) / part_size);
        Value materials = Json::Array();
        for (const auto& count : counts)
        {
            Value m;
            m["item"] = count.first;
            m["count"] = count.second;
            materials.push_back(m);
        }
        o["file"] = file;
        o["blocks"] = static_cast<int>(plan.size());
        o["skipped_blocks_of_other_mods"] = skipped;
        o["materials"] = materials;
        o["parts"] = parts;
        if (a.contains("x") && a.contains("y") && a.contains("z") && parts > 0)
        {
            // ponytail: one part (4000 blocks, a team build's limit) per call; the caller asks for the next
            // one when status says build_done. Chain them like the castle's stages if that gets tedious
            const int part = std::clamp(static_cast<int>(Num(a, "part", 1)), 1, parts);
            const Position origin(static_cast<int>(Num(a, "x")), static_cast<int>(Num(a, "y")), static_cast<int>(Num(a, "z")));
            Value blocks = Json::Array();
            for (size_t i = (part - 1) * part_size; i < std::min(plan.size(), part * part_size); ++i)
            {
                Value entry = Json::Array();
                entry.push_back(origin.x + plan[i].first.x);
                entry.push_back(origin.y + plan[i].first.y);
                entry.push_back(origin.z + plan[i].first.z);
                entry.push_back(plan[i].second);
                blocks.push_back(entry);
            }
            Value order;
            order["objective"] = "build";
            order["plan"] = "blocks";
            order["blocks"] = blocks;
            order["name"] = file;
            CallTool("team", order);
            o["building_part"] = part;
        }
        return o;
    }
    if (name == "leader")
    {
        // The elected leader: with name, sets it (and makes it the leader of the team at the next order)
        if (a.is_object() && a.contains("name") && a["name"].is_string())
        {
            const std::string elected = a["name"].get_string();
            for (const std::string& old : LoadRole("leader"))
            {
                SaveRole(old, "leader", false);
            }
            SaveRole(elected, "leader", true);
            std::lock_guard<std::mutex> lock(bots_mutex);
            elected_leader = elected;
        }
        std::lock_guard<std::mutex> lock(bots_mutex);
        return elected_leader;
    }
    if (name == "feeders")
    {
        static std::mutex feeders_mutex;
        static std::vector<std::string> feeders;
        static std::map<std::string, std::string> feeder_notes;
        static std::set<std::string> feeder_busy;
        static std::map<std::string, int> feeder_hunts;
        static std::map<std::string, std::chrono::steady_clock::time_point> feeder_wood_at;
        static bool loop_started = false;
        const auto statuses = []()
            {
                std::vector<std::string> names;
                {
                    std::lock_guard<std::mutex> lock(bots_mutex);
                    for (const BotSlot& slot : bots)
                    {
                        names.push_back(slot.bot->GetNetworkManager()->GetMyName());
                    }
                    for (const std::shared_ptr<Child>& child : children)
                    {
                        names.push_back(child->name);
                    }
                }
                std::vector<Value> list;
                for (const std::string& n : names)
                {
                    try
                    {
                        Value args;
                        args["bot"] = n;
                        Value st = CallTool("status", args);
                        if (st.is_object() && st.contains("name"))
                        {
                            list.push_back(st);
                        }
                    }
                    catch (const std::exception&)
                    {
                    }
                }
                return list;
            };
        const auto count_of = [](const Value& st, const std::function<bool(const std::string&)>& what)
            {
                int n = 0;
                if (st.contains("inventory") && st["inventory"].is_object())
                {
                    for (const auto& [item, count] : st["inventory"].get_object())
                    {
                        n += what("minecraft:" + item) && count.is_number() ? count.get_number<int>() : 0;
                    }
                }
                return n;
            };
        const auto is_food = [](const std::string& item) { return foods.count(item) != 0; };
        const auto is_wood = [](const std::string& item) { return EndsWith(item, "_log") || EndsWith(item, "_planks"); };
        if (a.is_object() && a.contains("stop") && a["stop"].is_bool() && a["stop"].get<bool>())
        {
            std::vector<std::string> gone;
            {
                std::lock_guard<std::mutex> lock(feeders_mutex);
                gone.swap(feeders);
            }
            for (const std::string& f : gone)
            {
                SaveRole(f, "feeder", false);
            }
            for (const std::string& f : gone)
            {
                Value order;
                order["bot"] = f;
                order["objective"] = "disband";
                order["members"] = Json::Array();
                order["leader"] = "";
                order["count"] = 1;
                order["solo"] = false;
                CallTool("team_member", order);
            }
            return "the feeders are back with the others";
        }
        if (a.is_object() && (a.contains("bots") || a.contains("count")))
        {
            std::vector<std::string> chosen;
            if (a.contains("bots") && a["bots"].is_array())
            {
                for (const Value& n : a["bots"].get_array())
                {
                    chosen.push_back(n.get_string());
                }
            }
            else
            {
                // The best fed: they can go far without eating
                std::vector<Value> all = statuses();
                std::sort(all.begin(), all.end(), [](const Value& l, const Value& r) { return l["food"].get_number<double>() * 2 + l["health"].get_number<double>() > r["food"].get_number<double>() * 2 + r["health"].get_number<double>(); });
                for (size_t i = 0; i < all.size() && chosen.size() < static_cast<size_t>(std::clamp(static_cast<int>(Num(a, "count", 3)), 1, 10)); ++i)
                {
                    // Not a bot that has a job of its own already (roads, berries)
                    std::lock_guard<std::mutex> lock(bots_mutex);
                    if (solo_bots.count(all[i]["name"].get_string()) == 0)
                    {
                        chosen.push_back(all[i]["name"].get_string());
                    }
                }
            }
            for (const std::string& f : LoadRole("feeder"))
            {
                SaveRole(f, "feeder", false);
            }
            for (const std::string& f : chosen)
            {
                SaveRole(f, "feeder", true);
            }
            std::lock_guard<std::mutex> lock(feeders_mutex);
            feeders = chosen;
            if (!loop_started)
            {
                loop_started = true;
                // ponytail: one detached loop for the life of the program, idle while there are no feeders
                std::thread([statuses, count_of, is_food, is_wood]()
                    {
                        Logger::GetInstance().RegisterThread("feeders");
                        static const char* game[] = { "cow", "pig", "sheep" };
                        while (true)
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(15));
                            std::vector<std::string> mine;
                            {
                                std::lock_guard<std::mutex> lock(feeders_mutex);
                                mine = feeders;
                            }
                            if (mine.empty())
                            {
                                continue;
                            }
                            const std::vector<Value> all = statuses();
                            std::set<std::string> targeted;
                            for (const std::string& f : mine)
                            {
                                const auto me = std::find_if(all.begin(), all.end(), [&](const Value& st) { return st["name"].get_string() == f; });
                                {
                                    std::lock_guard<std::mutex> lock(feeders_mutex);
                                    if (me == all.end() || feeder_busy.count(f) != 0)
                                    {
                                        continue;
                                    }
                                }
                                const int food = count_of(*me, is_food);
                                const int wood = count_of(*me, is_wood);
                                // The hungriest bot that has nothing to eat, not a feeder, nobody on the way to it yet
                                const Value* hungry = nullptr;
                                const Value* woodless = nullptr;
                                for (const Value& st : all)
                                {
                                    const std::string n = st["name"].get_string();
                                    if (std::find(mine.begin(), mine.end(), n) != mine.end() || targeted.count(n) != 0)
                                    {
                                        continue;
                                    }
                                    if (st["food"].get_number<double>() <= 8.0 && count_of(st, is_food) == 0 &&
                                        (hungry == nullptr || st["food"].get_number<double>() < (*hungry)["food"].get_number<double>()))
                                    {
                                        hungry = &st;
                                    }
                                    if (count_of(st, is_wood) == 0 && woodless == nullptr)
                                    {
                                        woodless = &st;
                                    }
                                }
                                const Value* to = nullptr;
                                std::string item;
                                int count = 0;
                                if (hungry != nullptr && food >= 2)
                                {
                                    to = hungry;
                                    item = "food";
                                    count = std::clamp(food / 2, 2, 16);
                                }
                                else if (woodless != nullptr && wood >= 16)
                                {
                                    to = woodless;
                                    item = "wood";
                                    count = 8;
                                }
                                if (to != nullptr)
                                {
                                    const std::string name = (*to)["name"].get_string();
                                    targeted.insert(name);
                                    Value order;
                                    order["bot"] = f;
                                    order["to"] = name;
                                    order["item"] = item;
                                    order["count"] = count;
                                    if ((*to).contains("position"))
                                    {
                                        order["x"] = (*to)["position"]["x"];
                                        order["y"] = (*to)["position"]["y"];
                                        order["z"] = (*to)["position"]["z"];
                                    }
                                    {
                                        std::lock_guard<std::mutex> lock(feeders_mutex);
                                        feeder_busy.insert(f);
                                        feeder_notes[f] = "taking " + std::to_string(count) + " " + item + " to " + name;
                                    }
                                    std::thread([f, order, name, item]()
                                        {
                                            std::string note;
                                            try
                                            {
                                                const Value r = CallTool("give", order);
                                                note = "gave " + r["given"].Dump() + " " + item + " to " + name;
                                            }
                                            catch (const std::exception& e)
                                            {
                                                note = "could not give " + item + " to " + name + ": " + e.what();
                                            }
                                            LOG_INFO("Feeder " << f << ": " << note);
                                            std::lock_guard<std::mutex> lock(feeders_mutex);
                                            feeder_notes[f] = note;
                                            feeder_busy.erase(f);
                                        }).detach();
                                    continue;
                                }
                                // Raw meat feeds less than half what it does cooked (beef 3, steak 8 hunger): a furnace first.
                                // The make tool smelts it (its own furnace, its own fuel)
                                {
                                    static const std::pair<const char*, const char*> cook[] = {
                                        { "beef", "cooked_beef" }, { "porkchop", "cooked_porkchop" }, { "mutton", "cooked_mutton" }, { "chicken", "cooked_chicken" } };
                                    bool cooking = false;
                                    const std::string state = (*me)["state"].get_string();
                                    for (const auto& [raw, cooked] : cook)
                                    {
                                        const int n = count_of(*me, [&](const std::string& item) { return item == std::string("minecraft:") + raw; });
                                        if (n >= 3 && state != "making" && state != "hunting" && state != "fighting")
                                        {
                                            Value order;
                                            order["bot"] = f;
                                            order["item"] = cooked;
                                            order["count"] = n + count_of(*me, [&](const std::string& item) { return item == std::string("minecraft:") + cooked; });
                                            try
                                            {
                                                CallTool("make", order);
                                                std::lock_guard<std::mutex> lock(feeders_mutex);
                                                feeder_notes[f] = std::string("cooking ") + std::to_string(n) + " " + raw;
                                                cooking = true;
                                            }
                                            catch (const std::exception& e)
                                            {
                                                LOG_WARNING("Feeder " << f << ": can't cook " << raw << ": " << e.what());
                                            }
                                            break;
                                        }
                                    }
                                    // Carrots and gold: golden carrots, the best food there is (6 hunger, 14.4 saturation).
                                    // A carrot and 8 gold nuggets, the nuggets from an ingot
                                    const int carrots = count_of(*me, [](const std::string& item) { return item == "minecraft:carrot"; });
                                    const int gold = count_of(*me, [](const std::string& item) { return item == "minecraft:gold_ingot"; }) +
                                        count_of(*me, [](const std::string& item) { return item == "minecraft:gold_nugget"; }) / 8;
                                    if (!cooking && carrots > 0 && gold > 0 && state != "making" && state != "hunting" && state != "fighting")
                                    {
                                        Value order;
                                        order["bot"] = f;
                                        order["item"] = "golden_carrot";
                                        order["count"] = std::min(carrots, gold) + count_of(*me, [](const std::string& item) { return item == "minecraft:golden_carrot"; });
                                        try
                                        {
                                            CallTool("make", order);
                                            std::lock_guard<std::mutex> lock(feeders_mutex);
                                            feeder_notes[f] = "making golden carrots";
                                            cooking = true;
                                        }
                                        catch (const std::exception& e)
                                        {
                                            LOG_WARNING("Feeder " << f << ": can't make golden carrots: " << e.what());
                                        }
                                    }
                                    if (cooking)
                                    {
                                        continue;
                                    }
                                }
                                // Nothing to hand over: stock up. A hunt of its own (a team of one, out of the castle work),
                                // the next kind of game each time the last hunt ended; and wood every five minutes
                                const std::string state = (*me)["state"].get_string();
                                try
                                {
                                    if (food < 12 && state != "hunting" && state != "fighting" && state != "giving")
                                    {
                                        int kind;
                                        {
                                            std::lock_guard<std::mutex> lock(feeders_mutex);
                                            kind = feeder_hunts[f]++;
                                            feeder_notes[f] = std::string("hunting ") + game[kind % 3];
                                        }
                                        Value order;
                                        order["bot"] = f;
                                        order["objective"] = "attack";
                                        order["type"] = game[kind % 3];
                                        order["members"] = Json::Array();
                                        order["members"].push_back(f);
                                        order["leader"] = f;
                                        order["count"] = 6;
                                        order["solo"] = true;
                                        CallTool("team_member", order);
                                    }
                                    const auto now = std::chrono::steady_clock::now();
                                    bool wood_due;
                                    {
                                        std::lock_guard<std::mutex> lock(feeders_mutex);
                                        wood_due = wood < 32 && now >= feeder_wood_at[f];
                                        if (wood_due)
                                        {
                                            feeder_wood_at[f] = now + std::chrono::minutes(5);
                                        }
                                    }
                                    if (wood_due)
                                    {
                                        Value order;
                                        order["bot"] = f;
                                        order["enabled"] = true;
                                        order["gather"] = "oak_log,spruce_log,birch_log,jungle_log,acacia_log,dark_oak_log,mangrove_log,cherry_log";
                                        order["count"] = 32;
                                        CallTool("autopilot", order);
                                    }
                                }
                                catch (const std::exception& e)
                                {
                                    LOG_WARNING("Feeder " << f << ": " << e.what());
                                }
                            }
                        }
                    }).detach();
            }
        }
        Value o = Json::Array();
        std::lock_guard<std::mutex> lock(feeders_mutex);
        for (const std::string& f : feeders)
        {
            Value one;
            one["bot"] = f;
            one["doing"] = feeder_notes.count(f) ? feeder_notes[f] : "starting";
            o.push_back(one);
        }
        return o;
    }
    if (name == "sites")
    {
        // The stored construction boxes; with x1..z2 adds one by hand, with remove=<n> forgets the n-th (from 1)
        if (a.is_object() && a.contains("x1") && a.contains("x2"))
        {
            AddSite(Position(static_cast<int>(Num(a, "x1")), static_cast<int>(Num(a, "y1")), static_cast<int>(Num(a, "z1"))),
                Position(static_cast<int>(Num(a, "x2")), static_cast<int>(Num(a, "y2")), static_cast<int>(Num(a, "z2"))), SiteName(a, "manual"));
        }
        std::lock_guard<std::mutex> lock(sites_mutex);
        std::vector<Site> sites = LoadSites();
        const int remove = a.is_object() && a.contains("remove") ? static_cast<int>(Num(a, "remove")) : 0;
        if (remove >= 1 && remove <= static_cast<int>(sites.size()))
        {
            sites.erase(sites.begin() + remove - 1);
            sqlite3_stmt* statement = nullptr;
            sqlite3_prepare_v2(SharedDb(), "DELETE FROM sites WHERE rowid = (SELECT rowid FROM sites ORDER BY rowid LIMIT 1 OFFSET ?1)", -1, &statement, nullptr);
            sqlite3_bind_int(statement, 1, remove - 1);
            sqlite3_step(statement);
            sqlite3_finalize(statement);
        }
        Value list = Json::Array();
        for (const Site& site : sites)
        {
            Value o;
            o["name"] = site.name;
            o["x1"] = site.min.x;
            o["y1"] = site.min.y;
            o["z1"] = site.min.z;
            o["x2"] = site.max.x;
            o["y2"] = site.max.y;
            o["z2"] = site.max.z;
            list.push_back(o);
        }
        return list;
    }
    if (name == "mines")
    {
        // The entrances of the team's mines; with x y z, declares one by hand
        if (a.is_object() && a.contains("x"))
        {
            AddMine(Position(static_cast<int>(Num(a, "x")), static_cast<int>(Num(a, "y")), static_cast<int>(Num(a, "z"))),
                a.contains("resources") && a["resources"].is_string() ? a["resources"].get_string() : "");
        }
        Value list = Json::Array();
        std::lock_guard<std::mutex> lock(mines_mutex);
        for (const Mine& mine : LoadMines())
        {
            Value o;
            o["x"] = mine.entrance.x;
            o["y"] = mine.entrance.y;
            o["z"] = mine.entrance.z;
            o["resources"] = mine.resources;
            list.push_back(o);
        }
        return list;
    }
    if (name == "vault")
    {
        // Declare (or list) the vault's chests: one per category of item, and per floor ("food", "food#2"...).
        // Stored with the team's chest list. full: a bot found the last floor's chest of a category full
        std::lock_guard<std::mutex> lock(chests_mutex);
        LoadChests();
        if (a.is_object() && a.contains("category"))
        {
            chests["category:" + Str(a, "category")] = Position(static_cast<int>(Num(a, "x")), static_cast<int>(Num(a, "y")), static_cast<int>(Num(a, "z")));
            SaveChests();
        }
        if (a.is_object() && a.contains("full") && a["full"].is_bool() && !a["full"].get<bool>())
        {
            chests.erase("vault:full");
            SaveChests();
        }
        Value o;
        o["chests"] = Json::Object();
        for (const auto& [key, position] : chests)
        {
            if (key.rfind("category:", 0) == 0)
            {
                o["chests"][key.substr(9)] = std::to_string(position.x) + " " + std::to_string(position.y) + " " + std::to_string(position.z);
            }
        }
        o["full"] = chests.count("vault:full") != 0;
        return o;
    }
    if (name == "spawn")
    {
        if (is_child || stopping) { throw std::runtime_error("spawn needs a running main program"); }
        if (!a.is_object() || !a.contains("count") || !a["count"].is_integer() ||
            a["count"].get_number<double>() < 1 || a["count"].get_number<double>() > 100)
        {
            throw std::runtime_error("count must be an integer from 1 to 100");
        }
        std::lock_guard<std::mutex> lock(bots_mutex);
        std::set<std::string> used;
        for (const auto& slot : bots) { used.insert(slot.bot->GetNetworkManager()->GetMyName()); }
        for (const auto& child : children) { used.insert(child->name); }
        for (const auto& [waiting, rejoin] : rejoining) { used.insert(waiting); }
        const auto names = NewBotNames(a["count"].get_number<int>(), a.contains("base_name") ? Str(a, "base_name") : "Bot", used);
        const auto now = std::chrono::steady_clock::now();
        Value result = Json::Array();
        for (std::size_t i = 0; i < names.size(); ++i)
        {
            Rejoin queued;
            queued.at = now + std::chrono::seconds(std::max(spawn_delay_seconds, 5) * static_cast<int>(i));
            queued.restore["bot"] = names[i];
            queued.restore["enabled"] = start_autopilot;
            rejoining[names[i]] = queued;
            RosterChange(names[i], true);
            result.push_back(names[i]);
        }
        return result;
    }
    if (name == "connect")
    {
        {
            // One bot per name: a second login under the same name makes the server kick the first,
            // which then rejoins and kicks the second, for ever
            std::lock_guard<std::mutex> lock(bots_mutex);
            const std::string wanted = Str(a, "name");
            const bool local = LocalBot(wanted) != nullptr;
            const bool child = std::any_of(children.begin(), children.end(), [&](const std::shared_ptr<Child>& c) { return c->name == wanted; });
            if (local || child)
            {
                throw std::runtime_error(wanted + " is already connected");
            }
            if (rejoining.count(wanted) != 0) { throw std::runtime_error(wanted + " is already queued to join"); }
            Rejoin reserved;
            reserved.at = std::chrono::steady_clock::time_point::max();
            rejoining[wanted] = reserved;
        }
        RosterChange(Str(a, "name"), true);
        try { SpawnBot(Str(a, "name")); }
        catch (...)
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            rejoining.erase(Str(a, "name"));
            throw;
        }
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            rejoining.erase(Str(a, "name"));
        }
        return "connected " + Str(a, "name");
    }
    bool is_lua_tool = false;
    const Value lua_result = CallLuaTool(name, a, is_lua_tool);
    if (is_lua_tool)
    {
        return lua_result;
    }

    if (const std::shared_ptr<Child> child = TargetChild(a))
    {
        return name == "disconnect" ? DisconnectChild(child) : Forward(*child, name, a);
    }
    const std::shared_ptr<McpBot> target = TargetBot(a);
    McpBot& bot = *target;
    if (bot.GetShouldBeClosed() && name != "disconnect")
    {
        // ponytail: no auto-reconnect; add a reconnect loop if kicks are routine
        throw std::runtime_error("this bot was dropped by the server, it rejoins by itself after 60 seconds");
    }
    const std::shared_ptr<LocalPlayer> me = bot.GetLocalPlayer();
    const std::shared_ptr<World> world = bot.GetWorld();
    if (!me || !world || !bot.GetInventoryManager() || !bot.GetEntityManager())
    {
        throw std::runtime_error("this bot is not in the world yet (still joining, or just dropped by the server)");
    }

    if (name == "status")
    {
        static const char* modes[] = { "survival", "creative", "adventure", "spectator" };
        const int mode = static_cast<int>(me->GetGameMode());
        Value o;
        o["name"] = bot.GetNetworkManager()->GetMyName();
        o["position"] = PosJson(me->GetPosition());
        o["health"] = me->GetHealth();
        o["food"] = me->GetFood();
        o["air"] = me->GetDataAirSupplyId();
        o["game_mode"] = mode >= 0 && mode < 4 ? modes[mode] : "unknown";
        o["dimension"] = world->GetCurrentDimension();
        o["day_time"] = bot.GetDayTime();
        o["rain_level"] = bot.weather.rain.load();
        o["thunder_level"] = bot.weather.thunder.load();
        const Biome* biome = world->GetBiome(BlockPos(me->GetPosition()));
        o["biome"] = biome == nullptr ? "" : biome->GetName();
        const ProtocolCraft::Slot held = bot.GetInventoryManager()->GetHotbarSelected();
        o["held_item"] = held.IsEmptySlot() ? "" : AssetsManager::getInstance().GetItem(held.GetItemId())->GetName();
        Value players = Json::Array();
        for (const auto& [uuid, player_name] : *bot.GetOnlinePlayers())
        {
            players.push_back(player_name);
        }
        o["players_online"] = players;
        // Worn and held items, then what the bot carries (item -> total count)
        const std::map<short, ProtocolCraft::Slot> slots = PlayerSlots(bot);
        const auto item_at = [&](const short index) -> std::string
            {
                const auto slot = slots.find(index);
                return slot == slots.end() || slot->second.IsEmptySlot() ? "" : AssetsManager::getInstance().GetItem(slot->second.GetItemId())->GetName().substr(10);
            };
        o["equipment"]["head"] = item_at(Window::INVENTORY_HEAD_ARMOR);
        o["equipment"]["chest"] = item_at(Window::INVENTORY_CHEST_ARMOR);
        o["equipment"]["legs"] = item_at(Window::INVENTORY_LEGS_ARMOR);
        o["equipment"]["feet"] = item_at(Window::INVENTORY_FEET_ARMOR);
        o["equipment"]["main_hand"] = held.IsEmptySlot() ? "" : AssetsManager::getInstance().GetItem(held.GetItemId())->GetName().substr(10);
        o["equipment"]["off_hand"] = item_at(Window::INVENTORY_OFFHAND_INDEX);
        o["inventory"] = Json::Object();
        for (const auto& [index, slot] : slots)
        {
            if (index >= Window::INVENTORY_STORAGE_START && index != Window::INVENTORY_OFFHAND_INDEX && !slot.IsEmptySlot())
            {
                Value& count = o["inventory"][item_at(index)];
                count = (count.is_null() ? 0 : count.get_number<int>()) + slot.GetItemCount();
            }
        }
        // Remembered places: kind -> how many, and items remembered in chests
        o["memory"] = MemorySummary(bot);
        o["stored"] = StoredSummary(world->GetCurrentDimension());
        {
            std::lock_guard<std::mutex> lock(bot.team_mutex);
            o["team_leader"] = bot.team.leader;
            // Leader: what each member last reported in the chat. Member: the order it is carrying out
            o["team_reports"] = bot.team.reports;
            // Build objective: is one under way, and how many spots are left
            o["building"] = bot.team.building;
            o["build_name"] = bot.team.build_name;
            o["build_failed"] = static_cast<int>(bot.team.build_failed.size());
            // Spots of the plan not standing yet, as of the bot's last look (see build_idle), its own failures included
            o["build_left"] = bot.team.blueprint.empty() ? 0 : bot.team.build_left;
            // The whole plan stands as far as this bot can see, with no spot it had to give up on
            o["build_done"] = !bot.team.building && !bot.team.blueprint.empty() && bot.team.build_failed.empty();
            // Seconds since the bot last worked on the build: large means something else keeps it busy
            o["build_idle"] = static_cast<int>(std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() - bot.team.build_looked_at).count());
            o["team_order"] = !bot.team.has_order ? "" : (bot.team.order_what.empty() ? std::string("come") : "mine " + bot.team.order_what) + " at " +
                std::to_string(bot.team.order_at.x) + " " + std::to_string(bot.team.order_at.y) + " " + std::to_string(bot.team.order_at.z);
        }
        // Times the bot was detected stuck (busy but not moving for 20 seconds) and broke a block beside it
        o["times_stuck"] = bot.times_stuck.load();
        o["trade"] = TradeOf(o["name"].get_string());
        {
            std::lock_guard<std::mutex> lock(bot.stats_mutex);
            Value stats = Json::Object();
            for (const auto& [what, n] : bot.stats)
            {
                stats[what] = n;
            }
            o["stats"] = stats;
        }
        {
            // How many times the bot died since it connected, and where the last time
            const McpBot::Death death = bot.GetDeath();
            o["deaths"] = death.count;
            if (death.count > 0)
            {
                o["died_at"] = PosJson(death.at);
            }
        }
        o["state"] = bot.GetState();
        o["last_note"] = bot.GetNote();
        // What it was doing when it last left, from its memory
        o["was_doing"] = bot.was_doing;
        // What the bot was asked to make, and how many it must end up with
        o["making"] = bot.GetMake().first;
        o["making_until"] = bot.GetMake().second;
        o["gear"] = bot.gear.load();
        // Gear stages the bot gave up on, and why
        o["gear_skipped"] = bot.GetGearSkipped();
        o["autopilot"] = bot.autopilot.load();
        o["gather"] = bot.GetGoal().first;
        o["gather_left"] = bot.GetGoal().second;
        // Recent "state -> state" transitions, oldest first
        o["flow"] = bot.GetFlow();
        return o;
    }
    if (name == "chat")
    {
        const std::string message = Str(a, "message");
        if (!message.empty() && message[0] == '/')
        {
            bot.SendChatCommand(message.substr(1));
        }
        else
        {
            bot.SendChatMessage(message);
        }
        return "sent";
    }
    if (name == "read_chat")
    {
        Value lines = Json::Array();
        for (const std::string& line : bot.TakeChat())
        {
            lines.push_back(line);
        }
        return lines;
    }
    if (name == "inventory")
    {
        Value slots = Json::Array();
        for (const auto& [index, slot] : PlayerSlots(bot))
        {
            if (slot.IsEmptySlot())
            {
                continue;
            }
            Value o;
            o["slot"] = index;
            o["item"] = AssetsManager::getInstance().GetItem(slot.GetItemId())->GetName();
            o["count"] = slot.GetItemCount();
            slots.push_back(o);
        }
        return slots;
    }
    if (name == "entities")
    {
        const std::string type = a.is_object() && a.contains("type") && a["type"].is_string() ? a["type"].get_string() : "";
        Value list = Json::Array();
        for (const Ent& e : FindEntities(bot, type, Num(a, "radius", 32)))
        {
            Value o;
            o["id"] = e.id;
            o["type"] = e.name;
            o["position"] = PosJson(e.pos);
            o["distance"] = std::round(e.dist * 10.0) / 10.0;
            // A player: who it is
            const std::shared_ptr<Entity> entity = bot.GetEntityManager()->GetEntity(e.id);
            if (entity && entity->IsRemotePlayer())
            {
                o["name"] = bot.GetPlayerName(entity->GetUUID());
            }
            list.push_back(o);
        }
        return list;
    }
    if (name == "get_block")
    {
        Position p = Pos(a);
        const int bottom = a.contains("y_min") ? static_cast<int>(Num(a, "y_min")) : p.y;
        if (bottom > p.y || p.y - bottom > 384) { throw std::runtime_error("y_min must be within 384 blocks below y"); }
        const Blockstate* block = world->GetBlock(p);
        while (block != nullptr && p.y > bottom && (block->IsAir() || !block->IsSolid()))
        {
            --p.y;
            block = world->GetBlock(p);
        }
        if (block == nullptr)
        {
            throw std::runtime_error("position is not loaded");
        }
        Value o;
        o["block"] = block->GetName();
        o["state"] = block->GetVariables();
        o["y"] = p.y;
        return o;
    }
    if (name == "find_blocks")
    {
        // "_log" (a leading underscore): every block whose id ends so, e.g. all kinds of logs
        const std::string raw = Str(a, "block");
        const bool suffix = !raw.empty() && raw[0] == '_';
        const std::string id = suffix ? raw : Id(raw);
        Value list = Json::Array();
        for (const Position& p : FindBlocks(bot, [&](const Blockstate& b) { return suffix ? EndsWith(b.GetName(), id) : b.GetName() == id; },
            static_cast<int>(Num(a, "radius", 32)), static_cast<size_t>(std::max(1.0, Num(a, "max", 10)))))
        {
            Value o;
            o["x"] = p.x;
            o["y"] = p.y;
            o["z"] = p.z;
            list.push_back(o);
        }
        return list;
    }
    if (name == "goto")
    {
        const Position p = Pos(a);
        const int tolerance = static_cast<int>(Num(a, "tolerance", 0));
        return Act(bot, 120, "goto", [=](McpBot& c) { return GoTo(c, p, tolerance); });
    }
    if (name == "look_at")
    {
        const Vector3<double> p(Num(a, "x"), Num(a, "y"), Num(a, "z"));
        return Act(bot, 10, "look_at", [=](McpBot& c) { return LookAt(c, p); });
    }
    if (name == "dig")
    {
        const Position p = Pos(a);
        return Act(bot, 120, "dig", [=](McpBot& c) { return SafeDig(c, p, true); });
    }
    if (name == "place_block")
    {
        const Position p = Pos(a);
        const std::string item = Id(Str(a, "item"));
        return Act(bot, 120, "place_block", [=](McpBot& c) { return PlaceCheckedBlock(c, item, p, std::nullopt, true); });
    }
    if (name == "interact_block")
    {
        const Position p = Pos(a);
        return Act(bot, 120, "interact_block", [=](McpBot& c)
            {
                return GoTo(c, p, 4, 0, 1) == Status::Success ? InteractWithBlock(c, p) : Status::Failure;
            });
    }
    if (name == "equip")
    {
        const std::string item = Id(Str(a, "item"));
        const std::string slot = a.contains("slot") ? Str(a, "slot") : "hand";
        if (slot != "hand" && slot != "armor") { throw std::runtime_error("slot must be hand or armor"); }
        if (slot == "armor")
        {
            short destination = -1;
            for (const auto& piece : armor)
            {
                const std::string suffix = std::string(piece.item).substr(std::string(piece.item).find_last_of('_'));
                if (EndsWith(item, suffix)) { destination = piece.slot; break; }
            }
            if (destination < 0) { throw std::runtime_error("item must be a helmet, chestplate, leggings or boots"); }
            return Act(bot, 20, "equip armor", [=](McpBot& c) { return Wear(c, item, destination); });
        }
        return Act(bot, 20, "equip", [=](McpBot& c) { return SetItemInHand(c, item); });
    }
    if (name == "home")
    {
        // The bed in the bot's house: its own, slept in every night (the sleep branch walks back to it)
        if (a.is_object() && a.contains("forget") && a["forget"].is_bool() && a["forget"].get<bool>())
        {
            ForgetOwn(bot, "bed");
            return std::string("its bed is forgotten");
        }
        const Position bed(static_cast<int>(Num(a, "x")), static_cast<int>(Num(a, "y")), static_cast<int>(Num(a, "z")));
        SetOwn(bot, "bed", bed);
        return "its bed is at " + std::to_string(bed.x) + " " + std::to_string(bed.y) + " " + std::to_string(bed.z);
    }
    if (name == "eat")
    {
        const std::string item = Id(Str(a, "item"));
        return Act(bot, 30, "eat", [=](McpBot& c) { return Eat(c, item); });
    }
    if (name == "collect")
    {
        const std::string id = Id(Str(a, "block"));
        const int count = std::clamp(static_cast<int>(Num(a, "count", 1)), 1, 64);
        const int radius = static_cast<int>(Num(a, "radius", 32));
        const auto done = std::make_shared<std::atomic<int>>(0);
        const auto stored = std::make_shared<std::atomic<int>>(0);
        const bool finished = Run(bot, 300, name, [=](McpBot& c)
            {
                const Status result = CollectTask(c, id, count, radius, *done);
                std::string error;
                if (auto_store && *done > 0)
                {
                    StoreTask(c, false, *stored, error);
                }
                return result;
            });
        Value o;
        o["stored_in_chests"] = stored->load();
        o["collected"] = done->load();
        o["requested"] = count;
        o["note"] = finished ? "done" : "stopped early: no more reachable blocks of this type in range, or timed out";
        return o;
    }
    if (name == "kill")
    {
        const std::string type = Str(a, "type");
        const int count = std::clamp(static_cast<int>(Num(a, "count", 1)), 1, 64);
        const double radius = Num(a, "radius", 24);
        const auto done = std::make_shared<std::atomic<int>>(0);
        const bool finished = Run(bot, 300, name, [=](McpBot& c) { return KillTask(c, type, count, radius, *done); });
        Value o;
        o["killed"] = done->load();
        o["requested"] = count;
        o["note"] = finished ? "done" : "stopped early: no more reachable targets of this type in range, or timed out";
        return o;
    }
    if (name == "make")
    {
        const std::string item = Id(Str(a, "item"));
        const int count = std::clamp(static_cast<int>(Num(a, "count", 1)), 0, 2304);
        if (count > 0 && recipes.count(item) == 0 && smelt_inputs.count(item) == 0)
        {
            throw std::runtime_error(item + " is neither crafted nor smelted: use collect or the autopilot's gather for raw materials");
        }
        std::lock_guard<std::mutex> lock(bot.run_mutex);
        bot.SetMake(item, count);
        if (count > 0)
        {
            bot.autopilot = true;
        }
        bot.ResumeDefault();
        Value o;
        o["making"] = count > 0 ? item : "";
        o["until_it_has"] = count;
        o["has"] = CountItem(bot, item);
        o["note"] = count > 0 ? "the autopilot works on it: see state and last_note in status" : "make goal dropped";
        return o;
    }
    if (name == "shear")
    {
        const int count = std::clamp(static_cast<int>(Num(a, "count", 4)), 1, 64);
        const double radius = Num(a, "radius", 32);
        const auto sheared = std::make_shared<std::atomic<int>>(0);
        const auto wool = std::make_shared<std::atomic<int>>(0);
        const auto error = std::make_shared<std::string>();
        Run(bot, 300, name, [=](McpBot& c)
            {
                std::string craft_error;
                if (CountItem(c, "minecraft:shears") == 0 && CraftOne(c, "minecraft:shears", craft_error) == 0)
                {
                    *error = "needs shears, and could not make them (2 iron ingots): " + craft_error;
                    return Status::Failure;
                }
                const int before = CountWool(c);
                const Status result = ShearTask(c, count, radius, *sheared);
                *wool = CountWool(c) - before;
                return result;
            });
        if (!error->empty())
        {
            throw std::runtime_error(*error);
        }
        Value o;
        o["sheep_sheared"] = sheared->load();
        o["wool_gained"] = wool->load();
        o["note"] = *sheared > 0 ? "the sheep live and grow their wool back by grazing" : "no sheep with wool on them within range";
        return o;
    }
    if (name == "fish")
    {
        const int casts = std::clamp(static_cast<int>(Num(a, "count", 5)), 1, 32);
        const auto caught = std::make_shared<std::atomic<int>>(0);
        const auto error = std::make_shared<std::string>();
        Run(bot, 60 + casts * 45, name, [=](McpBot& c)
            {
                std::string craft_error;
                if (CountItem(c, "minecraft:fishing_rod") == 0 && CraftOne(c, "minecraft:fishing_rod", craft_error) == 0)
                {
                    *error = "needs a fishing rod, and could not make one (3 sticks, 2 string): " + craft_error;
                    return Status::Failure;
                }
                return FishTask(c, casts, *caught, *error);
            });
        if (!error->empty())
        {
            throw std::runtime_error(*error);
        }
        Value o;
        o["caught"] = caught->load();
        o["note"] = *caught > 0 ? "raw fish: cook it (make cooked_cod / cooked_salmon)" : "nothing bit";
        return o;
    }
    if (name == "milk")
    {
        const int count = std::clamp(static_cast<int>(Num(a, "count", 1)), 1, 16);
        const double radius = Num(a, "radius", 24);
        const auto milked = std::make_shared<std::atomic<int>>(0);
        const auto error = std::make_shared<std::string>();
        Run(bot, 300, name, [=](McpBot& c)
            {
                std::string craft_error;
                if (CountItem(c, "minecraft:bucket") == 0 && CraftOne(c, "minecraft:bucket", craft_error) == 0)
                {
                    *error = "needs an empty bucket, and could not make one (3 iron ingots): " + craft_error;
                    return Status::Failure;
                }
                return MilkTask(c, count, radius, *milked);
            });
        if (!error->empty())
        {
            throw std::runtime_error(*error);
        }
        Value o;
        o["milked"] = milked->load();
        o["note"] = *milked > 0 ? "milk buckets for the bakers' cakes" : "no cow within range";
        return o;
    }
    if (name == "dye")
    {
        std::string color = Str(a, "color");
        std::transform(color.begin(), color.end(), color.begin(), [](unsigned char ch) { return std::tolower(ch); });
        const int count = std::clamp(static_cast<int>(Num(a, "count", 1)), 1, 64);
        const bool on_sheep = a.contains("sheep") && a["sheep"].is_bool() && a["sheep"].get<bool>();
        const std::string dye = "minecraft:" + color + "_dye";
        if (AssetsManager::getInstance().GetItem(dye) == nullptr)
        {
            throw std::runtime_error("no such dye: " + color + " (white, orange, magenta, light_blue, yellow, lime, pink, gray, light_gray, cyan, purple, blue, brown, green, red, black)");
        }
        const auto dyed = std::make_shared<std::atomic<int>>(0);
        const auto error = std::make_shared<std::string>();
        Run(bot, 300, name, [=](McpBot& c)
            {
                std::set<int> tried;
                while (*dyed < count)
                {
                    // The dye itself: from the inventory, or made from a flower or another source the bot carries
                    if (CountItem(c, dye) == 0 && CraftOne(c, dye, *error) == 0)
                    {
                        *error = "no " + color + " dye, and could not make one: " + *error;
                        return Status::Failure;
                    }
                    if (!on_sheep)
                    {
                        // One dye and one wool of any colour give one dyed wool
                        if (CraftOne(c, "minecraft:" + color + "_wool", *error) == 0)
                        {
                            return Status::Failure;
                        }
                        ++*dyed;
                        continue;
                    }
                    // On the animal: from then on it gives wool of that colour every time it is shorn
                    const std::vector<Ent> sheep = FindEntities(c, "sheep", 32.0);
                    const auto next = std::find_if(sheep.begin(), sheep.end(), [&](const Ent& e) { return tried.count(e.id) == 0; });
                    if (next == sheep.end())
                    {
                        *error = "no more sheep within 32 blocks";
                        return Status::Failure;
                    }
                    tried.insert(next->id);
                    if (SetItemInHand(c, dye) == Status::Success && InteractEntity(c, next->id, Hand::Main, true) == Status::Success)
                    {
                        ++*dyed;
                        Wait(c, 300);
                    }
                }
                return Status::Success;
            });
        if (*dyed == 0)
        {
            throw std::runtime_error(error->empty() ? "dye timed out" : *error);
        }
        Value o;
        o[on_sheep ? "sheep_dyed" : "wool_dyed"] = dyed->load();
        o["note"] = *dyed >= count ? "done" : "stopped early: " + *error;
        return o;
    }
    if (name == "lure")
    {
        const std::string type = Str(a, "type");
        const std::string food = a.contains("food") && a["food"].is_string() ? Id(a["food"].get_string()) : LureFood(type);
        const Position destination = Pos(a);
        if (food.empty())
        {
            throw std::runtime_error("don't know what a " + type + " follows: give the food (the item it is bred with)");
        }
        const auto arrived = std::make_shared<std::atomic<int>>(0);
        const auto error = std::make_shared<std::string>();
        Run(bot, 600, name, [=](McpBot& c)
            {
                if (CountItem(c, food) == 0)
                {
                    *error = "needs " + food + " in the inventory: a " + type + " follows whoever holds it";
                    return Status::Failure;
                }
                if (FindEntities(c, type, 48.0).empty())
                {
                    *error = "no " + type + " within 48 blocks";
                    return Status::Failure;
                }
                return LureTask(c, type, food, destination, *arrived);
            });
        if (!error->empty())
        {
            throw std::runtime_error(*error);
        }
        Value o;
        o["arrived"] = arrived->load();
        o["note"] = *arrived > 0 ? "they stay only while the food is held: close the pen (place_block a fence gate), then breed" : "none followed all the way: they lose interest beyond 10 blocks, and can't cross water or climb";
        return o;
    }
    if (name == "breed")
    {
        const std::string type = Str(a, "type");
        const std::string food = Id(Str(a, "food"));
        const double radius = Num(a, "radius", 16);
        const auto fed = std::make_shared<std::atomic<int>>(0);
        const auto babies = std::make_shared<std::atomic<int>>(0);
        Run(bot, 120, name, [=](McpBot& c)
            {
                for (const Ent& e : FindEntities(c, type, radius))
                {
                    // A baby fed its food grows up sooner (a tenth of what is left, each time): up to 4 of them
                    const std::shared_ptr<AgeableMobEntity> ageable = std::dynamic_pointer_cast<AgeableMobEntity>(c.GetEntityManager()->GetEntity(e.id));
                    if (ageable && ageable->GetDataBabyId())
                    {
                        if (*babies < 4 && SetItemInHand(c, food) == Status::Success && InteractEntity(c, e.id, Hand::Main, true) == Status::Success)
                        {
                            ++*babies;
                            Wait(c, 300);
                        }
                        continue;
                    }
                    if (*fed >= 2)
                    {
                        continue;
                    }
                    // Feeding consumes the item, so re-equip before each animal
                    if (SetItemInHand(c, food) == Status::Failure)
                    {
                        return Status::Failure;
                    }
                    if (InteractEntity(c, e.id, Hand::Main, true) == Status::Success)
                    {
                        ++*fed;
                        Count(c, "bred");
                        Wait(c, 300);
                    }
                }
                return *fed >= 2 ? Status::Success : Status::Failure;
            });
        if (*fed < 2)
        {
            throw std::runtime_error("fed " + std::to_string(fed->load()) + "/2 animals: need two reachable '" + type + "' in range and at least 2 " + food + " in the inventory");
        }
        return "fed 2 " + type + " with " + food + (*babies > 0 ? ", and " + std::to_string(babies->load()) + " babies to make them grow up sooner" : "") +
            " (adults bred less than 5 minutes ago ignore food, check with the entities tool)";
    }
    if (name == "farm")
    {
        const int radius = static_cast<int>(Num(a, "radius", 16));
        const bool replant = !(a.is_object() && a.contains("replant") && a["replant"].is_bool() && !a["replant"].get<bool>());
        const auto harvested = std::make_shared<std::atomic<int>>(0);
        const auto replanted = std::make_shared<std::atomic<int>>(0);
        const auto stored = std::make_shared<std::atomic<int>>(0);
        const bool finished = Run(bot, 300, name, [=](McpBot& c)
            {
                const Status result = FarmTask(c, radius, replant, *harvested, *replanted);
                std::string error;
                if (auto_store && *harvested > 0)
                {
                    StoreTask(c, false, *stored, error);
                }
                return result;
            });
        Value o;
        o["stored_in_chests"] = stored->load();
        o["harvested"] = harvested->load();
        o["replanted"] = replanted->load();
        o["note"] = finished ? (harvested->load() == 0 ? "no fully grown crops in range" : "done") : "timed out";
        return o;
    }
    if (name == "autopilot")
    {
        if (!a.is_object() || !a.contains("enabled") || !a["enabled"].is_bool())
        {
            throw std::runtime_error("missing boolean argument: enabled");
        }
        // Not while a tool action (e.g. from a Lua chat handler) owns the tree
        std::lock_guard<std::mutex> lock(bot.run_mutex);
        bot.autopilot = a["enabled"].get<bool>();
        if (a.contains("gear") && a["gear"].is_bool())
        {
            if (a["gear"].get<bool>())
            {
                bot.ResetGear();
            }
            else
            {
                bot.gear = false;
            }
        }
        if (a.contains("gather") && a["gather"].is_string())
        {
            bot.SetGoal(Id(a["gather"].get_string()), static_cast<int>(Num(a, "count", 16)));
            bot.explorations = 0;
            bot.SetNote("");
        }
        bot.ResumeDefault();
        Value o;
        o["autopilot"] = bot.autopilot.load();
        o["gear"] = bot.gear.load();
        o["gather"] = bot.GetGoal().first;
        o["gather_left"] = bot.GetGoal().second;
        return o;
    }
    if (name == "recipe")
    {
        const std::string item = Id(Str(a, "item"));
        Value list = Json::Array();
        const auto range = recipes.equal_range(item);
        for (auto it = range.first; it != range.second; ++it)
        {
            const Recipe& recipe = it->second;
            Value o;
            o["makes"] = recipe.count;
            if (!recipe.pattern.empty())
            {
                o["pattern"] = recipe.pattern;
                o["key"] = Json::Object();
                for (const auto& [letter, text] : recipe.key_text)
                {
                    o["key"][std::string(1, letter)] = text;
                }
                o["needs_crafting_table"] = recipe.pattern.size() > 2 || std::any_of(recipe.pattern.begin(), recipe.pattern.end(), [](const std::string& row) { return row.size() > 2; });
            }
            else
            {
                o["ingredients"] = recipe.ingredient_text;
                o["needs_crafting_table"] = recipe.ingredients.size() > 4;
            }
            list.push_back(o);
        }
        Value o;
        o["item"] = item;
        o["crafting"] = list;
        o["smelted_from"] = smelted_from.count(item) != 0 ? smelted_from[item] : "";
        return o;
    }
    if (name == "craft_item")
    {
        const std::string item = Id(Str(a, "item"));
        const int count = std::clamp(static_cast<int>(Num(a, "count", 1)), 1, 64);
        if (recipes.count(item) == 0)
        {
            throw std::runtime_error(smelted_from.count(item) != 0 ? item + " is not crafted: it is smelted from " + smelted_from[item] + " in a furnace" :
                "there is no crafting recipe for " + item + " (check the id with the recipe tool)");
        }
        const auto made = std::make_shared<std::atomic<int>>(0);
        const auto error = std::make_shared<std::string>();
        Run(bot, 180, name, [=](McpBot& c)
            {
                while (*made < count)
                {
                    const int crafted = CraftOne(c, item, *error);
                    if (crafted == 0)
                    {
                        return Status::Failure;
                    }
                    *made += crafted;
                }
                return Status::Success;
            });
        if (*made == 0)
        {
            throw std::runtime_error(error->empty() ? "craft_item timed out" : *error);
        }
        Value o;
        o["crafted"] = made->load();
        o["note"] = *made >= count ? "done" : "stopped early: " + *error;
        return o;
    }
    if (name == "make_farm")
    {
        const int radius = std::clamp(static_cast<int>(Num(a, "radius", 4)), 1, 4);
        const auto tilled = std::make_shared<std::atomic<int>>(0);
        const auto planted = std::make_shared<std::atomic<int>>(0);
        const auto error = std::make_shared<std::string>();
        Run(bot, 300, name, [=](McpBot& c)
            {
                const std::shared_ptr<World> w = c.GetWorld();
                const std::string hoe = ItemLike(c, "_hoe");
                if (hoe.empty())
                {
                    *error = "needs a hoe in the inventory (craft_item wooden_hoe)";
                    return Status::Failure;
                }
                // Water hydrates farmland up to 4 blocks away on its own level: the plot is the soil around a water block
                const auto is_soil = [&](const Position& p)
                    {
                        const Blockstate* block = w->GetBlock(p);
                        const Blockstate* above = w->GetBlock(p + Position(0, 1, 0));
                        return block != nullptr && above != nullptr && above->IsAir() && (block->GetName() == "minecraft:dirt" || block->GetName() == "minecraft:grass_block");
                    };
                const std::vector<Position> waters = FindBlocks(c, [](const Blockstate& b) { return b.IsWater(); }, 32, 400);
                const auto water = std::find_if(waters.begin(), waters.end(), [&](const Position& p)
                    {
                        return is_soil(p + Position(1, 0, 0)) || is_soil(p + Position(-1, 0, 0)) || is_soil(p + Position(0, 0, 1)) || is_soil(p + Position(0, 0, -1));
                    });
                Position center;
                if (water != waters.end())
                {
                    center = *water;
                }
                else
                {
                    // Village farm plots built next to homes have a composter on the rim and a 5x5 dirt bed beside it
                    const std::vector<Position> composters = FindBlocks(c, [](const Blockstate& b) { return b.GetName() == "minecraft:composter"; }, 32, 64);
                    const auto composter = std::find_if(composters.begin(), composters.end(), [&](const Position& p)
                        {
                            for (int dx = -4; dx <= 4; ++dx)
                            {
                                for (int dz = -4; dz <= 4; ++dz)
                                {
                                    if (is_soil(p + Position(dx, -1, dz)) || is_soil(p + Position(dx, 0, dz)))
                                    {
                                        return true;
                                    }
                                }
                            }
                            return false;
                        });
                    if (composter == composters.end())
                    {
                        *error = "no water with dirt or grass beside it within 32 blocks: farmland has to be within 4 blocks of water";
                        return Status::Failure;
                    }
                    center = *composter + Position(3, -1, 0);
                    if (!is_soil(center) && !is_soil(center + Position(1, 0, 0)))
                    {
                        center = *composter;
                    }
                    // If the bot has a water bucket, pour it in the middle of the plot so the farmland stays hydrated
                    if (CountItem(c, "minecraft:water_bucket") > 0)
                    {
                        const Blockstate* mid = w->GetBlock(center);
                        if (mid != nullptr && !mid->IsWater() && (mid->GetName() == "minecraft:dirt" || mid->GetName() == "minecraft:grass_block") &&
                            GoTo(c, center, 3) == Status::Success && Dig(c, center, true) == Status::Success)
                        {
                            UseHeldAt(c, "minecraft:water_bucket", Vector3<double>(center.x + 0.5, center.y, center.z + 0.5));
                        }
                    }
                }
                for (int dx = -radius; dx <= radius; ++dx)
                {
                    for (int dz = -radius; dz <= radius; ++dz)
                    {
                        const Position soil = center + Position(dx, 0, dz);
                        if (!is_soil(soil) || GoTo(c, soil, 3) == Status::Failure)
                        {
                            continue;
                        }
                        // Right-click with the hoe turns the soil into farmland
                        if (SetItemInHand(c, hoe) == Status::Failure || InteractWithBlock(c, soil, PlayerDiggingFace::Up) == Status::Failure)
                        {
                            continue;
                        }
                        ++*tilled;
                        Wait(c, 250);
                        // Rows of different crops side by side grow faster (a third more often, says the wiki):
                        // each row along x gets the next kind the bot carries
                        std::vector<std::string> kinds;
                        for (const char* seed : { "minecraft:wheat_seeds", "minecraft:carrot", "minecraft:potato", "minecraft:beetroot_seeds" })
                        {
                            if (CountItem(c, seed) > 0)
                            {
                                kinds.push_back(seed);
                            }
                        }
                        if (!kinds.empty())
                        {
                            const std::string& seed = kinds[((soil.z % static_cast<int>(kinds.size())) + kinds.size()) % kinds.size()];
                            if (PlaceBlock(c, seed, soil + Position(0, 1, 0), PlayerDiggingFace::Up, true) == Status::Success)
                            {
                                ++*planted;
                            }
                        }
                    }
                }
                LightUp(c, center + Position(0, 1, 0), true);
                return Status::Success;
            });
        if (!error->empty())
        {
            throw std::runtime_error(*error);
        }
        Value o;
        o["tilled"] = tilled->load();
        o["planted"] = planted->load();
        o["note"] = *planted < *tilled ? "tilled soil without a crop turns back to dirt: get seeds (break short_grass with the collect tool) and run make_farm again, or place_block them" : "done; the farm tool harvests and replants when the crops are ripe";
        return o;
    }
    if (name == "use_item")
    {
        const Vector3<double> at(Num(a, "x") + 0.5, Num(a, "y"), Num(a, "z") + 0.5);
        const std::string item = a.contains("item") && a["item"].is_string() ? Id(a["item"].get_string()) : "";
        return Act(bot, 60, "use_item", [=](McpBot& c)
            {
                const ProtocolCraft::Slot held = c.GetInventoryManager()->GetHotbarSelected();
                const std::string in_hand = held.IsEmptySlot() ? "" : AssetsManager::getInstance().GetItem(held.GetItemId())->GetName();
                return UseHeldAt(c, item.empty() ? in_hand : item, at);
            });
    }
    if (name == "travel")
    {
        const double x = Num(a, "x");
        const double z = Num(a, "z");
        return Act(bot, 900, "travel", [=](McpBot& c) { return TravelTask(c, x, z); });
    }
    if (name == "locate")
    {
        const std::string kind = Str(a, "kind");
        if (kind != "biome" && kind != "structure" && kind != "poi")
        {
            throw std::runtime_error("kind must be biome, structure or poi");
        }
        const std::string what = Str(a, "name");
        // The answer is a system message: look at the ones that arrive after the command
        const long long before = bot.GetSystemCount();
        bot.SendChatCommand("locate " + kind + " " + (what.rfind('#', 0) == 0 ? what : Id(what)));
        std::string answer;
        Utilities::WaitForCondition([&]()
            {
                const std::vector<std::string> lines = bot.GetSystemLinesAfter(before);
                answer = lines.empty() ? "" : lines.front();
                return !lines.empty();
            }, 8000);
        std::smatch found;
        // "... chat.coordinates 120 ~ -340 ..." once flattened, or "[120, ~, -340]" as plain text
        if (!std::regex_search(answer, found, std::regex(R"((-?\d+),? (~|-?\d+),? (-?\d+))")) || answer.find("locate") == std::string::npos)
        {
            throw std::runtime_error(answer.empty() ? "the server did not answer /locate" :
                "locate failed, the server answered: " + answer + ". The bot must be an operator (run `op " + bot.GetNetworkManager()->GetMyName() + "` in the server console), and the name must be a valid " + kind + " id");
        }
        Value o;
        o["x"] = std::stoi(found[1]);
        o["z"] = std::stoi(found[3]);
        o["answer"] = answer;
        if (a.contains("go") && a["go"].is_bool() && a["go"].get<bool>())
        {
            const double x = std::stoi(found[1]);
            const double z = std::stoi(found[3]);
            o["arrived"] = Run(bot, 900, "travel", [=](McpBot& c) { return TravelTask(c, x, z); });
        }
        return o;
    }
    // One bot's part of the team tool (also how the main process tells a bot where its leader is)
    if (name == "team_member")
    {
        if (a.is_object() && a.contains("objective") && a["objective"].is_string())
        {
            // The order, for the next time it joins. Not a build plan: the castle (or whoever gave it) hands it over again
            const std::string objective = a["objective"].get_string();
            const bool solo = a.contains("solo") && a["solo"].is_bool() && a["solo"].get<bool>();
            if (objective == "build" || (objective == "disband" && !solo))
            {
                SaveDoing(bot, "team", "");
            }
            else
            {
                Value order = a;
                order.get_object().erase("bot");
                SaveDoing(bot, "team", order.Dump());
            }
        }
        if (a.is_object() && a.contains("leader_x"))
        {
            std::lock_guard<std::mutex> lock(bot.team_mutex);
            bot.team.leader_at = Vector3<double>(Num(a, "leader_x"), Num(a, "leader_y"), Num(a, "leader_z"));
            bot.team.leader_located = true;
            return "ok";
        }
        const std::string objective = Str(a, "objective");
        Team team;
        team.leader = Str(a, "leader");
        if (a.contains("members") && a["members"].is_array())
        {
            for (size_t i = 0; i < a["members"].size(); ++i)
            {
                team.members.push_back(a["members"][i].get_string());
            }
        }
        if (objective == "gather")
        {
            // Finds of these are reported in the chat; ore is spread out, so the team may spread out too
            team.wanted = SplitSpec(Str(a, "block"));
            team.follow_distance = 48.0;
        }
        if (a.contains("site") && a["site"].is_array() && a["site"].size() == 6)
        {
            // [x1, y1, z1, x2, y2, z2]
            team.has_site = true;
            team.site_min = Position(a["site"][0].get_number<int>(), a["site"][1].get_number<int>(), a["site"][2].get_number<int>());
            team.site_max = Position(a["site"][3].get_number<int>(), a["site"][4].get_number<int>(), a["site"][5].get_number<int>());
        }
        const int count = std::max(static_cast<int>(Num(a, "count", 16)), 1);
        if (objective == "attack")
        {
            team.hunt_type = Str(a, "type");
            team.hunt_left = count;
        }
        else if (objective == "build")
        {
            const std::string plan = a.contains("plan") && a["plan"].is_string() ? a["plan"].get_string() : "box";
            const std::string item = plan == "blocks" ? "" : Id(Str(a, "item"));
            const Position corner(static_cast<int>(Num(a, "x1", 0)), static_cast<int>(Num(a, "y1", 0)), static_cast<int>(Num(a, "z1", 0)));
            if (plan == "shelter")
            {
                team.blueprint = ShelterPlan(corner, std::clamp(static_cast<int>(Num(a, "size", 5)), 4, 15), item);
            }
            else if (plan == "box")
            {
                team.blueprint = BoxPlan(corner, Position(static_cast<int>(Num(a, "x2")), static_cast<int>(Num(a, "y2")), static_cast<int>(Num(a, "z2"))), item);
            }
            else if (plan == "blocks")
            {
                // Any structure, block by block: [[x, y, z, "item"], ...], "air" to clear a spot
                if (!a.contains("blocks") || !a["blocks"].is_array())
                {
                    throw std::runtime_error("plan=blocks needs blocks: [[x, y, z, item], ...]");
                }
                for (size_t i = 0; i < a["blocks"].size(); ++i)
                {
                    const Value& entry = a["blocks"][i];
                    // "item", or for a sign "item|line 1|line 2|line 3|line 4": the text is kept as written
                    const std::string& what = entry[3].get_string();
                    const size_t bar = what.find('|');
                    team.blueprint.push_back({ Position(entry[0].get_number<int>(), entry[1].get_number<int>(), entry[2].get_number<int>()),
                        Id(what.substr(0, bar)) + (bar == std::string::npos ? "" : what.substr(bar)) });
                }
            }
            else
            {
                throw std::runtime_error("plan must be box, shelter or blocks");
            }
            if (team.blueprint.size() > 4096)
            {
                throw std::runtime_error("too big to build: 4096 blocks at most");
            }
            for (const auto& entry : team.blueprint)
            {
                if (entry.second.rfind("minecraft:air", 0) != 0)
                {
                    team.planned.insert(entry.first);
                }
            }
            const std::string site_name = SiteName(a, plan);
            if (team.has_site)
            {
                AddSite(team.site_min, team.site_max, site_name);
            }
            else if (team.blueprint.size() >= 8 && !EndsWith(site_name, "_road"))
            {
                // (Not for a torch or two, nor an L-shaped road whose bounding box would fence off the village)
                Position low = team.blueprint.front().first, high = low;
                for (const auto& entry : team.blueprint)
                {
                    low = Position(std::min(low.x, entry.first.x), std::min(low.y, entry.first.y), std::min(low.z, entry.first.z));
                    high = Position(std::max(high.x, entry.first.x), std::max(high.y, entry.first.y), std::max(high.z, entry.first.z));
                }
                // ponytail: a sparse plan (a diagonal road) is not stored, its box would fence off the whole
                // countryside; store one box per segment if roads get dug up
                if (static_cast<long>(high.x - low.x + 1) * (high.z - low.z + 1) <= 4L * static_cast<long>(team.blueprint.size()))
                {
                    AddSite(low, high, site_name);
                }
            }
            team.building = true;
            team.build_name = a.contains("name") ? Str(a, "name") : plan;
            team.build_left = static_cast<int>(team.blueprint.size());
            // The site keeps a building team together, not the leader (who may be off getting wood)
            team.follow_distance = 1.0e9;
        }
        // Not while a tool action owns the tree
        std::lock_guard<std::mutex> run_lock(bot.run_mutex);
        {
            std::lock_guard<std::mutex> lock(bot.team_mutex);
            static std::atomic<int> plans = 0;
            team.plan_id = ++plans;
            bot.team = team;
        }
        if (objective == "build" || objective == "disband")
        {
            bot.SetGoal("", 0);
            bot.SetMake("", 0);
        }
        if (objective == "gather")
        {
            if (Str(a, "block").empty())
            {
                throw std::runtime_error("gather needs a block id");
            }
            bot.SetGoal(Id(Str(a, "block")), count);
            bot.explorations = 0;
            if (Str(a, "block").find("_ore") != std::string::npos)
            {
                // Mining needs the pickaxes: retry the tool stages this bot may have given up on
                bot.ResetGear();
            }
        }
        if (objective != "disband")
        {
            bot.autopilot = true;
        }
        bot.SetNote(objective == "disband" ? "team disbanded" : "team objective: " + objective + (team.leader == bot.GetNetworkManager()->GetMyName() ? " (leader)" : ", following " + team.leader));
        bot.ResumeDefault();
        return "ok";
    }
    if (name == "give")
    {
        const std::string to = Str(a, "to");
        const Position at(static_cast<int>(Num(a, "x", 0)), static_cast<int>(Num(a, "y", 0)), static_cast<int>(Num(a, "z", 0)));
        const std::string item = a.contains("item") && a["item"].is_string() ? a["item"].get_string() : "food";
        const int count = std::clamp(static_cast<int>(Num(a, "count", 16)), 1, 2304);
        const auto given = std::make_shared<std::atomic<int>>(0);
        Run(bot, 180, name, [=](McpBot& c) { return GiveTask(c, to, at, item, count, *given); });
        Value o;
        o["given"] = given->load();
        o["to"] = to;
        o["item"] = item;
        return o;
    }
    if (name == "drop")
    {
        const auto dropped = std::make_shared<std::atomic<int>>(0);
        const std::string to = a.is_object() && a.contains("to") && a["to"].is_string() ? a["to"].get_string() : "";
        Run(bot, 30, name, [=](McpBot& c) { return DropTask(c, to, *dropped); });
        Value o;
        o["dropped"] = dropped->load();
        return o;
    }
    if (name == "recall")
    {
        Value list = Json::Array();
        for (const auto& [kind, p] : Recall(bot, Str(a, "kind"), std::clamp(static_cast<int>(Num(a, "max", 5)), 1, 100)))
        {
            Value o;
            o["kind"] = kind;
            o["x"] = p.x;
            o["y"] = p.y;
            o["z"] = p.z;
            list.push_back(o);
        }
        return list;
    }
    if (name == "store")
    {
        const bool everything = a.is_object() && a.contains("everything") && a["everything"].is_bool() && a["everything"].get<bool>();
        const auto stored = std::make_shared<std::atomic<int>>(0);
        const auto error = std::make_shared<std::string>();
        const bool finished = Run(bot, 180, name, [=](McpBot& c) { return StoreTask(c, everything, *stored, *error); });
        Value o;
        o["stored"] = stored->load();
        o["note"] = finished ? "done" : (error->empty() ? "timed out" : *error);
        return o;
    }
    if (name == "disconnect")
    {
        return DisconnectBot(target);
    }
    if (name == "snapshot")
    {
        if (snapshot_path.empty())
        {
            throw std::runtime_error("snapshots are off: start the server with --snapshot FILE");
        }
        // A fresh one, so the image is what the bot sees now
        Renderer::Settings::TakeScreenshot(snapshot_path);
        Utilities::SleepFor(std::chrono::milliseconds(500));
        // The renderer rewrites the file every 5 seconds: retry if we catch it half written
        for (int attempt = 0; attempt < 5; ++attempt)
        {
            std::ifstream file(snapshot_path, std::ios::binary);
            const std::string png((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
            if (png.size() > 12 && png.compare(png.size() - 8, 4, "IEND") == 0)
            {
                Value o;
                o["__image"] = Base64(png);
                return o;
            }
            Utilities::SleepFor(std::chrono::milliseconds(200));
        }
        throw std::runtime_error("no snapshot available yet (the renderer needs a display, and a few seconds after startup)");
    }
    if (name == "craft")
    {
        if (!a.is_object() || !a.contains("grid") || !a["grid"].is_array() || a["grid"].size() > 3)
        {
            throw std::runtime_error("grid must be an array of up to 3 rows of up to 3 item names");
        }
        std::array<std::array<std::string, 3>, 3> grid;
        int min_x = 3, max_x = -1, min_y = 3, max_y = -1;
        for (size_t y = 0; y < a["grid"].size(); ++y)
        {
            const Value& row = a["grid"][y];
            if (!row.is_array() || row.size() > 3)
            {
                throw std::runtime_error("grid must be an array of up to 3 rows of up to 3 item names");
            }
            for (size_t x = 0; x < row.size(); ++x)
            {
                const std::string item = row[x].is_string() ? row[x].get_string() : "";
                if (item.empty() || Id(item) == "minecraft:air")
                {
                    continue;
                }
                grid[y][x] = Id(item);
                if (AssetsManager::getInstance().GetItem(grid[y][x]) == nullptr)
                {
                    throw std::runtime_error("unknown item: " + grid[y][x]);
                }
                min_x = std::min(min_x, static_cast<int>(x));
                max_x = std::max(max_x, static_cast<int>(x));
                min_y = std::min(min_y, static_cast<int>(y));
                max_y = std::max(max_y, static_cast<int>(y));
            }
        }
        if (max_x < 0)
        {
            throw std::runtime_error("grid is empty");
        }
        const auto error = std::make_shared<std::string>();
        const bool ok = Run(bot, 120, name, [=](McpBot& c) { return CraftAt(c, grid, *error); });
        if (!ok && error->empty())
        {
            *error = "craft timed out";
        }
        if (!ok)
        {
            throw std::runtime_error(*error);
        }
        return "crafted";
    }
    throw std::runtime_error("unknown tool: " + name);
}


// ---------------------------------------------------------------- Lua plugins

// Plugins are the *.lua files of the plugins directory, run once at startup with a global `bot`:
//   bot.call(tool, args)   run any tool of this server, returns its result (raises on failure)
//   bot.tool{name=, description=, params={arg="number"|"string"|"boolean"}, run=function(args) end}
//                          add a tool the LLM (and other plugins) can call
//   bot.on_chat(function(line) end)   react to chat lines
//   bot.every(seconds, function() end)  do something regularly (watch a long job, start its next step)
//   bot.log(message)
static lua_State* lua = nullptr;
// Lua states are single threaded: the MCP thread (plugin tools) and the chat thread (handlers) share it
static std::recursive_mutex lua_mutex;
// MCP tool definitions, built-in ones then the plugins'
static Value tools;
// Plugin tool name -> registry reference of its run function
static std::map<std::string, int> lua_tools;
// The plugin tools' names, under their own lock: whether a call is for a plugin is known without waiting for
// the Lua lock, which a plugin can hold for minutes (a timer walking a bot somewhere). Every built-in tool,
// status included, waited behind it: the control panel froze
static std::set<std::string> lua_tool_names;
static std::mutex lua_tool_names_mutex;
static std::vector<int> lua_chat_handlers;
// bot.every(seconds, function): called again and again, on the plugin thread
struct LuaTimer
{
    int function;
    std::chrono::seconds interval;
    std::chrono::steady_clock::time_point next;
};
static std::vector<LuaTimer> lua_timers;

static void PushJson(lua_State* L, const Value& v)
{
    if (v.is_string())
    {
        lua_pushlstring(L, v.get_string().data(), v.get_string().size());
    }
    else if (v.is_bool())
    {
        lua_pushboolean(L, v.get<bool>());
    }
    else if (v.is_integer())
    {
        lua_pushinteger(L, v.get_number<long long int>());
    }
    else if (v.is_number())
    {
        lua_pushnumber(L, v.get_number<double>());
    }
    else if (v.is_array())
    {
        lua_createtable(L, static_cast<int>(v.size()), 0);
        for (size_t i = 0; i < v.size(); ++i)
        {
            PushJson(L, v[i]);
            lua_rawseti(L, -2, static_cast<lua_Integer>(i + 1));
        }
    }
    else if (v.is_object())
    {
        lua_newtable(L);
        for (const auto& [key, value] : v.get_object())
        {
            PushJson(L, value);
            lua_setfield(L, -2, key.c_str());
        }
    }
    else
    {
        lua_pushnil(L);
    }
}

static Value ToJson(lua_State* L, int index)
{
    index = lua_absindex(L, index);
    switch (lua_type(L, index))
    {
    case LUA_TBOOLEAN:
        return Value(static_cast<bool>(lua_toboolean(L, index)));
    case LUA_TNUMBER:
        return lua_isinteger(L, index) ? Value(static_cast<long long int>(lua_tointeger(L, index))) : Value(static_cast<double>(lua_tonumber(L, index)));
    case LUA_TSTRING:
    {
        size_t size = 0;
        const char* data = lua_tolstring(L, index, &size);
        return Value(std::string(data, size));
    }
    case LUA_TTABLE:
    {
        // A table with a sequence part is an array, anything else (including {}) an object
        const size_t size = lua_rawlen(L, index);
        if (size > 0)
        {
            Value array = Json::Array();
            for (size_t i = 1; i <= size; ++i)
            {
                lua_rawgeti(L, index, static_cast<lua_Integer>(i));
                array.push_back(ToJson(L, -1));
                lua_pop(L, 1);
            }
            return array;
        }
        Value object = Json::Object();
        lua_pushnil(L);
        while (lua_next(L, index) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING)
            {
                object[lua_tostring(L, -2)] = ToJson(L, -1);
            }
            lua_pop(L, 1);
        }
        return object;
    }
    default:
        return Value();
    }
}

static Value CallLuaTool(const std::string& name, const Value& a, bool& found)
{
    {
        std::lock_guard<std::mutex> names_lock(lua_tool_names_mutex);
        if (lua_tool_names.count(name) == 0)
        {
            found = false;
            return Value();
        }
    }
    std::lock_guard<std::recursive_mutex> lock(lua_mutex);
    const auto tool = lua_tools.find(name);
    found = tool != lua_tools.end();
    if (!found)
    {
        return Value();
    }
    lua_rawgeti(lua, LUA_REGISTRYINDEX, tool->second);
    PushJson(lua, a);
    if (lua_pcall(lua, 1, 1, 0) != LUA_OK)
    {
        const char* message = lua_tostring(lua, -1);
        const std::string error = message != nullptr ? message : "lua error";
        lua_pop(lua, 1);
        throw std::runtime_error(error);
    }
    const Value result = ToJson(lua, -1);
    lua_pop(lua, 1);
    return result.is_null() ? Value("done") : result;
}

// lua_error() longjmps: the C++ work of each binding lives in a helper that returns normally,
// so no C++ object is alive (and no exception in flight) when the Lua error is raised

static bool CallFromLua(lua_State* L, const char* name)
{
    try
    {
        if (stopping)
        {
            throw std::runtime_error("the program is stopping");
        }
        PushJson(L, CallTool(name, lua_istable(L, 2) ? ToJson(L, 2) : Value(Json::Object())));
        return true;
    }
    catch (const std::exception& e)
    {
        lua_pushstring(L, e.what());
        return false;
    }
}

static int LuaCall(lua_State* L)
{
    const char* name = luaL_checkstring(L, 1);
    return CallFromLua(L, name) ? 1 : lua_error(L);
}

/// @brief Register a plugin tool. Expects its params table (or nil) on top of the stack
static bool RegisterLuaTool(lua_State* L, const char* name, const char* description, const int run_ref)
{
    for (size_t i = 0; i < tools.size(); ++i)
    {
        if (tools[i]["name"].get_string() == name)
        {
            return false;
        }
    }
    Value tool;
    tool["name"] = name;
    tool["description"] = description;
    tool["inputSchema"]["type"] = "object";
    tool["inputSchema"]["properties"] = Json::Object();
    if (lua_istable(L, -1))
    {
        lua_pushnil(L);
        while (lua_next(L, -2) != 0)
        {
            if (lua_type(L, -2) == LUA_TSTRING && lua_type(L, -1) == LUA_TSTRING)
            {
                tool["inputSchema"]["properties"][lua_tostring(L, -2)]["type"] = lua_tostring(L, -1);
            }
            lua_pop(L, 1);
        }
    }
    tools.push_back(tool);
    lua_tools[name] = run_ref;
    {
        std::lock_guard<std::mutex> names_lock(lua_tool_names_mutex);
        lua_tool_names.insert(name);
    }
    return true;
}

static int LuaTool(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TTABLE);
    lua_getfield(L, 1, "name");
    const char* name = luaL_checkstring(L, -1);
    lua_getfield(L, 1, "description");
    const char* description = luaL_optstring(L, -1, "");
    lua_getfield(L, 1, "run");
    luaL_checktype(L, -1, LUA_TFUNCTION);
    const int run_ref = luaL_ref(L, LUA_REGISTRYINDEX);
    lua_getfield(L, 1, "params");
    if (!RegisterLuaTool(L, name, description, run_ref))
    {
        return luaL_error(L, "a tool named %s already exists", name);
    }
    return 0;
}

static int LuaOnChat(lua_State* L)
{
    luaL_checktype(L, 1, LUA_TFUNCTION);
    lua_pushvalue(L, 1);
    lua_chat_handlers.push_back(luaL_ref(L, LUA_REGISTRYINDEX));
    return 0;
}

static int LuaEvery(lua_State* L)
{
    const lua_Integer seconds = luaL_checkinteger(L, 1);
    luaL_checktype(L, 2, LUA_TFUNCTION);
    lua_pushvalue(L, 2);
    const std::chrono::seconds interval(std::max<lua_Integer>(seconds, 1));
    lua_timers.push_back({ luaL_ref(L, LUA_REGISTRYINDEX), interval, std::chrono::steady_clock::now() + interval });
    return 0;
}

static int LuaLog(lua_State* L)
{
    LOG_INFO("[lua] " << luaL_checkstring(L, 1));
    return 0;
}

static void LoadPlugins(const std::string& directory)
{
    if (!std::filesystem::is_directory(directory))
    {
        return;
    }
    std::lock_guard<std::recursive_mutex> lock(lua_mutex);
    lua = luaL_newstate();
    luaL_openlibs(lua);
    static const luaL_Reg api[] = { { "call", LuaCall }, { "tool", LuaTool }, { "on_chat", LuaOnChat }, { "every", LuaEvery }, { "log", LuaLog }, { nullptr, nullptr } };
    luaL_newlib(lua, api);
    lua_setglobal(lua, "bot");

    std::vector<std::filesystem::path> files;
    for (const auto& entry : std::filesystem::directory_iterator(directory))
    {
        if (entry.path().extension() == ".lua")
        {
            files.push_back(entry.path());
        }
    }
    std::sort(files.begin(), files.end());
    for (const std::filesystem::path& file : files)
    {
        if (luaL_dofile(lua, file.c_str()) != LUA_OK)
        {
            LOG_ERROR("Plugin " << file.filename().string() << " failed: " << lua_tostring(lua, -1));
        }
        else
        {
            LOG_INFO("Loaded plugin " << file.filename().string());
        }
        lua_settop(lua, 0);
    }
}

// ---------------------------------------------------------------- MCP requests (stdio and HTTP)

/// @brief Answer one MCP JSON-RPC message. False if it needs no answer (notification, garbage)
static bool HandleRpc(const Value& request, Value& response);

/// @brief JSON-RPC lines as on stdin, on a unix socket. Each bot answers on its own, run/<name>.sock, every tool
/// call aimed at that bot; the main program on run/.program.sock (no player name has a dot), which also takes
/// {"method":"quit"}: the bots store everything and the program exits. The TUI (bot_tui) talks through them
static void ServeSocket(const std::string& path, const std::string& name)
{
    std::filesystem::create_directories("../run");
    sockaddr_un address{};
    address.sun_family = AF_UNIX;
    std::strncpy(address.sun_path, path.c_str(), sizeof(address.sun_path) - 1);
    // A socket file left by a run that ended badly
    unlink(path.c_str());
    const int server = socket(AF_UNIX, SOCK_STREAM, 0);
    if (bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(server, 8) != 0)
    {
        LOG_WARNING("Can't listen on " << path);
        close(server);
        return;
    }
    // ponytail: the threads live as long as the process; the socket file is removed by the next bot of that name
    std::thread([server, name, path]()
        {
            Logger::GetInstance().RegisterThread("socket " + (name.empty() ? std::string("program") : name));
            while (true)
            {
                const int fd = accept(server, nullptr, nullptr);
                if (fd < 0)
                {
                    if (errno == EINTR)
                    {
                        continue;
                    }
                    return;
                }
                std::thread([fd, name, path]()
                    {
                        std::string pending;
                        char buffer[65536];
                        ssize_t n;
                        while ((n = read(fd, buffer, sizeof(buffer))) > 0)
                        {
                            pending.append(buffer, n);
                            size_t end;
                            while ((end = pending.find('\n')) != std::string::npos)
                            {
                                const std::string line = pending.substr(0, end);
                                pending.erase(0, end + 1);
                                try
                                {
                                    Value request = Json::Parse(line);
                                    const std::string method = request.is_object() && request.contains("method") && request["method"].is_string() ? request["method"].get_string() : "";
                                    if (method == "tools/call" && !name.empty())
                                    {
                                        request["params"]["arguments"]["bot"] = name;
                                    }
                                    Value response;
                                    if (method == "quit" && name.empty())
                                    {
                                        quit_signal = 1;
                                        response["jsonrpc"] = "2.0";
                                        response["id"] = request.contains("id") ? request["id"] : Value();
                                        response["result"] = Json::Object();
                                    }
                                    if (response.is_object() || HandleRpc(request, response))
                                    {
                                        const std::string answer = response.Dump() + "\n";
                                        send(fd, answer.data(), answer.size(), MSG_NOSIGNAL);
                                    }
                                }
                                catch (const std::exception& e)
                                {
                                    LOG_WARNING("Socket " << path << ": " << e.what());
                                }
                            }
                        }
                        close(fd);
                    }).detach();
            }
        }).detach();
}

static bool HandleRpc(const Value& request, Value& response)
{
    // No id means notification: nothing to answer
    if (!request.is_object() || !request.contains("id") || !request.contains("method") || !request["method"].is_string())
    {
        return false;
    }
    const std::string method = request["method"].get_string();
    const Value params = request.contains("params") ? request["params"] : Value(Json::Object());

    response["jsonrpc"] = "2.0";
    response["id"] = request["id"];
    if (method == "initialize")
    {
        Value result;
        result["protocolVersion"] = params.is_object() && params.contains("protocolVersion") ? params["protocolVersion"] : Value("2024-11-05");
        result["capabilities"]["tools"] = Json::Object();
        result["serverInfo"]["name"] = "botcraft";
        result["serverInfo"]["version"] = "0.4.0";
        response["result"] = result;
    }
    else if (method == "ping")
    {
        response["result"] = Json::Object();
    }
    else if (method == "tools/list")
    {
        response["result"]["tools"] = tools;
    }
    else if (method == "tools/call")
    {
        Value content;
        content["type"] = "text";
        bool is_error = false;
        try
        {
            const Value arguments = params.is_object() && params.contains("arguments") ? params["arguments"] : Value(Json::Object());
            const Value result = CallTool(Str(params, "name"), arguments);
            if (result.is_object() && result.contains("__image"))
            {
                content["type"] = "image";
                content["data"] = result["__image"];
                content["mimeType"] = "image/png";
            }
            else
            {
                content["text"] = result.is_string() ? result.get_string() : result.Dump();
            }
        }
        catch (const std::exception& e)
        {
            content["text"] = std::string(e.what());
            is_error = true;
        }
        Value contents = Json::Array();
        contents.push_back(content);
        response["result"]["content"] = contents;
        response["result"]["isError"] = is_error;
    }
    else
    {
        response["error"]["code"] = -32601;
        response["error"]["message"] = "Method not found: " + method;
    }
    return true;
}

// ---------------------------------------------------------------- web GUI and HTTP MCP endpoint (--web PORT)

// The page (web/index.html) polls /state and drives the bots through /call, which runs any tool.
// Decision tree branches as shown in the GUI flow view: keep in sync with MakeAutopilotTree()
static const char* tree_json = R"JSON([
{"branch":"unstick","condition":"busy but not moving for 20 seconds?","action":"break a block beside (never above or below)","state":"unsticking"},
{"branch":"survive","condition":"hungry and has food?","action":"eat","state":"eating"},
{"branch":"defend","condition":"hostile within 10 blocks?","action":"fight with the best sword","state":"fighting"},
{"branch":"avenge","condition":"a player attacked one of the bots?","action":"go after that player","state":"avenging"},
{"branch":"assist","condition":"a bot nearby called for help against mobs, and this one is fit to fight?","action":"fight them together","state":"fighting with the others"},
{"branch":"recover","condition":"died less than 5 minutes ago and lost what it carried?","action":"go back for the items","state":"recovering items"},
{"branch":"salvage","condition":"a bot just died close by?","action":"pick up what it dropped","state":"picking up a teammate's items"},
{"branch":"sleep","condition":"night, and a bed to sleep in?","action":"sleep until morning","state":"sleeping"},
{"branch":"spacing","condition":"another bot closer than the minimum distance?","action":"step away","state":"spacing out"},
{"branch":"team","condition":"more than 16 blocks from the team leader?","action":"rejoin the leader","state":"following leader"},
{"branch":"give","condition":"teammate nearby asked for a block this bot has plenty of?","action":"bring half a stack over","state":"giving"},
{"branch":"store when full","condition":"chests on and inventory full?","action":"store in chests","state":"storing"},
{"branch":"orders","condition":"order from the team leader?","action":"go where the leader said","state":"following orders"},
{"branch":"hunt","condition":"team attack objective set?","action":"hunt one","state":"hunting"},
{"branch":"ready to build","condition":"team build objective set, and missing its tools or a chest?","action":"get ready to build","state":"getting ready to build"},
{"branch":"build","condition":"team build objective set?","action":"place one block","state":"building"},
{"branch":"make","condition":"asked to make something it does not have enough of yet?","action":"one step toward it (craft, smelt, or get what is missing)","state":"making"},
{"branch":"gather","condition":"resource goal set?","action":"collect one","state":"gathering"},
{"branch":"food","condition":"has no food?","action":"get food (hunt)","state":"getting food"},
{"branch":"out of the mine","condition":"underground with no pickaxe nor wood to make one?","action":"dig stairs up to the surface","state":"digging out of the mine"},
{"branch":"wood","condition":"has no wood?","action":"cut a tree","state":"getting wood"},
{"branch":"pickaxe","condition":"has no pickaxe?","action":"make pickaxe","state":"making pickaxe"},
{"branch":"axe","condition":"has no axe?","action":"make axe","state":"making axe"},
{"branch":"sword","condition":"has no sword?","action":"make sword","state":"making sword"},
{"branch":"shovel","condition":"has no shovel?","action":"make shovel","state":"making shovel"},
{"branch":"better tools","condition":"still missing stone tools or an iron pickaxe?","action":"next tool step","state":"gearing up"},
{"branch":"farm","condition":"ripe crops within 16 blocks?","action":"harvest and replant","state":"farming"},
{"branch":"store","condition":"chests on and something to store?","action":"store in chests","state":"storing"},
{"branch":"armor","condition":"still missing a bed, leather armor or a shield?","action":"next step (bed: wool from sheep, craft, place)","state":"getting armor"},
{"branch":"idle","condition":"nothing else to do","action":"wait","state":"idle"}
])JSON";

static std::string ReadFile(const std::string& path)
{
    std::ifstream file(path, std::ios::binary);
    return std::string((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
}

static void HandleHttpRequest(const int fd);

/// @brief One web request, on its own thread: whatever goes wrong in it stays in it
static void HandleHttp(const int fd)
{
    try
    {
        HandleHttpRequest(fd);
    }
    catch (const std::exception& e)
    {
        LOG_ERROR("Web request failed: " << e.what());
        close(fd);
    }
}

static void HandleHttpRequest(const int fd)
{
    // Read the headers, then the body announced by Content-Length
    std::string request;
    char buffer[4096];
    size_t header_end = std::string::npos;
    size_t body_size = 0;
    while (request.size() < 1 << 20)
    {
        if (header_end != std::string::npos && request.size() >= header_end + 4 + body_size)
        {
            break;
        }
        const ssize_t n = recv(fd, buffer, sizeof(buffer), 0);
        if (n <= 0)
        {
            break;
        }
        request.append(buffer, static_cast<size_t>(n));
        if (header_end == std::string::npos && (header_end = request.find("\r\n\r\n")) != std::string::npos)
        {
            std::string headers = request.substr(0, header_end);
            std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return std::tolower(c); });
            const size_t length = headers.find("content-length:");
            body_size = length == std::string::npos ? 0 : std::strtoul(headers.c_str() + length + 15, nullptr, 10);
        }
    }

    std::string status = "200 OK";
    std::string type = "application/json";
    std::string body;
    if (header_end == std::string::npos)
    {
        status = "400 Bad Request";
    }
    else if (request.rfind("GET / ", 0) == 0)
    {
        type = "text/html; charset=utf-8";
        body = ReadFile("../web/index.html");
    }
    else if (request.rfind("GET /snapshot.png", 0) == 0 && !snapshot_path.empty())
    {
        // /snapshot.png?bot=NAME: only the names of our own children select another file
        const size_t query = request.find("?bot=");
        const std::string wanted = query == std::string::npos || query > request.find('\r') ? "" :
            request.substr(query + 5, request.find_first_of(" &\r", query + 5) - query - 5);
        std::string child_name;
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            for (const std::shared_ptr<Child>& child : children)
            {
                if (child->name == wanted)
                {
                    child_name = wanted;
                }
            }
        }
        type = "image/png";
        body = ReadFile(SnapshotPathFor(child_name));
    }
    else if (request.rfind("GET /state ", 0) == 0)
    {
        Value state;
        state["tree"] = Json::Parse(tree_json);
        state["snapshot"] = !snapshot_path.empty();
        state["chests"] = auto_store;
        state["bots"] = Json::Array();
        std::vector<std::string> local;
        std::vector<std::shared_ptr<Child>> remote;
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            for (const BotSlot& slot : bots)
            {
                if (slot.bot->GetNetworkManager())
                {
                    local.push_back(slot.bot->GetNetworkManager()->GetMyName());
                }
            }
            remote = children;
        }
        // All at once: one by one, 40 bots took two minutes
        std::vector<std::future<Value>> answers;
        for (const std::string& name : local)
        {
            answers.push_back(std::async(std::launch::async, [name]()
                {
                    Value arguments;
                    arguments["bot"] = name;
                    try
                    {
                        return CallTool("status", arguments);
                    }
                    catch (const std::exception&)
                    {
                        Value dropped;
                        dropped["name"] = name;
                        dropped["state"] = "dropped by the server";
                        return dropped;
                    }
                }));
        }
        for (const std::shared_ptr<Child>& child : remote)
        {
            answers.push_back(std::async(std::launch::async, [child]() { return ChildStatus(*child); }));
        }
        for (std::future<Value>& answer : answers)
        {
            state["bots"].push_back(answer.get());
        }
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            for (const auto& [waiting, rejoin] : rejoining)
            {
                Value o;
                o["name"] = waiting;
                o["state"] = "waiting to join in " + std::to_string(std::max<long long>(0,
                    std::chrono::duration_cast<std::chrono::seconds>(rejoin.at - std::chrono::steady_clock::now()).count())) + " s";
                state["bots"].push_back(o);
            }
        }
        body = state.Dump();
    }
    // MCP over HTTP (streamable HTTP transport, plain JSON answers): POST one JSON-RPC message, get its response.
    // A browser page of another site would send its Origin: refuse those, only local programs may drive the bots
    else if (request.rfind("POST /mcp ", 0) == 0)
    {
        std::string headers = request.substr(0, header_end);
        std::transform(headers.begin(), headers.end(), headers.begin(), [](unsigned char c) { return std::tolower(c); });
        const size_t origin = headers.find("\r\norigin:");
        const bool local_origin = origin == std::string::npos || headers.find("//127.0.0.1", origin) < headers.find("\r\n", origin + 2) ||
            headers.find("//localhost", origin) < headers.find("\r\n", origin + 2);
        Value response;
        if (!local_origin)
        {
            status = "403 Forbidden";
        }
        else
        {
            try
            {
                if (HandleRpc(Json::Parse(request.substr(header_end + 4)), response))
                {
                    body = response.Dump();
                }
                else
                {
                    status = "202 Accepted";
                }
            }
            catch (const std::exception&)
            {
                status = "400 Bad Request";
            }
        }
    }
    // No server-initiated stream: MCP clients expect 405 when they probe for one
    else if (request.rfind("GET /mcp", 0) == 0 || request.rfind("DELETE /mcp", 0) == 0)
    {
        status = "405 Method Not Allowed";
    }
    // The custom header can't be sent by another website's page without a CORS preflight we never grant,
    // so only our own page can drive the bots
    else if (request.rfind("POST /call ", 0) == 0 && request.substr(0, header_end).find("X-Bot:") != std::string::npos)
    {
        Value reply;
        try
        {
            const Value call = Json::Parse(request.substr(header_end + 4));
            reply["result"] = CallTool(Str(call, "name"), call.contains("arguments") ? call["arguments"] : Value(Json::Object()));
            reply["ok"] = true;
        }
        catch (const std::exception& e)
        {
            reply["result"] = std::string(e.what());
            reply["ok"] = false;
        }
        body = reply.Dump();
    }
    else
    {
        status = "404 Not Found";
    }
    const std::string response = "HTTP/1.1 " + status + "\r\nContent-Type: " + type + "\r\nContent-Length: " + std::to_string(body.size()) +
        "\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n" + body;
    send(fd, response.data(), response.size(), MSG_NOSIGNAL);
    close(fd);
}

static void ServeWeb(const std::string& bind_address, const int port, const std::atomic<bool>& stop)
{
    const int server = socket(AF_INET, SOCK_STREAM, 0);
    const int yes = 1;
    setsockopt(server, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes));
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_port = htons(static_cast<uint16_t>(port));
    // Local machine only unless --bind says otherwise: /call can make the bots do anything
    if (inet_pton(AF_INET, bind_address.c_str(), &address.sin_addr) != 1 || bind(server, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 || listen(server, 16) != 0)
    {
        LOG_ERROR("Web GUI: can't listen on " << bind_address << ":" << port);
        close(server);
        return;
    }
    LOG_INFO("Control panel on http://" << bind_address << ":" << port << ", MCP endpoint on http://" << bind_address << ":" << port << "/mcp");
    while (!stop)
    {
        pollfd waiting{ server, POLLIN, 0 };
        if (poll(&waiting, 1, 200) > 0)
        {
            const int client = accept(server, nullptr, nullptr);
            if (client >= 0)
            {
                // A tool call can take minutes: one thread per request keeps the page responsive
                std::thread(HandleHttp, client).detach();
            }
        }
    }
    close(server);
}

// ---------------------------------------------------------------- main: options, bots, MCP stdio loop

static void Usage()
{
    std::cerr << "Usage: bot_mcp [options]\n"
        << "  --address HOST:PORT  server to join (default 192.168.100.78:25565)\n"
        << "  --login NAME         offline player name, \"\" for Microsoft account auth (default Bot)\n"
        << "  --bots N             spawn N bots, named NAME1..NAMEN (default 1)\n"
        << "  --delay M            seconds to wait between two bot spawns (default 5)\n"
        << "  --autopilot          start with the decision tree on\n"
        << "  --chests             store collected and harvested resources in chests, one chest per resource\n"
        << "  --spacing N          blocks the bots keep between each other on autopilot, 0 for none (default 3)\n"
        << "  --no-sleep-rule      don't set the server's sleeping rule so one sleeping bot skips the night\n"
        << "  --web PORT           control panel on http://127.0.0.1:PORT (states, decision flow, controls, views)\n"
        << "                       and MCP over HTTP on http://127.0.0.1:PORT/mcp\n"
        << "  --gui                open a window per bot with its view and (key B) its live behaviour tree\n"
        << "  --snapshot FILE      save each bot's view every 5 seconds: FILE (png) for the first bot,\n"
        << "                       FILE.NAME.png for the others\n"
        << "  --bind ADDRESS       address the control panel listens on (default 127.0.0.1, 0.0.0.0 for all)\n"
        << "  --plugins DIR        directory of Lua plugins (default: plugins/ next to the sources)\n"
        << "  --headless           no window, no views, no MCP on stdin (use --web, bin/bot_tui or tui.py);\n"
        << "                       runs until Ctrl-C or SIGTERM. Without a display this is the default for views\n"
        << "SIGHUP, SIGUSR1 or SIGUSR2 (any mode): the bots store everything in chests, disconnect, and it exits.\n"
        << "Then speaks MCP (JSON-RPC) on stdin/stdout. When stdin is closed (headless: on Ctrl-C/SIGTERM),\n"
        << "the bots store their inventory in chests and disconnect." << std::endl;
}

int main(int argc, char* argv[])
{
    // stdout is the MCP channel: keep a private copy of it and point fd 1 at stderr,
    // so nothing the library or a plugin prints can corrupt the protocol
    FILE* out = fdopen(dup(STDOUT_FILENO), "w");
    dup2(STDERR_FILENO, STDOUT_FILENO);

    Logger::GetInstance().SetLogLevel(LogLevel::Info);
    Logger::GetInstance().SetFilename("");
    Logger::GetInstance().RegisterThread("main");

    try
    {
        std::string login = "Bot";
        std::string plugins = "../plugins";
        int count = 1;
        bool chosen = false;        // --bots or --login: those, not the saved list
        int delay = 5;
        int web_port = 0;
        std::string web_bind = "127.0.0.1";
        for (int i = 1; i < argc; ++i)
        {
            const std::string arg = argv[i];
            const auto value = [&]() -> std::string
                {
                    if (i + 1 >= argc)
                    {
                        throw std::runtime_error(arg + " requires a value");
                    }
                    return argv[++i];
                };
            if (arg == "--address") { server_address = value(); }
            else if (arg == "--login") { login = value(); chosen = true; }
            else if (arg == "--bots") { count = std::clamp(std::stoi(value()), 1, 100); chosen = true; }
            else if (arg == "--delay") { delay = std::max(std::stoi(value()), 0); }
            else if (arg == "--autopilot") { start_autopilot = true; }
            else if (arg == "--chests") { auto_store = true; }
            else if (arg == "--spacing") { min_spacing = std::max(std::stod(value()), 0.0); }
            else if (arg == "--no-sleep-rule") { set_sleep_rule = false; }
            else if (arg == "--web") { web_port = std::stoi(value()); }
            else if (arg == "--bind") { web_bind = value(); }
            else if (arg == "--gui") { show_gui = true; }
            else if (arg == "--headless") { headless = true; }
            else if (arg == "--child") { is_child = true; }
            // Paths are given relative to the caller's directory, resolve them before we leave it
            else if (arg == "--snapshot") { snapshot_path = std::filesystem::absolute(value()).string(); }
            else if (arg == "--plugins") { plugins = std::filesystem::absolute(value()).string(); }
            else
            {
                Usage();
                return arg == "--help" || arg == "-h" ? 0 : 2;
            }
        }
        // Botcraft loads ./Assets from the working directory, they are installed next to the executable
        self_path = std::filesystem::canonical("/proc/self/exe").string();
        std::filesystem::current_path(std::filesystem::path(self_path).parent_path());
        // A bot process ends with the main program, however that one ends (closed, killed, crashed): it watches
        // for its parent to change. (The kernel's parent-death signal is no use here: it fires when the thread
        // that started the child ends, and those are short-lived.)
        if (is_child)
        {
            // Ctrl-C and a hangup reach the whole process group: the main program decides, and tells each bot
            // to store its things and leave. Dying on the spot here would leave them on the ground
            for (const int sig : { SIGHUP, SIGINT, SIGUSR1, SIGUSR2 })
            {
                std::signal(sig, SIG_IGN);
            }
            const pid_t parent = getppid();
            std::thread([parent]()
                {
                    while (getppid() == parent)
                    {
                        std::this_thread::sleep_for(std::chrono::seconds(1));
                    }
                    _exit(0);
                }).detach();
        }

        tools = Json::Parse(tools_json);
        LoadChests();
        LoadRecipes("../data/data/minecraft");

        std::atomic<bool> stop = false;
        const auto nap = [&](const int seconds)
            {
                for (int i = 0; i < seconds * 10 && !stop; ++i)
                {
                    Utilities::SleepFor(std::chrono::milliseconds(100));
                }
            };

        // A dead child must show as an error on its pipe, not kill us
        std::signal(SIGPIPE, SIG_IGN);
        // The renderer gives the bot of this process its view (window and snapshots)
        // ponytail: one process per bot costs a copy of the game assets each; fine for a handful of bots
        if (headless || (getenv("DISPLAY") == nullptr && getenv("WAYLAND_DISPLAY") == nullptr && !is_child))
        {
            // Nothing to draw on (or not wanted): no window and no views, the bots play just the same
            if (!headless && (show_gui || !snapshot_path.empty()))
            {
                LOG_WARNING("No display: running without the window and the views");
            }
            show_gui = false;
            snapshot_path.clear();
        }
        if (show_gui || !snapshot_path.empty())
        {
            Renderer::Settings::Enable();
            Renderer::Settings::Headless(!show_gui);
            Renderer::Settings::SetResolution(854, 480);
        }
        // The game assets, built here. Every block's model has faces (~700 MB per process) needed only to draw: a
        // process without a window or views keeps none (and blocks with the same colliders share a model), ~60 MB
        // instead of ~840. What loading freed goes back to the system
        Blockstate::keep_render_data = show_gui || !snapshot_path.empty();
        if (!is_child && !Blockstate::keep_render_data)
        {
            shared_world = std::make_shared<World>(true);
        }
        Botcraft::AssetsManager::getInstance();
        malloc_trim(0);
        spawn_delay_seconds = delay;
        // The bots of last time (bot/bots.txt), unless the command line says which
        std::vector<std::string> names;
        if (!is_child && !chosen) { names = LoadRoster(); }
        if (names.empty())
        {
            for (int i = 1; i <= count; ++i) { names.push_back(count > 1 && !login.empty() ? login + std::to_string(i) : login); }
            if (!is_child) { SaveRoster(names); }
        }
        {
            std::lock_guard<std::mutex> lock(bots_mutex);
            for (size_t i = 1; i < names.size(); ++i)
            {
                Rejoin queued;
                queued.at = std::chrono::steady_clock::now() + std::chrono::seconds(std::max(delay, 5) * static_cast<int>(i));
                queued.restore["bot"] = names[i];
                queued.restore["enabled"] = start_autopilot;
                rejoining[names[i]] = queued;
            }
        }
        SpawnBot(names[0]);

        std::thread snapshots([&]()
            {
                while (!snapshot_path.empty() && !stop)
                {
                    Renderer::Settings::TakeScreenshot(snapshot_path);
                    nap(5);
                }
            });

        if (!is_child)
        {
            LoadPlugins(plugins);
        }
        // Every tool but the ones about the bot list itself can target a given bot
        for (size_t i = 0; i < tools.size(); ++i)
        {
            const std::string& name = tools[i]["name"].get_string();
            if (name != "bots" && name != "connect" && name != "spawn" && name != "vault" && name != "mines")
            {
                tools[i]["inputSchema"]["properties"]["bot"]["type"] = "string";
                tools[i]["inputSchema"]["properties"]["bot"]["description"] = "Name of the bot to use, see the bots tool. Defaults to the first bot.";
            }
        }
        // Feeds the chat to the plugins
        std::thread chat_thread([&]()
            {
                Logger::GetInstance().RegisterThread("lua chat");
                while ((!lua_chat_handlers.empty() || !lua_timers.empty()) && !stop)
                {
                    // The timers that are due
                    for (LuaTimer& timer : lua_timers)
                    {
                        if (std::chrono::steady_clock::now() < timer.next)
                        {
                            continue;
                        }
                        timer.next = std::chrono::steady_clock::now() + timer.interval;
                        std::lock_guard<std::recursive_mutex> lock(lua_mutex);
                        lua_rawgeti(lua, LUA_REGISTRYINDEX, timer.function);
                        if (lua_pcall(lua, 0, 0, 0) != LUA_OK)
                        {
                            LOG_ERROR("Lua timer failed: " << lua_tostring(lua, -1));
                            lua_pop(lua, 1);
                        }
                    }
                    std::string line;
                    {
                        std::unique_lock<std::mutex> lock(chat_events_mutex);
                        if (!chat_events_cond.wait_for(lock, std::chrono::milliseconds(200), []() { return !chat_events.empty(); }))
                        {
                            continue;
                        }
                        line = chat_events.front();
                        chat_events.pop_front();
                    }
                    std::lock_guard<std::recursive_mutex> lock(lua_mutex);
                    for (const int handler : lua_chat_handlers)
                    {
                        lua_rawgeti(lua, LUA_REGISTRYINDEX, handler);
                        lua_pushlstring(lua, line.data(), line.size());
                        if (lua_pcall(lua, 1, 0, 0) != LUA_OK)
                        {
                            LOG_ERROR("Lua chat handler failed: " << lua_tostring(lua, -1));
                            lua_pop(lua, 1);
                        }
                    }
                }
            });
        if (!is_child)
        {
            ServeSocket("../run/.program.sock", "");
        }
        std::thread web([&]()
            {
                if (web_port > 0 && !is_child)
                {
                    Logger::GetInstance().RegisterThread("web");
                    ServeWeb(web_bind, web_port, stop);
                }
            });

        // Tells the team where its leader is, for the bots that can't see it any more
        std::thread team_feed([&]()
            {
                Logger::GetInstance().RegisterThread("team");
                while (!is_child && !stop)
                {
                    nap(3);
                    std::string leader;
                    std::vector<std::string> followers;
                    {
                        std::lock_guard<std::mutex> lock(bots_mutex);
                        leader = team_leader;
                        for (const BotSlot& slot : bots)
                        {
                            followers.push_back(slot.bot->GetNetworkManager()->GetMyName());
                        }
                        for (const std::shared_ptr<Child>& child : children)
                        {
                            followers.push_back(child->name);
                        }
                    }
                    if (leader.empty() || stop)
                    {
                        continue;
                    }
                    try
                    {
                        Value target;
                        target["bot"] = leader;
                        const Value position = CallTool("status", target)["position"];
                        for (const std::string& follower : followers)
                        {
                            if (follower != leader)
                            {
                                Value update;
                                update["bot"] = follower;
                                update["leader_x"] = position["x"];
                                update["leader_y"] = position["y"];
                                update["leader_z"] = position["z"];
                                CallTool("team_member", update);
                            }
                        }
                    }
                    catch (const std::exception&)
                    {
                        // A bot is gone or busy leaving: try again next round
                    }
                }
            });

        // A bot that was kicked, or whose process died, joins again 60 seconds later with the same autopilot settings
        if (!is_child)
        {
            // The jobs given before the restart: bots with a job of their own stay out of the team, the
            // feeders feed again once they are all back
            for (const std::string& n : LoadRole("solo"))
            {
                std::lock_guard<std::mutex> lock(bots_mutex);
                solo_bots.insert(n);
            }
            for (const std::string& n : LoadRole("leader"))
            {
                std::lock_guard<std::mutex> lock(bots_mutex);
                elected_leader = n;
            }
            const std::vector<std::string> saved_feeders = LoadRole("feeder");
            if (!saved_feeders.empty())
            {
                std::thread([saved_feeders]()
                    {
                        for (int i = 0; i < 300; ++i)
                        {
                            std::this_thread::sleep_for(std::chrono::seconds(1));
                            const std::string listed = CallTool("bots", Value(Json::Object())).Dump();
                            if (std::all_of(saved_feeders.begin(), saved_feeders.end(), [&](const std::string& f) { return listed.find("\"" + f + "\"") != std::string::npos; }))
                            {
                                break;
                            }
                        }
                        Value args;
                        args["bots"] = Json::Array();
                        for (const std::string& f : saved_feeders)
                        {
                            args["bots"].push_back(f);
                        }
                        try
                        {
                            CallTool("feeders", args);
                            LOG_INFO("The feeders are back at it");
                        }
                        catch (const std::exception& e)
                        {
                            LOG_WARNING("Feeders: " << e.what());
                        }
                    }).detach();
            }
        }
        std::thread rejoin_thread([&]()
            {
                Logger::GetInstance().RegisterThread("rejoin");
                while (!stop)
                {
                    nap(2);
                    if (stop)
                    {
                        break;
                    }
                    const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
                    // A bot of this process dropped by the server (one a turn, every 2 seconds): take it down
                    BotSlot dropped;
                    std::vector<std::shared_ptr<Child>> dead;
                    {
                        std::lock_guard<std::mutex> lock(bots_mutex);
                        const auto gone = std::find_if(bots.begin(), bots.end(), [](const BotSlot& slot) { return slot.bot->GetShouldBeClosed(); });
                        if (gone != bots.end())
                        {
                            dropped = std::move(*gone);
                            bots.erase(gone);
                            NewChatSource();
                        }
                        // The bots whose process ended without being asked to
                        for (auto it = children.begin(); it != children.end();)
                        {
                            if (waitpid((*it)->pid, nullptr, WNOHANG) != 0)
                            {
                                dead.push_back(*it);
                                it = children.erase(it);
                            }
                            else
                            {
                                ++it;
                            }
                        }
                    }
                    if (dropped.bot)
                    {
                        const std::string name = dropped.bot->GetNetworkManager()->GetMyName();
                        Rejoin rejoin;
                        rejoin.at = now + rejoin_delay;
                        rejoin.restore["bot"] = name;
                        rejoin.restore["enabled"] = dropped.bot->autopilot.load();
                        if (dropped.bot->GetGoal().second > 0)
                        {
                            rejoin.restore["gather"] = dropped.bot->GetGoal().first;
                            rejoin.restore["count"] = dropped.bot->GetGoal().second;
                        }
                        LeaveGame(dropped);
                        // Frees its renderer: there can be only one per process
                        dropped.bot.reset();
                        LOG_WARNING(name << " was dropped by the server, joining again in 60 seconds");
                        std::lock_guard<std::mutex> lock(bots_mutex);
                        rejoining[name] = rejoin;
                    }
                    for (const std::shared_ptr<Child>& child : dead)
                    {
                        Rejoin rejoin;
                        rejoin.at = now + rejoin_delay;
                        rejoin.restore["bot"] = child->name;
                        const Value last = child->GetLastStatus();
                        rejoin.restore["enabled"] = last.is_object() && last.contains("autopilot") && last["autopilot"].is_bool() ? last["autopilot"].get<bool>() : start_autopilot;
                        if (last.is_object() && last.contains("gather_left") && last["gather_left"].get_number<int>() > 0)
                        {
                            rejoin.restore["gather"] = last["gather"];
                            rejoin.restore["count"] = last["gather_left"];
                        }
                        {
                            std::lock_guard<std::mutex> lock(child->mutex);
                            fclose(child->to);
                            fclose(child->from);
                            child->to = child->from = nullptr;
                        }
                        LOG_WARNING("The process of " << child->name << " ended, starting it again in 60 seconds");
                        std::lock_guard<std::mutex> lock(bots_mutex);
                        rejoining[child->name] = rejoin;
                    }

                    // Whoever has waited long enough joins again
                    std::string name;
                    Value restore;
                    {
                        std::lock_guard<std::mutex> lock(bots_mutex);
                        for (const auto& [waiting, rejoin] : rejoining)
                        {
                            if (rejoin.at <= now && (name.empty() || rejoin.at < rejoining.at(name).at))
                            {
                                name = waiting;
                                restore = rejoin.restore;
                            }
                        }
                    }
                    if (name.empty())
                    {
                        continue;
                    }
                    try
                    {
                        SpawnBot(name);
                        {
                            std::lock_guard<std::mutex> lock(bots_mutex);
                            rejoining.erase(name);
                        }
                        CallTool("autopilot", restore);
                        LOG_INFO(name << " joined again");
                    }
                    catch (const std::exception& e)
                    {
                        LOG_WARNING(name << " could not join again, next try in 60 seconds: " << e.what());
                        std::lock_guard<std::mutex> lock(bots_mutex);
                        const auto it = rejoining.find(name);
                        if (it != rejoining.end())
                        {
                            it->second.at = std::chrono::steady_clock::now() + rejoin_delay;
                        }
                    }
                    // One join at a time, spaced like the first ones: the server turns away a second join from
                    // the same address within 4 seconds, and after a server restart every bot was waiting
                    nap(std::max(delay, 5));
                }
            });

        // SIGHUP, SIGUSR1, SIGUSR2 (and headless, Ctrl-C and SIGTERM too) end it the same way as closing
        // stdin: every bot stores everything in the chests, disconnects, then the program exits
        for (const int sig : { SIGHUP, SIGUSR1, SIGUSR2 })
        {
            std::signal(sig, QuitSignal);
        }
        if (headless)
        {
            std::signal(SIGINT, QuitSignal);
            std::signal(SIGTERM, QuitSignal);
        }
        // stdin read by hand, a poll at a time: a signal is seen within 200 ms, whatever thread it lands on
        std::string line, pending;
        const auto next_line = [&]()
            {
                while (!quit_signal)
                {
                    const size_t end = pending.find('\n');
                    if (end != std::string::npos)
                    {
                        line = pending.substr(0, end);
                        pending.erase(0, end + 1);
                        return true;
                    }
                    if (headless)
                    {
                        std::this_thread::sleep_for(std::chrono::milliseconds(200));
                        continue;
                    }
                    pollfd input{ STDIN_FILENO, POLLIN, 0 };
                    if (poll(&input, 1, 200) <= 0)
                    {
                        continue;
                    }
                    char buffer[65536];
                    const ssize_t n = read(STDIN_FILENO, buffer, sizeof(buffer));
                    if (n <= 0)
                    {
                        // Closed
                        return false;
                    }
                    pending.append(buffer, n);
                }
                return false;
            };
        while (next_line())
        {
            Value request;
            try
            {
                request = Json::Parse(line);
            }
            catch (const std::exception&)
            {
                continue;
            }
            Value response;
            if (!HandleRpc(request, response))
            {
                continue;
            }
            fprintf(out, "%s\n", response.Dump().c_str());
            fflush(out);
        }

        if (quit_signal)
        {
            LOG_INFO("Stopping on a signal: the bots store their items and leave");
        }
        // stdin closed: stop spawning, then every bot stores its inventory and leaves. The plugins' thread and the
        // team feed may be in the middle of long calls to the bots: joined once the bots are gone
        // (waiting for them first, a stop with 50 bots never got as far as telling one to leave)
        stop = true;
        stopping = true;
        snapshots.join();
        web.join();
        rejoin_thread.join();
        // All the bots at once: each walks to its chests, one after the other took minutes per bot
        {
            std::vector<std::shared_ptr<Child>> leaving;
            {
                std::lock_guard<std::mutex> lock(bots_mutex);
                leaving = children;
            }
            std::vector<std::thread> goodbyes;
            for (const std::shared_ptr<Child>& child : leaving)
            {
                goodbyes.emplace_back([child]()
                    {
                        try
                        {
                            LOG_INFO("Leaving: " << DisconnectChild(child).Dump());
                        }
                        catch (const std::exception& e)
                        {
                            LOG_WARNING("Leaving: " << e.what());
                        }
                    });
            }
            for (std::thread& t : goodbyes)
            {
                t.join();
            }
        }
        chat_thread.join();
        team_feed.join();
        while (true)
        {
            std::shared_ptr<McpBot> bot;
            {
                std::lock_guard<std::mutex> lock(bots_mutex);
                if (bots.empty())
                {
                    break;
                }
                bot = bots.front().bot;
            }
            try
            {
                LOG_INFO("Leaving: " << DisconnectBot(bot).Dump());
            }
            catch (const std::exception& e)
            {
                LOG_WARNING("Leaving: " << e.what());
            }
        }
    }
    catch (const std::exception& e)
    {
        LOG_FATAL("Exception: " << e.what());
        return 1;
    }
    return 0;
}
