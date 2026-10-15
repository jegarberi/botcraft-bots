-- Offline check of the groups: lua5.4 test_groups.lua plugins/groups.lua
local tool, calls, ticks = nil, {}, {}
bot = { call = function(name, a)
    calls[#calls + 1] = { name = name, args = a }
    if name == "bots" then local l = {} for i = 1, 15 do l[i] = { name = "Bot" .. i } end return l end
    if name == "berries" then return { bot = "Bot15" } end
    if name == "roles" then return { action = "harvest", note = "ok" } end
    return {}
end, tool = function(t) tool = t end, every = function(_, f) ticks[#ticks + 1] = f end, on_chat = function() end, log = print }
load(io.open(arg[1]):read("a"), "groups")()
local g = tool.run({})
local function same(a, b) return table.concat(a, ",") == table.concat(b, ",") end
assert(same(g.farmers, { "Bot15", "Bot3" }), "the berry picker farms")
assert(same(g.cooks, { "Bot1", "Bot2" }) and same(g.miners, { "Bot6", "Bot7" }) and same(g.explorers, { "Bot12", "Bot13" }))
assert(same(g.builders, { "Bot14" }), "the rest build")
local miners_team = 0
for _, c in ipairs(calls) do
    if c.name == "team_member" and c.args.objective == "gather" then
        miners_team = miners_team + 1
        assert(c.args.leader == "Bot6" and c.args.solo and #c.args.members == 2)
    end
end
assert(miners_team == 2, "the miners are one team")
-- Each turn goes to the next bot, through its trade's network
calls = {}
for _ = 1, 15 do for _, f in ipairs(ticks) do f() end end
local turns = {}
for _, c in ipairs(calls) do if c.name == "roles" then turns[c.args.act] = c.args.role end end
assert(turns.Bot2 == "cooks" and turns.Bot14 == "builders" and turns.Bot13 == "explorers", "every bot gets a turn in its trade")
assert(io.open("../groups_job.lua"), "kept for a restart")
tool.run({ stop = true })
assert(not io.open("../groups_job.lua"))
print("groups plugin OK")
