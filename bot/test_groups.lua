-- Offline check of the groups: lua5.4 test_groups.lua plugins/groups.lua
local tool, calls, ticks, busy = nil, {}, {}, {}
local clock = 1000
os.time = function() return clock end
bot = { call = function(name, a)
    calls[#calls + 1] = { name = name, args = a }
    if name == "bots" then local l = {} for i = 1, 15 do l[i] = { name = "Bot" .. i } end return l end
    if name == "berries" then return { bot = "Bot15" } end
    if name == "roles" then return { action = "harvest", note = "ok" } end
    return {}
end, spawn = function(key, f) if busy[key] then return false end f() return true end, busy = function(key) return busy[key] == true end,
tool = function(t) tool = t end, every = function(_, f) ticks[#ticks + 1] = f end, on_chat = function() end, log = print }
load(io.open(arg[1]):read("a"), "groups")()
local g = tool.run({})
local function same(a, b) return table.concat(a, ",") == table.concat(b, ",") end
assert(same(g.farmers, { "Bot15", "Bot3" }), "the berry picker farms")
assert(same(g.bakers, { "Bot1", "Bot2" }) and same(g.miners, { "Bot6", "Bot7" }) and same(g.explorers, { "Bot12", "Bot13" }))
assert(same(g.lumberjacks, { "Bot14" }), "wood has a dedicated trade")
local miners_team = 0
for _, c in ipairs(calls) do
    if c.name == "team_member" and c.args.objective == "gather" then
        miners_team = miners_team + 1
        assert(c.args.leader == "Bot6" and c.args.solo and #c.args.members == 2)
    end
end
assert(miners_team == 2, "the miners are one team")
-- Every bot a turn within the minute, in its trade, through its network; then not again before a minute
local turns = {}
ROLES_ACT = function(name, role) turns[name] = (turns[name] or 0) + 1; assert(g[role]) return { action = "harvest", note = "ok" } end
busy.Bot5 = true
for _ = 1, 12 do clock = clock + 5; for _, f in ipairs(ticks) do f() end end
for i = 1, 15 do
    if i ~= 5 then assert(turns["Bot" .. i] == 1, "Bot" .. i .. " had " .. tostring(turns["Bot" .. i]) .. " turns in its first minute") end
end
assert(not turns.Bot5, "a bot still on its last action waits")
busy.Bot5 = nil
for _ = 1, 12 do clock = clock + 5; for _, f in ipairs(ticks) do f() end end
assert(turns.Bot5 == 1 and turns.Bot1 == 2, "the next minute: everyone again")
assert(io.open("../groups_job.lua"), "kept for a restart")
tool.run({ stop = true })
assert(not io.open("../groups_job.lua"))
print("groups plugin OK")
