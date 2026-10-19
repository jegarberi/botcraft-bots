-- Offline check of the empire's resource count: lua5.4 test_empire_stock.lua plugins/empire.lua (from an empty
-- temporary directory). Gathering counts once; cooking, handing items to another bot and storing them add nothing.
assert(not io.open("../empire_job.lua"), "../empire_job.lua exists: run from a copy, or move it away")
local clock = 1000
os.time = function() return clock end
local inv = { Bot1 = {}, Bot2 = {} }
local stored = {}
local tool, tick
bot = { call = function(name, a)
    if name == "bots" then return { { name = "Bot1" }, { name = "Bot2" } } end
    if name == "status" then return { name = a.bot, inventory = inv[a.bot], stored = stored, health = 20, position = { x = 0, y = 64, z = 0 } } end
    if name == "brain" then return { enabled = true } end
    if name == "leader" then return "Bot1" end
    return {}
end, tool = function(t) tool = t end, every = function(_, f) tick = f end, on_chat = function() end, log = print }
load(io.open(arg[1]):read("a"), "empire")()
tool.run({ start = true })
local function minute() clock = clock + 60; tick() end
local function stock() return tool.run({}).resources end
clock = clock + 200
minute()                                                    -- the first look: nothing counted yet
inv.Bot1 = { beef = 5 }
minute()
assert(stock().food == 5, "5 beef gathered: " .. stock().food)
inv.Bot1 = { cooked_beef = 5 }
minute()
assert(stock().food == 5, "cooking adds nothing: " .. stock().food)
inv.Bot1, inv.Bot2 = {}, { cooked_beef = 5 }
minute()
assert(stock().food == 5, "handing it over adds nothing: " .. stock().food)
inv.Bot2, stored = {}, { ["minecraft:cooked_beef"] = 5 }
minute()
assert(stock().food == 5, "storing it adds nothing: " .. stock().food)
inv.Bot1 = { oak_log = 3, cobblestone = 2 }
minute()
local s = stock()
assert(s.food == 5 and s.wood == 3 and s.stone == 2, "new gathering counts")
inv.Bot1 = { oak_log = 3 }                                  -- 2 cobblestone placed in a build: nothing taken off
minute()
assert(stock().stone == 2, "using blocks does not lower the stock")
os.remove("../empire_job.lua")
print("empire stock OK: gathering counts once; cooking, giving and storing add nothing")
