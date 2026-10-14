-- Offline check of the castle plugin's road planner: lua5.4 test_road.lua plugins/castle.lua
-- A made-up profile along x: flat, a river 6 wide, flat, a ravine 6 deep and 7 wide, flat, then a hillside going down for good.
local profile = {}
for x = 0, 9 do profile[x] = { 70, "minecraft:grass_block" } end
for x = 10, 15 do profile[x] = { 69, "minecraft:water" } end
for x = 16, 25 do profile[x] = { 70, "minecraft:grass_block" } end
for x = 26, 32 do profile[x] = { 64, "minecraft:stone" } end
for x = 33, 40 do profile[x] = { 70, "minecraft:grass_block" } end
for x = 41, 90 do profile[x] = { 70 - (x - 40), "minecraft:stone" } end
bot = { call = function(tool, a)
    if tool == "get_block" then
        local col = profile[a.x]
        if not col then error("not loaded") end
        if a.y > col[1] then return { block = "minecraft:air" } end
        return { block = a.y == col[1] and col[2] or "minecraft:stone" }
    end
    return {}
end, tool = function() end, every = function() end, on_chat = function() end, log = print }
-- The planner is local to the plugin: expose it for the test
local src = io.open(arg[1]):read("a"):gsub("local function road%(", "function road(")
load(src, "castle")()
local blocks, _, bridged = road(0, 0, 90, 0, 70, "cobblestone")
local deck_at = {}
for _, b in ipairs(blocks) do
    if b[4] == "cobblestone" and b[3] == 0 then deck_at[b[1]] = math.max(deck_at[b[1]] or -999, b[2]) end
end
assert(deck_at[12] == 70, "the river must be bridged at bank level")
assert(deck_at[29] == 70, "the ravine must be bridged at bank level")
assert(deck_at[45] == 65, "a hillside is followed down, not bridged")
assert(bridged == 13, "6 steps of river and 7 of ravine are bridge")
print("road planner OK")
