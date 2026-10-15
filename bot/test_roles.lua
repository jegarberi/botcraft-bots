-- Offline check of the trades' networks: lua5.4 test_roles.lua plugins/brain.lua plugins/roles.lua
-- A miner whose iron trips pay and the rest not: its trade's network must come to prefer mining iron
os.remove("../roles_state.lua"); os.remove("../brain_state.lua")
local tools, last_ore = {}, nil
local stats = { collected = 0 }
bot = { call = function(name, a)
    if name == "status" then
        return { name = a.bot or "Bot1", health = 20, food = 18, day_time = 1000, inventory = { cobblestone = 10, iron_ingot = 2 },
            stats = stats, deaths = 0, times_stuck = 0, memory = { iron_ore = 3 }, position = { x = 0, y = 64, z = 0 } }
    end
    if name == "team_member" then last_ore = a.block end
    if name == "entities" then return {} end
    return {}
end, tool = function(t) tools[t.name] = t end, every = function() end, on_chat = function() end, log = print }
for i = 1, #arg do load(io.open(arg[i]):read("a"), arg[i])() end
local roles = tools.roles
local function want(action)
    for _, o in ipairs(roles.run({ network = "miners", bot = "Bot1" }).outputs) do if o.name == action then return o.percent end end
end
local before = want("mine iron")
for _ = 1, 300 do
    local r = roles.run({ act = "Bot1", role = "miners", members = { "Bot1" } })
    -- What it did paid off by the next turn
    if r.action == "mine iron" then stats.collected = stats.collected + 20 elseif r.action ~= "store" then stats.collected = stats.collected + 1 end
end
local after = want("mine iron")
assert(after > before + 15, string.format("learnt to mine iron: %.1f%% -> %.1f%%", before, after))
assert(last_ore and last_ore ~= "", "the miner's order went out")
local view = roles.run({ network = "miners", bot = "Bot1" })
assert(#view.inputs == 14 and #view.outputs == 5 and view.last_action, "the network for the panels")
local all = roles.run({})
assert(all.cooks and all.explorers and all.soldiers["hunt hostiles"], "every trade has its network")
-- Each trade acts without error
for _, role in ipairs({ "cooks", "farmers", "breeders", "builders", "soldiers", "crafters", "explorers" }) do
    for _ = 1, 10 do assert(roles.run({ act = "Bot2", role = role, members = { "Bot2" }, groups = { miners = { "Bot1" }, breeders = { "Bot3" } } }).action) end
end
os.remove("../roles_state.lua"); os.remove("../brain_state.lua")
print(string.format("roles plugin OK: mining iron %.1f%% -> %.1f%%", before, after))
