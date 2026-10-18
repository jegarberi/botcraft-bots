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
st.stats = {}
tools.roles.run({ act = "Builder", role = "builders", members = { "Builder" } })
st.building = true
st.stats.construction_penalty = 10
local pending = tools.roles.run({ act = "Builder", role = "builders", members = { "Builder" } })
assert(pending.action == "continue construction", "construction keeps its pending action")
st.building = false
local done = tools.roles.run({ act = "Builder", role = "builders", members = { "Builder" } })
assert(done.last_reward == -10, "completion penalty trains the action that started construction")
print("construction rewards OK: both policies, no repeated charge, counter resets, delayed construction credit")
