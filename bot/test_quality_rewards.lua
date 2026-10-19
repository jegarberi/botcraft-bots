-- Run from an empty temporary directory: lua5.4 /path/to/test_quality_rewards.lua /path/to/plugins
local plugins = assert(arg[1])
local tools = {}
local st = { name = "Builder", health = 20, food = 18, inventory = {}, stats = {}, deaths = 0, times_stuck = 0 }
bot = { call = function(name) return name == "status" and st or {} end, tool = function(t) tools[t.name] = t end, every = function() end,
    on_chat = function() end, log = function() end }
local function expose(file, names)
    local f = assert(io.open(plugins .. "/" .. file))
    local src = f:read("a"); f:close()
    for _, name in ipairs(names) do
        src = src:gsub("local function " .. name .. "%(", "function " .. name .. "(", 1)
    end
    assert(load(src, file))()
end
expose("brain.lua", { "own_score" })
expose("roles.lua", { "reward", "snapshot" })
local before = { stats = {}, living = NN.condition(st), deaths = 0, times_stuck = 0 }
local baseline = own_score(st, before)
st.stats.construction_penalty = 100
assert(own_score(st, before) == baseline - 100, "trade brain charges construction defects")
before.stats.construction_penalty = 100
assert(own_score(st, before) == baseline, "unchanged defects are not charged again")
st.stats.construction_penalty = 10
assert(own_score(st, before) == baseline - 10, "counter resets charge only new defects")
for _, role in ipairs({ "miners", "builders", "carpenters", "farmers" }) do
    st.stats = {}
    local old = snapshot(st, role)
    local normal = reward(role, old, snapshot(st, role))
    st.stats.construction_penalty = 100
    local bad = snapshot(st, role)
    assert(reward(role, old, bad) == normal - 100, role .. " action policy charges defects")
    assert(reward(role, bad, bad) == normal, "no repeated charge")
end
for _, role in ipairs({ "miners", "builders", "carpenters", "farmers" }) do
    st.stats = {}
    local old = snapshot(st, role)
    local normal = reward(role, old, old)
    st.stats.idle_seconds = 100
    local idle = snapshot(st, role)
    assert(reward(role, old, idle) == normal - 10, role .. " loses one point per ten idle seconds")
    assert(reward(role, idle, idle) == normal, "unchanged idle time is not charged twice")
    st.stats.idle_seconds = 10
    assert(reward(role, idle, snapshot(st, role)) == normal - 1, "idle counter reset")
end
st.stats = {}
local active_before = { stats = {}, living = NN.condition(st), deaths = 0, times_stuck = 0 }
local active_score = own_score(st, active_before)
st.stats.idle_seconds = 100
assert(own_score(st, active_before) == active_score - 10, "trade policy penalizes elapsed idle time")
active_before.stats.idle_seconds = 100
assert(own_score(st, active_before) == active_score, "no repeated daily charge")
st.stats.idle_seconds = 10
assert(own_score(st, active_before) == active_score - 1, "daily idle counter reset")
st.stats = {}
tools.roles.run({ act = "Builder", role = "builders", members = { "Builder" } })
st.building = true
st.stats.construction_penalty = 10
local pending = tools.roles.run({ act = "Builder", role = "builders", members = { "Builder" } })
assert(pending.action == "continue construction", "construction keeps its pending action")
st.building = false
local done = tools.roles.run({ act = "Builder", role = "builders", members = { "Builder" } })
assert(done.last_reward == -10, "completion penalty trains the action that started construction")
-- Food weighs like a stone tool, and every missing essential costs as long as it lacks
local empty = NN.condition({ health = 20, food = 20, inventory = {} })
local fed = NN.condition({ health = 20, food = 20, inventory = { bread = 4 } })
local tooled = NN.condition({ health = 20, food = 20, inventory = { stone_pickaxe = 1 } })
assert(empty.missing == 4 and fed.missing == 3 and tooled.missing == 3, "four essentials")
assert(NN.living_reward(empty, fed) - 2 == NN.living_reward(empty, tooled), "food gained = a stone tool gained (+2 for variety)")
local later = NN.condition({ health = 20, food = 20, inventory = {} })
later.at = empty.at + 300
assert(math.abs(NN.living_reward(empty, later) - (1 - 2)) < 1e-9, "5 minutes with nothing: -0.5 per missing essential")
local old = NN.condition({ health = 20, food = 20, inventory = { bread = 1 } })
old.has_food = nil
assert(NN.living_reward(old, fed) == 2 * (fed.variety - old.variety), "a snapshot from before the change gives no free bonus")
print("construction rewards OK: both policies, no repeated charge, counter resets, delayed construction credit, idle time")
