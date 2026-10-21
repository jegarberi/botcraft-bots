-- Offline check of the food plan: lua5.4 test_food.lua plugins/food.lua plugins/empire.lua (from an empty
-- temporary directory)
local calls = {}
local tools = {}
bot = { call = function(name, a)
    calls[#calls + 1] = { name = name, args = a }
    if name == "leader" then return "Bot1" end
    if name == "berries" or name == "castle" then return {} end
    return {}
end, tool = function(t) tools[t.name] = t end, every = function() end, on_chat = function() end, log = print,
    spawn = function(_, f) f() return true end, busy = function() return false end }
NN = { edible = { bread = true, cooked_cod = true } }
load(io.open(arg[1]):read("a"), "food")()

-- Open water and rods: fishing first, up to what the water allows; then farming, a baker for three farmers
local r, cap = FOOD.rates({ animals = 0, bushes = 0, water = 64, fields = 0, rods = 4, biomes = {} })
local p, made = FOOD.plan(40, r, cap, 30)
assert(p.fishers == 8, "fishers first, as many as the water allows: " .. p.fishers)
assert(p.farmers > 0 and p.bakers == math.ceil(p.farmers / 3), "then farmers, a baker for three")
assert(made >= 40, "the eating is covered: " .. made)
-- Animals but little water: butchers, as many as the animals keep busy
r, cap = FOOD.rates({ animals = 18, bushes = 0, water = 3, fields = 0, rods = 0, biomes = {} })
p = FOOD.plan(30, r, cap, 30)
assert(p.fishers == 0 and p.butchers == 3, "butchers up to the animals: " .. p.butchers)
-- 60% of the bots at most
p = FOOD.plan(1000, r, cap, 12)
assert(p.fishers + p.butchers + p.farmers + p.bakers == 12, "never more than the share")

-- The hungry with nothing to eat walk to the bot carrying most food, which gives it some
calls = {}
local list = {
    { name = "Hungry", food = 3, inventory = {}, position = { x = 0, y = 64, z = 0 } },
    { name = "Fisher", food = 18, inventory = { cooked_cod = 30 }, position = { x = 40, y = 64, z = 5 } },
    { name = "Baker", food = 18, inventory = { bread = 9 }, position = { x = -10, y = 64, z = 0 } },
}
FOOD.feed(list, 1000)
assert(calls[1].name == "goto" and calls[1].args.bot == "Hungry" and calls[1].args.x == 40, "the hungry one goes to the bot with most food")
assert(calls[2].name == "give" and calls[2].args.bot == "Fisher" and calls[2].args.to == "Hungry", "which hands it food")
calls = {}
FOOD.feed(list, 1060)
assert(#calls == 0, "not again within 3 minutes")

-- The empire: the food trades get at least the plan, and bots move into them from the trades over their share
local src = io.open(arg[2]):read("a")
src = src:gsub("local function targets%(", "function targets("):gsub("local function split%(", "function split("):gsub("local function supply_crew%(", "function supply_crew("):gsub("local state =", "state =")
load(src, "empire")()
FOOD_PLAN = { fishers = 5, butchers = 0, farmers = 2, bakers = 1 }
local want = targets(15)
local sum = 0
for _, n in pairs(want) do sum = sum + n end
assert(want.fishers >= 5 and want.farmers >= 2 and sum == 15, "the plan's minimums, the same bots in all")
local names = {}
state.groups = { miners = {} }
for i = 1, 15 do names[i] = "Bot" .. i; table.insert(state.groups.miners, names[i]) end
local g = split(names)
assert(#g.fishers == 5, "5 miners moved to fishing: " .. #g.fishers)
-- Village mode: the house builders' supply crew follows the plan too (food first, the rest wood and stone),
-- and keeps its roles from one minute to the next
FOOD_PLAN = { fishers = 8, butchers = 0, farmers = 35, bakers = 12 }
state.village = {}
local villagers = {}
for i = 1, 30 do villagers[i] = "V" .. i; state.village[villagers[i]] = { done = i <= 10 } end
local function crew()
    supply_crew(villagers)
    local c = {}
    for _, role in pairs(state.support) do c[role] = (c[role] or 0) + 1 end
    return c
end
local c = crew()
assert(c.fishers == 1 and c.farmers == 4 and c.bakers == 1 and c.lumberjacks == 1 and c.miners == 1, "6 of 8 on food, as planned")
local first = {}
for n, role in pairs(state.support) do first[n] = role end
crew()
for n, role in pairs(state.support) do assert(first[n] == role, n .. " kept its role") end
print("food OK: fishing first with water and rods, butchers up to the animals, the hungry fed, the empire and the village supply crew follow the plan")
