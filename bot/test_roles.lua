-- Offline check of the trades' networks: lua5.4 test_roles.lua plugins/brain.lua plugins/roles.lua
-- A miner whose iron trips pay and the rest not: its trade's network must come to prefer mining iron
math.randomseed(42)
os.remove("../roles_state.lua"); os.remove("../brain_state.lua")
local tools, last_ore, ticks = {}, nil, {}
local stats = { collected = 0 }
local gives = {}
local hungry, building = false, false
bot = { call = function(name, a)
    if name == "give" then gives[#gives + 1] = a end
    if name == "bots" and hungry then return { { name = "Hungry" } } end
    if name == "status" then
        local inv = { cobblestone = 10, iron_ingot = 2 }
        if a.bot == "Farm1" then inv = { wheat = 40, egg = 6 } end
        if hungry and a.bot == "Bot2" then inv.bread = 12 end
        return { name = a.bot or "Bot1", building = building, health = 20, food = a.bot == "Hungry" and 0 or 18, day_time = 1000, inventory = inv,
            stats = stats, deaths = 0, times_stuck = 0, memory = { iron_ore = 3 }, position = { x = 0, y = 64, z = 0 } }
    end
    if name == "team_member" then last_ore = a.block end
    if name == "entities" then return {} end
    return {}
end, tool = function(t) tools[t.name] = t end, every = function(_, f) ticks[#ticks + 1] = f end, on_chat = function() end, log = print }
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
assert(#view.inputs == 20 and #view.outputs == 11 and #view.activations == 3 and view.last_action, "the network for the panels")
local all = roles.run({})
assert(all.bakers and all.explorers and all.soldiers["hunt hostiles"], "every trade has its network")
-- Each trade acts without error
local groups = { miners = { "Bot1" }, butchers = { "Bot3" }, farmers = { "Farm1" }, bakers = { "Bot2" } }
for _, role in ipairs({ "bakers", "farmers", "butchers", "builders", "soldiers", "blacksmiths", "explorers", "fishers" }) do
    for _ = 1, 10 do
        local r = roles.run({ act = "Bot2", role = role, members = { "Bot2" }, groups = groups })
        assert(r.action and not tostring(r.note):match("^failed"), role .. " " .. tostring(r.action) .. ": " .. tostring(r.note))
    end
end
-- The exchange: a baker lacking eggs and wheat asks the farmer, who needs none of them itself
gives = {}
for _ = 1, 80 do roles.run({ act = "Bot2", role = "bakers", members = { "Bot2" }, groups = groups }) end
local got = false
for _, g in ipairs(gives) do got = got or (g.bot == "Farm1" and g.to == "Bot2" and (g.item == "minecraft:egg" and g.count == 2 or g.item == "minecraft:wheat" and g.count == 9)) end
assert(got, "the farmer handed the baker what it lacked")
-- And the farmer hands its surplus eggs to the baker on its own turn
gives = {}
for _ = 1, 80 do roles.run({ act = "Farm1", role = "farmers", members = { "Farm1" }, groups = groups }) end
got = false
for _, g in ipairs(gives) do got = got or (g.bot == "Farm1" and g.to == "Bot2" and (g.item == "minecraft:egg" or g.item == "minecraft:wheat")) end
assert(got, "the farmer gave its surplus to the baker")
local isolated = roles.run({ network = "miners", bot = "Independent" })
local isolated_weight = isolated.w1[1][1]
for _ = 1, 10 do roles.run({ act = "Bot1", role = "miners", members = { "Bot1" } }) end
assert(roles.run({ network = "miners", bot = "Independent" }).w1[1][1] == isolated_weight, "one bot's training cannot change another bot's network")
assert(roles.run({ network = "miners", bot = "Bot1" }).w1[1][1] ~= isolated_weight, "bots start with different policies")
hungry, gives = true, {}
local fed = roles.run({ act = "Bot2", role = "bakers", members = { "Bot2" }, groups = groups })
assert(fed.action == "feed the hungry team" and gives[1] and gives[1].to == "Hungry", "feeding overrides a random policy draw")
building, gives = true, {}
local kept = roles.run({ act = "Bot2", role = "bakers", members = { "Bot2" }, groups = groups })
assert(kept.action == "continue construction" and #gives == 0, "trade turns preserve an active construction order")
building, hungry = false, false
local clock, now = os.time, os.time()
os.time = function() return now end
-- Only the roles timer: empire mode disables the brain timer.
for _ = 1, 5 do now = now + 1501; ticks[#ticks]() end
local generation = roles.run({ network = "miners", bot = "Bot1" })
assert(generation.generation == 1 and generation.last_generation.winners[1] == "Bot1", "role evolution runs while the trade brain is stopped")
os.time = clock
os.remove("../roles_state.lua"); os.remove("../brain_state.lua")
print(string.format("roles plugin OK: mining iron %.1f%% -> %.1f%%", before, after))
