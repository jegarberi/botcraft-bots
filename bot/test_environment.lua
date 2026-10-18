-- Run from an empty temporary directory: lua5.4 /path/to/test_environment.lua /path/to/plugins
local tools = {}
local st = { name = "EnvBot", health = 20, food = 20, inventory = {}, stats = {}, day_time = 6000,
    rain_level = 0.4, thunder_level = 0.7, biome = "minecraft:plains" }
bot = { call = function(tool) return tool == "status" and st or {} end,
    tool = function(t) tools[t.name] = t end, every = function() end, on_chat = function() end }
assert(loadfile(arg[1] .. "/brain.lua"))()
assert(loadfile(arg[1] .. "/roles.lua"))()
local function near(a, b) assert(math.abs(a - b) < 1e-12, tostring(a) .. " != " .. tostring(b)) end
local x = NN.environment(st)
assert(#x == 72 and #NN.environment_inputs == 72)
near(x[1], 1); near(x[2], 0)
assert(x[3] == 0.4 and x[4] == 0.7 and x[5] == 0)
local ones = 0
for i = 5, #x do ones = ones + x[i] end
assert(ones == 1, "biomes use a one-hot category")
for _, time in ipairs({ 0, 6000, 12000, 18000, 23999 }) do
    local a = NN.environment({ day_time = time })
    local b = NN.environment({ day_time = time + 24000 })
    near(a[1], b[1]); near(a[2], b[2])
end
local missing = NN.environment({})
assert(missing[3] == 0 and missing[4] == 0 and missing[5] == 1)
assert(NN.environment({ biome = "mod:custom" })[5] == 1)
assert(NN.environment({ biome = "dappled_forest" })[5] == 0, "new 26.3 biome is recognized")
local clamped = NN.environment({ rain_level = -2, thunder_level = 3 })
assert(clamped[3] == 0 and clamped[4] == 1)
for _, view in ipairs({ tools.brain.run({ network = st.name }), tools.roles.run({ network = "miners", bot = st.name }) }) do
    local inputs = {}
    for _, input in ipairs(view.inputs) do inputs[input.name] = input.value end
    near(inputs["time of day sine"], 1)
    assert(inputs["rain strength"] == 0.4 and inputs["thunder strength"] == 0.7 and inputs["biome plains"] == 1)
end
for _, shape in ipairs({ { 44, 10, 11, 116 }, { 32, 8, 19, 104 } }) do
    local old = NN.new(shape[1], shape[2], shape[3], 42)
    local before, after = {}, {}
    for i = 1, shape[1] do before[i] = i / shape[1] end
    for i = 1, shape[4] do after[i] = before[i] or 0.8 end
    local p, _, value = NN.forward(old, before)
    local migrated = NN.grow(old, shape[4], shape[3])
    local q, _, migrated_value = NN.forward(migrated, after)
    for i = 1, #p do near(p[i], q[i]) end
    near(value, migrated_value)
    for _, row in ipairs(migrated.w1) do
        for i = shape[1] + 1, shape[4] do assert(row[i] == 0, "appended inputs preserve saved behavior") end
    end
end
print("environment OK: time cycle, weather, biome categories, both input layers, saved-weight migration")
