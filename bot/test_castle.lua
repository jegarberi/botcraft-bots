-- Offline check of the castle plugin: every stage of the full plan and of the miniature must build its list
-- without error. lua5.4 test_castle.lua plugins/castle.lua
local tool, tick, orders, handed, sites = nil, nil, {}, {}, {}
local creative = false
local crew, chats = false, {}
local eggs, animals = false, {}
local timers = {}
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
    if eggs and name == "get_block" and a.y == 70 then return {block = "minecraft:spruce_fence_gate", state = {open = "false"}} end
    if eggs and name == "entities" then
        local list = {}
        for _, animal in ipairs(animals) do if animal.type == a.type then list[#list + 1] = animal end end
        return list
    end
    if eggs and name == "use_item" then
        animals[#animals + 1] = {type = a.item:gsub("_spawn_egg$", ""), position = {x = a.x, y = a.y + 1, z = a.z}}
        return {}
    end
    if name == "get_block" then return { block = (a.y < 70 and not (a.x >= 5 and a.x <= 6 and a.z == 5 and a.y >= 66)) and "minecraft:stone" or "minecraft:air" } end
    if name == "status" then return { position = { x = 0, y = 70, z = 0 }, building = false, build_left = 0, build_idle = 0, game_mode = creative and a.bot ~= "Survival" and "creative" or "survival", dimension = "minecraft:overworld" } end
    if name == "chat" then chats[#chats + 1] = a return {} end
    if name == "bots" and crew then return { { name = "Bot1" }, { name = "Bot2" }, { name = "Bot3" }, { name = "Survival" } } end
    if name == "bots" then return creative and { { name = "Bot1" }, { name = "Survival" } } or { { name = "Bot1" } } end
    if name == "mines" then return { { x = 120, y = 70, z = 90, resources = "iron_ore,coal_ore" } } end
    if name == "vault" then return { chests = {}, full = false } end
    if name == "inventory" then return {} end
    return {}
end, tool = function(t) tool = t end, every = function(seconds, f) timers[seconds] = timers[seconds] or {}; timers[seconds][#timers[seconds] + 1] = f; local before = tick; tick = before and function() before(); f() end or f end, on_chat = function() end, log = print }
load(io.open(arg[1]):read("a"), "castle")()
-- A clock that moves 31 seconds a tick: the road builder gives a bot 30 seconds to take an order up
local clock = os.time()
os.time = function() return clock end
local plugin_tick = tick
tick = function() clock = clock + 31; plugin_tick() end
local plan = tool.run({ action = "plan", x = 0, y = 70, z = 0 })
assert(#plan.stages >= 10, "the full plan has all its stages")
for _, s in ipairs(plan.stages) do assert(s.blocks > 0, s.stage .. " is empty") end
local started = tool.run({ action = "start", x = 0, y = 70, z = 0 })
assert(started.started)
assert(timers[2], "creative completion has a two-second timer")
for _, f in ipairs(timers[2]) do f() end
assert(tool.run({action = "status"}).stage_number == 1, "survival stages retain their normal cadence")
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
local man = tool.run({ action = "roadbot", structures = "village_plains", x = 0, y = 70, z = 0, rail = false })
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
-- (The road builder is no castle builder: it goes back to the others first)
tool.run({ action = "roadbot", stop = true })
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
for _, f in ipairs(timers[2]) do f() end
assert(tool.run({action = "status"}).stage_number == 2, "creative stages advance on the two-second timer")
for _ = 1, 100 do
    if tool.run({action = "status"}).stage:match("^village") then break end
    tick()
end
assert(tool.run({action = "status"}).stage:match("^village"), "creative village stage reached")
local bricks, roof = false, false
for _, b in ipairs(orders[#orders].blocks) do
    assert(not b[4]:match("_log$") and not b[4]:match("_planks$"), "creative houses use masonry for their structure")
    bricks = bricks or b[4] == "stone_bricks"
    roof = roof or b[4]:match("^stone_brick_stairs%[") ~= nil
end
assert(bricks and roof, "creative houses have brick walls, posts and pitched roofs")
eggs = true
local ranch = tool.run({action = "ranch"})
assert(ranch.stocked == 4 and #animals == 8, "creative spawn eggs stock all four pens with breeding pairs")
assert(tool.run({action = "ranch"}).stocked == 4 and #animals == 8, "already stocked pens do not receive more animals")
eggs = false
tool.run({action = "stop"})
-- Three creative bots: each gets a part of its own, no block in two parts, every block in one, each says which
crew, orders, chats = true, {}, {}
assert(tool.run({action = "start", x = 0, y = 70, z = 0, creative = true}).started)
local seen, total, said = {}, 0, 0
for _, o in ipairs(orders) do
    assert(#o.members == 1 and o.members[1] == o.bot and o.leader == o.bot, "a part is built by its bot alone")
    for _, b in ipairs(o.blocks) do
        local key = b[1] .. " " .. b[2] .. " " .. b[3] .. " " .. b[4]
        assert(not seen[key], "no block is in two parts")
        seen[key], total = true, total + 1
    end
end
for _, c in ipairs(chats) do if c.message:match("takes part %d/3") then said = said + 1 end end
assert(#orders == 3 and said == 3, "each of the three bots takes one part and says so")
local stage_blocks
for _, c in ipairs(chats) do stage_blocks = stage_blocks or tonumber(c.message:match("^%[castle%] .-%(%d+/%d+%): (%d+) blocks")) end
assert(stage_blocks and stage_blocks > 0 and total == stage_blocks, "the parts hold every block of the stage: " .. total .. " of " .. tostring(stage_blocks))
tool.run({action = "stop"})
-- With a bot of another host on the site: strips of the same width, one per bot of every host; this host's
-- bots build only theirs, the other host's strip is left to it
TEAM_CREW = function(names)
    local c = { "zzzzzz:Remote1" }
    for _, n in ipairs(names) do c[#c + 1] = "local:" .. n end
    table.sort(c)
    return c
end
orders, chats = {}, {}
assert(tool.run({action = "start", x = 0, y = 70, z = 0, creative = true}).started)
assert(#orders >= 1 and #orders <= 3, "only this host's three bots get strips: " .. #orders)
for _, c in ipairs(chats) do
    local i, n, a, b = c.message:match("takes part (%d+)/(%d+) of .-: x (%-?%d+)%.%.(%-?%d+)")
    if i then
        assert(tonumber(n) == 4 and tonumber(i) <= 3, "four strips, the fourth for the other host: " .. c.message)
    end
end
for _, o in ipairs(orders) do
    local lo, hi = math.huge, -math.huge
    for _, b in ipairs(o.blocks) do lo, hi = math.min(lo, b[1]), math.max(hi, b[1]) end
    for _, other in ipairs(orders) do
        if other ~= o then for _, b in ipairs(other.blocks) do assert(b[1] < lo or b[1] > hi, "strips do not overlap") end end
    end
end
TEAM_CREW = nil
tool.run({action = "stop"})
crew = false
-- The repair bot: with no castle job, a creative bot goes over the plan part by part; it never clears a cell
-- planned empty outside the moat, nor touches the secret door's blocks, and is no builder of the next castle
orders = {}
local fix = tool.run({ action = "repair" })
assert(fix.bot == "Bot1", "the free creative bot repairs: " .. tostring(fix.bot))
for _ = 1, 6 do tick() end
assert(#orders >= 2, "a part per tick once the last is through: " .. #orders)
for _, o in ipairs(orders) do
    assert(o.bot == "Bot1" and o.solo and o.name == "castle repair" and #o.blocks > 0)
    for _, b in ipairs(o.blocks) do
        assert(b[4] ~= "air" or b[2] < 70 - 1, "no empty cell cleared above the ground (the moat is dug below it)")
        assert(not (b[1] == -10 and (b[2] == 77 or b[2] == 78) and b[3] == -11), "the secret door's blocks are left alone")
    end
end
assert(tool.run({ action = "repair" }).part == #orders)
assert(tool.run({ action = "repair", stop = true }):match("back"))
os.remove("../repair_job.lua")
print("castle plugin OK: " .. #plan.stages .. " stages, " .. plan.blocks .. " blocks")
