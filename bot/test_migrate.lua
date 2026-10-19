-- Offline check of migration: lua5.4 test_migrate.lua plugins/migrate.lua (from an empty temporary directory)
-- Five bots in a bare place; wood and animals lie 200+ blocks east, where they remember logs. They vote to move,
-- go east and settle there; the town moves, the trades are given again. A rich place votes no.
math.randomseed(1)
os.remove("../migrate_job.lua")
local clock = 1000
os.time = function() return clock end
local pos, calls, tool, ticks, chats = {}, {}, nil, {}, {}
for i = 1, 5 do pos["Bot" .. i] = { x = i, y = 64, z = 0 } end
local rich_from = 200
local function rich(n) return pos[n].x >= rich_from end
local function list(k) local l = {} for i = 1, k do l[i] = { x = 0, y = 0, z = 0 } end return l end
bot = { call = function(name, a)
    calls[name] = calls[name] or {}
    table.insert(calls[name], a)
    if name == "bots" then local l = {} for i = 1, 5 do l[i] = { name = "Bot" .. i } end return l end
    if name == "status" then return { name = a.bot, food = 18, position = { x = pos[a.bot].x, y = 64, z = pos[a.bot].z } } end
    if name == "find_blocks" then return list(rich(a.bot) and 10 or (a.block == "_log" and 2 or 0)) end
    if name == "entities" then return list(rich(a.bot) and 4 or 0) end
    if name == "recall" then return a.kind == "_log" and { { x = 300, y = 64, z = 5 } } or {} end
    if name == "leader" then return "Bot1" end
    if name == "travel" then
        pos[a.bot] = { x = a.x, y = 64, z = a.z }
        for n in pairs(pos) do if n ~= a.bot then pos[n] = { x = a.x - 2, y = 64, z = a.z } end end    -- the followers keep up
        return "arrived"
    end
    if name == "groups" and a.show then return { miners = { "Bot2: mining" }, builders = { "Bot3" } } end
    if name == "berries" then return { bot = "Bot5" } end
    if name == "castle" then return { building = false } end
    if name == "chat" then chats[#chats + 1] = a.message end
    return "ok"
end, tool = function(t) tool = t end, every = function(_, f) ticks[#ticks + 1] = f end, log = print }
load(io.open(arg[1]):read("a"), "migrate")()
local function tick() for _, f in ipairs(ticks) do f() end end

clock = clock + 1300; tick()                                  -- the first look: poor, a vote
local st = tool.run({})
assert(st.last_vote and st.last_vote.passed and st.last_vote.yes == 5 and st.last_vote.need == 3, "all five vote yes, three needed")
assert(st.migrating and st.trip.direction == "east", "they go where they remember wood: " .. tostring(st.trip and st.trip.direction))
assert(MIGRATING, "the other plugins pause")
for i = 1, 10 do clock = clock + 30; tick(); if not MIGRATING then break end end
assert(not MIGRATING, "they settled")
assert(#calls.home == 5 and calls.home[1].forget, "every bot forgot its bed")
local follow = 0
for _, c in ipairs(calls.team_member) do if c.objective == "follow" and c.leader == "Bot1" then follow = follow + 1 end end
assert(follow == 5, "everyone followed the leader")
local town = calls.empire[#calls.empire].town
assert(town and town.x >= rich_from + 144, "the town moved to the new place")
local assign = calls.groups[#calls.groups].assign
assert(assign and assign.miners[1] == "Bot2" and assign.builders[1] == "Bot3", "the trades are given again, notes stripped")
assert(calls.berries[#calls.berries].bot == "Bot5", "the berry picker looks for a patch there")
assert(chats[#chats]:match("settle"), "announced")

-- Right after settling: no new proposal for an hour; then a rich place keeps them
clock = clock + 1300; tick()
assert(tool.run({}).migrating == false and #chats == 2, "resting after a move")
clock = clock + 3600; tick()
assert(tool.run({}).last_look.logs >= 8 and not MIGRATING, "a good place: no vote")

-- A place poor for only two of five: proposed by hand, voted down
pos.Bot1, pos.Bot2 = { x = 0, y = 64, z = 0 }, { x = 0, y = 64, z = 0 }
local v = tool.run({ vote = true }).last_vote
assert(v.yes == 2 and not v.passed and not MIGRATING, "2 of 5 is not half + 1")
os.remove("../migrate_job.lua")
print("migrate plugin OK: voted 5-0 to go east, settled " .. (town.x - 144) .. " blocks east; a 2-3 vote stays")
