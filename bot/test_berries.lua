-- Offline check of the berry picker: lua5.4 test_berries.lua plugins/berries.lua
-- One ripe bush at 0 70 0 on grass, the bot beside it with 10 berries: one pick, then berries planted around
local tool, tick, picked, placed = nil, nil, 0, {}
local world = { ["0,70,0"] = { block = "minecraft:sweet_berry_bush", state = { age = "3" } } }
bot = { call = function(name, a)
    if name == "status" then return { position = { x = 1, y = 70, z = 1 } } end
    if name == "bots" then return { { name = "Bot15" } } end
    if name == "find_blocks" then return { { x = 0, y = 70, z = 0 } } end
    if name == "get_block" then
        local b = world[a.x .. "," .. a.y .. "," .. a.z]
        if b then return b end
        return { block = a.y < 70 and "minecraft:grass_block" or "minecraft:air", state = {} }
    end
    if name == "interact_block" then picked = picked + 1 return {} end
    if name == "inventory" then return { { item = "minecraft:sweet_berries", count = 10 } } end
    if name == "place_block" then
        placed[#placed + 1] = a
        world[a.x .. "," .. a.y .. "," .. a.z] = { block = "minecraft:sweet_berry_bush", state = { age = "0" } }
        return {}
    end
    return {}
end, tool = function(t) tool = t end, every = function(_, f) tick = f end, on_chat = function() end, log = print }
load(io.open(arg[1]):read("a"), "berries")()
local r = tool.run({ bot = "Bot15" })
assert(r.patch == "0 70 0", "the nearest bush is the patch")
tick()
assert(picked == 1, "the ripe bush is picked")
assert(#placed == 4 and placed[1].item == "sweet_berries" and placed[1].y == 70, "berries planted on the 4 grass spots around it, 4 kept to eat")
assert(tool.run({}).planted == 4)
-- Kept for a restart: loaded again, the plugin has its picker back
bot.tool = function(t) tool = t end
load(io.open(arg[1]):read("a"), "berries")()
assert(tool.run({}).bot == "Bot15", "the picker comes back after a restart")
tool.run({ stop = true })
assert(not io.open("../berries_job.lua"), "stopping forgets it")
print("berries plugin OK")
