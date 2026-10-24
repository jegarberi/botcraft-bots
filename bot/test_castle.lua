-- Offline check of the castle plugin: every stage of the full plan and of the miniature must build its list
-- without error. lua5.4 test_castle.lua plugins/castle.lua
local tool, tick, orders, handed, sites = nil, nil, {}, {}, {}
local creative = false
bot = { call = function(name, a)
    if name == "team" then
        handed[#handed + 1] = a
        -- As the program does: the box goes into the stored sites
        local b = a.site
        if b then sites[#sites + 1] = { name = a.name, x1 = math.min(b[1], b[4]), y1 = math.min(b[2], b[5]), z1 = math.min(b[3], b[6]),
            x2 = math.max(b[1], b[4]), y2 = math.max(b[2], b[5]), z2 = math.max(b[3], b[6]) } end
        return {}
    end
    if name == "sites" then
        if a.remove then table.remove(sites, a.remove) end
        return sites
    end
    if name == "team_member" then orders[#orders + 1] = a return {} end
    if name == "locate" then return { x = 40, z = 0 } end
    -- A pit 3 deep at x 5..6, z 5: the floor over it must stand on dirt down to the stone
    if name == "get_block" then return { block = (a.y < 70 and not (a.x >= 5 and a.x <= 6 and a.z == 5 and a.y >= 66)) and "minecraft:stone" or "minecraft:air" } end
    if name == "status" then return { position = { x = 0, y = 70, z = 0 }, building = false, build_left = 0, build_idle = 0, game_mode = creative and a.bot ~= "Survival" and "creative" or "survival", dimension = "minecraft:overworld" } end
    if name == "bots" then return creative and { { name = "Bot1" }, { name = "Survival" } } or { { name = "Bot1" } } end
    if name == "mines" then return { { x = 120, y = 70, z = 90, resources = "iron_ore,coal_ore" } } end
    if name == "vault" then return { chests = {}, full = false } end
    if name == "inventory" then return {} end
    return {}
end, tool = function(t) tool = t end, every = function(_, f) local before = tick; tick = before and function() before(); f() end or f end, on_chat = function() end, log = print }
load(io.open(arg[1]):read("a"), "castle")()
local plan = tool.run({ action = "plan", x = 0, y = 70, z = 0 })
assert(#plan.stages >= 10, "the full plan has all its stages")
for _, s in ipairs(plan.stages) do assert(s.blocks > 0, s.stage .. " is empty") end
local started = tool.run({ action = "start", x = 0, y = 70, z = 0 })
assert(started.started)
-- The first stage handed over is the levelling: the pit is filled, cobblestone on top, dirt under it to the stone
local filled = {}
for _, b in ipairs(handed[1].blocks) do if b[1] == 5 and b[3] == 5 then filled[b[2]] = b[4] end end
assert(filled[69] == "cobblestone" and filled[68] == "dirt" and filled[66] == "dirt" and filled[65] == nil, "the pit under the floor is filled")
-- Started again elsewhere while still levelling: the old box goes, the new one stays; stopped: that one goes too
sites[#sites + 1] = { name = "house", x1 = 0, y1 = 60, z1 = 0, x2 = 5, y2 = 70, z2 = 5 }
assert(#sites == 2)
assert(tool.run({ action = "start", x = 300, y = 70, z = 0 }).started)
assert(#sites == 2 and sites[1].name == "house" and sites[2].x1 == 300 - 134, "the unbuilt castle's box is forgotten when it moves")
tool.run({ action = "stop" })
assert(#sites == 1 and sites[1].name == "house", "and when it is dropped")
assert(tool.run({ action = "test", x = 0, y = 70, z = 0 }).started)
tool.run({ action = "stop" })
local roads = tool.run({ action = "roads" })
assert(roads.started, "a road to the test mine is planned")
tool.run({ action = "stop" })
-- The road builder: one bot, 32 blocks at a time, signs at both ends, to the located village and to the mine
local man = tool.run({ action = "roadbot", structures = "village_plains", x = 0, y = 70, z = 0 })
assert(man.bot == "Bot1" and #man.places == 1 and #man.not_found == 0)
local signs = 0
for _ = 1, 12 do tick() end
for _, o in ipairs(orders) do
    assert(o.solo and o.bot == "Bot1" and #o.blocks > 0, "each order lays a piece")
    for _, b in ipairs(o.blocks) do if b[4]:match("^spruce_sign|") then signs = signs + 1 end end
end
assert(#orders == 7, "2 pieces to the village 40 blocks away, 5 to the mine 150 away, got " .. #orders)
assert(signs == 4, "a sign at each end of both roads, got " .. signs)
assert(tool.run({ action = "roadbot" }).roads_laid == 2)
-- The road builder and the castle job are kept in files next to the plugin's working directory
local saved = io.open("../roadbot_job.lua")
assert(saved, "the road builder is saved for a restart")
saved:close()
tool.run({ action = "stop" })
os.remove("../roadbot_job.lua")
os.remove("../castle_job.lua")
creative = true
local creative_plan = tool.run({ action = "plan", x = 0, y = 70, z = 0, creative = true })
local planted = false
for _, stage in ipairs(creative_plan.stages) do if stage.stage == "planted farms" then planted = stage.blocks == 7 * 80 * 2 end end
assert(planted, "creative plan includes tilled and planted farms")
orders = {}
assert(tool.run({action = "start", x = 0, y = 70, z = 0, creative = true}).started)
assert(#orders == 1 and orders[1].bot == "Bot1", "only creative bots receive automatic build orders")
tool.run({action = "stop"})
print("castle plugin OK: " .. #plan.stages .. " stages, " .. plan.blocks .. " blocks")
