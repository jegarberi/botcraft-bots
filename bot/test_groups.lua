-- Offline check of the groups: lua5.4 test_groups.lua plugins/groups.lua
local tool, calls = nil, {}
bot = { call = function(name, a)
    calls[#calls + 1] = { name = name, args = a }
    if name == "bots" then local l = {} for i = 1, 15 do l[i] = { name = "Bot" .. i } end return l end
    if name == "feeders" then return a and a.bots and {} or { { bot = "Bot1" }, { bot = "Bot5" }, { bot = "Bot12" } } end
    if name == "berries" then return { bot = "Bot15" } end
    return {}
end, tool = function(t) tool = t end, every = function() end, on_chat = function() end, log = print }
load(io.open(arg[1]):read("a"), "groups")()
local g = tool.run({})
local function same(a, b) return table.concat(a, ",") == table.concat(b, ",") end
assert(same(g.hunters, { "Bot1", "Bot5", "Bot12" }), "the feeders hunt")
assert(same(g.farmers, { "Bot15", "Bot2", "Bot3" }), "the berry picker farms")
assert(same(g.breeders, { "Bot4", "Bot6", "Bot7" }) and same(g.miners, { "Bot8", "Bot9", "Bot10" }) and same(g.builders, { "Bot11", "Bot13", "Bot14" }))
local miners_team = 0
for _, c in ipairs(calls) do
    if c.name == "team_member" and c.args.objective == "gather" then
        miners_team = miners_team + 1
        assert(c.args.leader == "Bot8" and c.args.solo and #c.args.members == 3)
    end
end
assert(miners_team == 3, "the three miners are one team")
assert(io.open("../groups_job.lua"), "kept for a restart")
tool.run({ stop = true })
assert(not io.open("../groups_job.lua"))
print("groups plugin OK")
