-- Offline check of the players' chat orders: lua5.4 test_players.lua plugins/players.lua
local handler, calls = nil, {}
local visible = true
bot = { call = function(name, a)
    calls[#calls + 1] = { name = name, args = a }
    if name == "bots" then return { { name = "Bot1" }, { name = "Bot3" } } end
    if name == "entities" then return visible and { { type = "player", name = "jon", position = { x = 1, y = 70, z = 2 } } } or {} end
    if name == "status" then return { health = 20, food = 18, position = { x = 0, y = 70, z = 0 }, state = "idle", last_note = "" } end
    return {}
end, tool = function() end, every = function() end, on_chat = function(f) handler = f end, log = print }
load(io.open(arg[1]):read("a"), "players")()
local function find(tool, test) for _, c in ipairs(calls) do if c.name == tool and (not test or test(c.args)) then return c end end end
handler("<jon> bot3 follow")
local f = find("team_member")
assert(f and f.args.bot == "Bot3" and f.args.leader == "jon" and f.args.objective == "follow" and f.args.solo, "Bot3 follows jon")
calls = {}
handler("<Bot1> Bot3 follow")
assert(not find("team_member"), "the bots' own lines are not orders")
calls = {}
handler("<jon> all status")
local n = 0
for _, c in ipairs(calls) do if c.name == "status" then n = n + 1 end end
assert(n == 2, "all: every bot answers")
calls = {}
visible = false
handler("<jon> Bot1 come")
assert(find("chat", function(a) return a.message == "/tpa jon" end), "out of sight: asks to teleport to the player")
assert(not find("goto"))
print("players plugin OK")
