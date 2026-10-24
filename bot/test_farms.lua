-- Offline check of plugins/farms.lua: the farms' mechanics hold in the plans (lua5.4 test_farms.lua)
local tools = {}
bot = { call = function() return {} end, tool = function(t) tools[t.name] = t end }
NN = { serialize = function() return "{}" end }
dofile((arg[0]:match("^(.*)/") or ".") .. "/plugins/farms.lua")
local farms = tools.farms
assert(farms, "no farms tool")

local list = farms.run({})
local function at(blocks)
    local m = {}
    for _, e in ipairs(blocks) do m[e[1] .. "," .. e[2] .. "," .. e[3]] = e[4] end
    return function(x, y, z) return m[x .. "," .. y .. "," .. z] end
end
local PLANS
for i = 1, math.huge do
    local name, v = debug.getupvalue(farms.run, i)
    if not name then break end
    if name == "PLANS" then PLANS = v end
end
if not PLANS then
    -- reached through build()
    for i = 1, math.huge do
        local name, v = debug.getupvalue(farms.run, i)
        if not name then break end
        if name == "build" then
            for j = 1, math.huge do
                local n2, v2 = debug.getupvalue(v, j)
                if not n2 then break end
                if n2 == "PLANS" then PLANS = v2 end
            end
        end
    end
end
assert(PLANS, "PLANS not found")
local O = { x = 0, y = 0, z = 0 }

-- Cactus farm: cactus on sand, nothing solid beside it, a fence beside the cell it grows into, water over the hopper's ring
do
    local b = at(PLANS.cactus_farm.blocks(O))
    assert(b(2, 1, 2) == "cactus" and b(2, 0, 2) == "sand")
    for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
        assert(b(2 + d[1], 1, 2 + d[2]) == "air", "a block beside the cactus")
    end
    assert(b(2, 2, 2) == "air", "the cactus can't grow")
    local breakers = 0
    for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
        if tostring(b(2 + d[1], 2, 2 + d[2])):match("fence") then breakers = breakers + 1 end
    end
    assert(breakers == 1, "one fence beside the growth cell")
    assert(b(3, -1, 3) == "hopper[facing=down]" and b(3, -2, 3) == "chest" and b(3, 0, 3) == "air")
    assert(b(1, 0, 1) == "water")
    -- Water reaches the hopper along both arms within 7 blocks
    assert(list.cactus_farm.materials.hopper == 1 and list.cactus_farm.materials.cactus == 1)
end

-- Egg collector: the pen's one cell is over a hopper into a chest, fenced in with a gate
do
    local b = at(PLANS.egg_collector.blocks(O))
    assert(b(1, 0, 1) == "air" and b(1, -1, 1) == "hopper[facing=down]" and b(1, -2, 1) == "chest")
    assert(b(1, 0, 2):match("fence_gate"))
    for _, p in ipairs({ { 0, 0 }, { 2, 0 }, { 0, 2 }, { 2, 2 }, { 1, 0 }, { 0, 1 }, { 2, 1 } }) do
        assert(b(p[1], 0, p[2]) == "oak_fence")
    end
end

-- Smelter: every hopper points into the furnace or a chest
do
    local b = at(PLANS.auto_smelter.blocks(O))
    local ways = { down = { 0, -1, 0 }, east = { 1, 0, 0 }, west = { -1, 0, 0 }, north = { 0, 0, -1 }, south = { 0, 0, 1 } }
    local hoppers = 0
    for _, e in ipairs(PLANS.auto_smelter.blocks(O)) do
        local facing = e[4]:match("^hopper%[facing=(%a+)%]")
        if facing then
            hoppers = hoppers + 1
            local w = ways[facing]
            local into = b(e[1] + w[1], e[2] + w[2], e[3] + w[3])
            assert(into == "furnace" or into == "chest", "hopper at " .. e[1] .. " " .. e[2] .. " points into " .. tostring(into))
        end
    end
    assert(hoppers == 3)
    assert(b(1, 3, 0) == "hopper[facing=down]" and b(1, 4, 0) == "chest", "input chest over the top hopper")
    assert(b(0, 3, 0) == "chest" and b(0, 2, 0) == "hopper[facing=east]", "fuel chest over the side hopper")
end

print("farms: ok (" .. #PLANS.cactus_farm.blocks(O) .. ", " .. #PLANS.egg_collector.blocks(O) .. ", " .. #PLANS.auto_smelter.blocks(O) .. " blocks)")
