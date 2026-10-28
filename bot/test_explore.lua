-- Offline check of the castle plugin's explorer: lua5.4 test_explore.lua plugins/castle.lua
-- A flat world, two villages; the explorer finds the near one from the origin, then (standing on it, /locate gives
-- that one again) wanders off until the far one is the nearest
local pos = { x = 0, y = 71, z = 0 }
local orders, chat, tool, tick = {}, {}, nil, nil
local villages = { { x = 300, z = 0 }, { x = 300, z = 400 } }
bot = { call = function(name, a)
    if name == "status" then return { position = pos, building = false } end
    if name == "bots" then return { { name = "Bot1" } } end
    if name == "mines" then return {} end
    if name == "chat" then chat[#chat + 1] = a.message return {} end
    if name == "team_member" then orders[#orders + 1] = a return {} end
    if name == "get_block" then return { block = a.y > 70 and "minecraft:air" or "minecraft:grass_block" } end
    if name == "locate" then
        if a.name ~= "#minecraft:village" then error("none near") end
        local best
        for _, v in ipairs(villages) do
            local d = (v.x - pos.x) ^ 2 + (v.z - pos.z) ^ 2
            if not best or d < best.d then best = { x = v.x, z = v.z, d = d } end
        end
        return { x = best.x, y = 70, z = best.z }
    end
    return {}
end, tool = function(t) if t.name == "castle" then tool = t end end,
    every = function(_, f) tick = tick or f end, on_chat = function() end, log = print }
-- The plugin keeps its job in ../roadbot_job.lua: not over a real one
assert(not io.open("../roadbot_job.lua"), "../roadbot_job.lua exists: run from a copy, or move it away")
local ok, err = pcall(function()
load(io.open(arg[1]):read("a"), "castle")()
local r = tool.run({ action = "explore", bot = "Bot1", structures = "#minecraft:village", radius = 1000, rail = false })
assert(r.bot == "Bot1" and r.within == 1000)
signs = 0
for _ = 1, 400 do
    tick()
    local o = orders[#orders]
    -- The bot walks where it was sent: the last block of its last order
    if o and o.blocks and #o.blocks > 0 then
        local b = o.blocks[#o.blocks]
        pos = { x = b[1], y = 71, z = b[3] }
    end
end
for _, o in ipairs(orders) do
    for _, b in ipairs(o.blocks or {}) do if tostring(b[4]):match("^spruce_sign|") then signs = signs + 1 end end
end
local status = tool.run({ action = "explore" })
assert(#status.places == 2, "both villages found, got " .. #status.places)
assert(status.roads_laid == 2, "a road to each, got " .. tostring(status.roads_laid))
assert(#chat == 1 and chat[1]:match("village at 300 400"), "the find made exploring is announced")
-- 2 roads: 2 end signposts each, and waypoints at 128 and 256 on the first, 128..384 on the second
assert(signs >= 6, "signposts and waypoints, got " .. signs)
end)
os.remove("../roadbot_job.lua")
assert(ok, err)
print("explorer OK: 2 villages, " .. signs .. " signs")
