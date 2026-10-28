-- Offline check of the keep's rooms (creative): lua5.4 test_keep.lua plugins/castle.lua
bot = { call = function() return {} end, tool = function() end, every = function() end, on_chat = function() end, log = function() end, spawn = function() end }
local structures, entities = load(io.open(arg[1]):read("a") .. "\nreturn structures, function() return KEEP_ENTITIES end", "castle")()
local cx, y, cz = 0, 70, 0
local stages = structures(cx, y, cz, "stone", "stone_bricks", true)
local world, keep_blocks = {}, nil
for _, s in ipairs(stages) do
    if s.name == "keep" then keep_blocks = s.blocks end
    for _, b in ipairs(s.blocks) do world[b[1] .. "," .. b[2] .. "," .. b[3]] = b[4] end
end
assert(keep_blocks, "a keep stage")
local seen = {}
for _, b in ipairs(keep_blocks) do
    local k = b[1] .. "," .. b[2] .. "," .. b[3]
    assert(not seen[k], "each spot once in the keep: " .. k)
    seen[k] = true
end
local function at(x, yy, z) return world[(cx + x) .. "," .. (y + yy) .. "," .. (cz + z)] end
local function solid(b) return b and b ~= "air" and not b:match("lantern") and not b:match("torch") and not b:match("button")
    and not b:match("ladder") and not b:match("pane") and not b:match("bars") and not b:match("_door") and not b:match("chain") end
-- The shaft: a ladder all the way from the lower level to the hidden passage, a wall behind each rung
for dy = -5, 8 do
    assert(at(-11, dy, -11) == "ladder[facing=south]", "ladder at dy " .. dy .. ": " .. tostring(at(-11, dy, -11)))
    assert(solid(at(-11, dy, -12)), "a wall behind the ladder at dy " .. dy .. ": " .. tostring(at(-11, dy, -12)))
end
-- The secret door: opening, door blocks, pistons, torch, the block it powers, the buttons' block and buttons
assert(at(-10, 7, -11) == "air" and at(-10, 8, -11) == "air", "the opening")
assert(at(-10, 7, -9) == "sticky_piston[facing=north]" and at(-10, 8, -9) == "sticky_piston[facing=north]", "two pistons")
assert(solid(at(-10, 7, -10)) and solid(at(-10, 8, -10)), "the door blocks")
assert(at(-10, 7, -8) == "redstone_wall_torch[facing=north]" and solid(at(-10, 7, -7)) and solid(at(-10, 8, -8)), "the torch, on its block, under the one it powers")
assert(at(-9, 7, -7):match("button") and at(-11, 7, -7):match("button"), "a button on each side")
assert(at(-11, 7, -9) == "air" and at(-11, 8, -6) == "air" and at(-9, 7, -11) == "air", "the passage and the way in are clear")
-- Beds whole, doors whole, buttons and hanging lanterns on something
local step = { north = { 0, -1 }, south = { 0, 1 }, east = { 1, 0 }, west = { -1, 0 } }
local beds, doors, buttons, lanterns = 0, 0, 0, 0
for k, b in pairs(world) do
    local x, yy, z = k:match("(-?%d+),(-?%d+),(-?%d+)")
    x, yy, z = tonumber(x), tonumber(yy), tonumber(z)
    local facing = b:match("facing=(%a+)")
    local inside = math.abs(x - cx) <= 12 and math.abs(z - cz) <= 12
    if not inside then
    elseif b:match("_bed%[") and b:match("part=foot") then
        local d = step[facing]
        assert(world[(x + d[1]) .. "," .. yy .. "," .. (z + d[2])] == b:gsub("part=foot", "part=head"), "a bed's head at " .. k)
        beds = beds + 1
    elseif b:match("_door%[") and b:match("half=lower") then
        assert(world[x .. "," .. (yy + 1) .. "," .. z] == b:gsub("half=lower", "half=upper"), "a door's upper half at " .. k)
        doors = doors + 1
    elseif b:match("button%[face=wall") then
        local d = step[facing]
        assert(solid(world[(x - d[1]) .. "," .. yy .. "," .. (z - d[2])]), "a button on a block at " .. k)
        buttons = buttons + 1
    elseif b:match("hanging=true") or b:match("^chain") then
        local above = world[x .. "," .. (yy + 1) .. "," .. z]
        assert(solid(above) or (above and above:match("^chain")), "hung from something at " .. k .. ": " .. tostring(above))
        lanterns = lanterns + 1
    end
end
assert(beds == 8 and doors == 12 and buttons == 8, "beds " .. beds .. ", doors " .. doors .. ", buttons " .. buttons)
-- The paintings: in a free spot, against a wall
local back = { south = { 0, -1 }, north = { 0, 1 }, west = { 1, 0 }, east = { -1, 0 } }
local paintings = 0
for _, e in ipairs(entities()) do
    if e.kind == "painting" then
        local d = back[e.facing]
        assert(world[e.x .. "," .. e.y .. "," .. e.z] == "air", "a painting's spot is free: " .. e.x .. " " .. e.y .. " " .. e.z)
        assert(solid(world[(e.x + d[1]) .. "," .. e.y .. "," .. (e.z + d[2])]), "a wall behind the painting at " .. e.x .. " " .. e.y .. " " .. e.z)
        paintings = paintings + 1
    end
end
assert(paintings == 14, "paintings: " .. paintings)
-- The vault's stair stays open through the hall's floor
for x = -2, 1 do assert(at(x, -1, 0) == "air", "the vault stair at " .. x) end
print("keep OK: " .. #keep_blocks .. " blocks, " .. beds .. " beds, " .. doors .. " doors, " .. lanterns .. " lights, " .. paintings .. " paintings")
