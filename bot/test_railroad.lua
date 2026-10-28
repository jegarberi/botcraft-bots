-- Offline check of the road with its railway: lua5.4 test_railroad.lua plugins/castle.lua
-- Ground at 70, a mountain at x 30..50 (up to 90), a valley at x 60..70 (down to 50). The road goes east along
-- z 0 to x 90, turns, and goes south to z 60
local function ground(x, z)
    if x >= 30 and x <= 50 and z <= 6 then return 90 end
    if x >= 60 and x <= 70 and z <= 6 then return 50 end
    return 70
end
bot = { call = function(tool, a)
    if tool == "team_member" then
        refusals = (refusals or 0) - 1
        if refusals >= 0 then error("busy") end
        given = a
        return {}
    end
    if tool == "get_block" then
        local laid = built and built[a.x .. "," .. a.y .. "," .. a.z]
        if laid then return { block = "minecraft:" .. laid:gsub("%[.*", ""):gsub("|.*", "") } end
        if a.y > ground(a.x, a.z) then return { block = "minecraft:air" } end
        return { block = "minecraft:stone" }
    end
    return {}
end, tool = function() end, every = function() end, on_chat = function() end, log = print }
local src = io.open(arg[1]):read("a"):gsub("local function rail_road%(", "function rail_road("):gsub("local function rail_route%(", "function rail_route("):gsub("local function rail_tick%(", "function rail_tick("):gsub("local function road_tick%(", "function road_tick("):gsub("\nlocal roadman ", "\nroadman = nil ")
assert(load(src, "castle"))()
local c = { sx = 0, sz = 0, px = 0, pz = 0, kx = 90, kz = 0, x = 90, z = 60, y = 70, leg = 1, has_turn = true, first = true, d0 = 180, name = "Farm" }
local world, rails, segs = {}, {}, {}
built = false
local function lay(ex, ez)
    local blocks, x, z, y, _, laid = rail_road(c, ex, ez, "cobblestone", 24)
    segs[c.leg] = segs[c.leg] or { ax = laid.ax, line = laid.line, rail = laid.rail, lv = {} }
    for a, l in pairs(laid.lv) do segs[c.leg].lv[a] = l end
    assert(blocks, "the piece is in sight")
    for _, b in ipairs(blocks) do
        world[b[1] .. "," .. b[2] .. "," .. b[3]] = b[4]
        if b[4]:match("rail") then rails[#rails + 1] = b end
    end
    c.first, c.px, c.pz, c.y = false, x, z, y
end
local deck = {}
while c.px ~= 90 do lay(math.min(c.px + 32, 90), 0) ; end
c.leg = 2
while c.pz ~= 60 do lay(90, math.min(c.pz + 32, 60)) end
-- The grade: the road's level along z 0 never moves more than 1 in 4 steps, and it tunnels through the mountain
for x = 0, 90 do
    for y = 40, 100 do if world[x .. "," .. y .. ",0"] == "cobblestone" or world[x .. "," .. y .. ",0"] == "redstone_block" then deck[x] = y end end
end
local last_change = -99
for x = 1, 90 do
    assert(math.abs(deck[x] - deck[x - 1]) <= 1, "one block at a time at x " .. x)
    if deck[x] ~= deck[x - 1] then assert(x - last_change >= 4, "4 steps between changes at x " .. x); last_change = x end
end
assert(deck[40] < 80 and world["40," .. deck[40] + 1 .. ",0"] == "air", "a tunnel through the mountain")
assert(deck[65] > 55, "a deck over the valley")
-- The railway: one chain, each rail joined to the next side by side, a slope where the level changes
table.sort(rails, function(a, b) return a[1] < b[1] or (a[1] == b[1] and a[3] < b[3]) end)
local at = {}
for _, r in ipairs(rails) do at[r[1] .. "," .. r[3]] = r end
local corner = at["88,2"]
assert(corner and corner[4] == "rail[shape=south_west]", "the curve at 88 2: " .. tostring(corner and corner[4]))
for _, r in ipairs(rails) do
    local links = 0
    for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
        local o = at[(r[1] + d[1]) .. "," .. (r[3] + d[2])]
        if o then
            links = links + 1
            assert(math.abs(o[2] - r[2]) <= 1, "neighbouring rails a block apart at most")
            if o[2] == r[2] + 1 then assert(r[4]:match("ascending"), "a rail below its neighbour slopes up: " .. r[1] .. " " .. r[3] .. " " .. r[4]) end
        end
    end
    assert(links >= 1 and links <= 2, "rail at " .. r[1] .. " " .. r[3] .. " joins " .. links)
    if r[4]:match("ascending") then
        assert(r[4]:match("^powered_rail"), "slopes are powered")
        assert(world[r[1] .. "," .. (r[2] - 1) .. "," .. r[3]] == "redstone_block", "a powered rail stands on a redstone block")
    end
end
local ends = 0
for _, r in ipairs(rails) do
    local links = 0
    for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do if at[(r[1] + d[1]) .. "," .. (r[3] + d[2])] then links = links + 1 end end
    if links == 1 then ends = ends + 1 end
end
assert(ends == 2, "one unbroken line: " .. ends .. " ends")
-- Milestones: the road starts 180 blocks from the castle, so at x 20 it is 200: a sign by the road (z -2, the torch
-- side) and one by the railway (z 3), each saying how far back to the castle and how far on (70 + 60)
local milestones = 0
for k, b in pairs(world) do
    if b:match("^spruce_sign|") then
        assert(b == "spruce_sign|Castle: 200|-> Farm|130 blocks|", "milestone text: " .. b)
        local x, z = k:match("^(-?%d+),%-?%d+,(-?%d+)$")
        assert(x == "20" and (z == "-2" or z == "3"), "milestone at " .. k)
        milestones = milestones + 1
    end
end
assert(milestones == 2, "a milestone by the road and by the railway, got " .. milestones)
-- Laid again over itself (as after a restart): the same levels all along, not a deck a block off
built = world
local again = { sx = 0, sz = 0, px = 0, pz = 0, kx = 90, kz = 0, x = 90, z = 60, y = 70, leg = 1, has_turn = true, first = true, d0 = 180, name = "Farm" }
while again.px ~= 90 do
    local blocks, x, z, y = rail_road(again, math.min(again.px + 32, 90), 0, "cobblestone", 24)
    for _, b in ipairs(blocks) do
        if b[3] == 0 and b[4] == "cobblestone" then assert(b[2] == deck[b[1]], "laid again at x " .. b[1] .. ": " .. b[2] .. ", was " .. deck[b[1]]) end
    end
    again.first, again.px, again.pz, again.y = false, x, z, y
end
built = false
-- The network: with that road laid, a place off its first leg gets a branch at a right angle from its nearest
-- point, at its level, 3 from its middle; that road's rail is cut there for 5 blocks
local r = { origin = { x = 0, y = 70, z = 0 }, done = { ["90 60"] = true }, net = { segs[1], segs[2] } }
local b = rail_route(r, { { name = "Farm", x = 20, z = -40 } })
assert(b.sx == 20 and b.sz == -3 and b.kx == 20 and b.kz == -40 and not b.has_turn, "a straight branch north from 20 -3")
assert(b.joined and b.y == deck[20], "the branch starts at the road's level")
assert(#b.cut == 5, "5 rails cut")
for _, cell in ipairs(b.cut) do
    assert(cell[4] == "air" and cell[3] == 2 and cell[1] >= 18 and cell[1] <= 22, "the cut is on the rail's line")
    assert(world[cell[1] .. "," .. cell[2] .. "," .. cell[3]]:match("rail"), "a rail is what is cut")
end
-- With nothing laid yet, the main road goes to the farthest place
local m = rail_route({ origin = { x = 0, y = 70, z = 0 }, done = {} }, { { name = "Near", x = 50, z = 10 }, { name = "Far", x = -300, z = 900 } })
assert(m.name == "Far" and m.sx == 0 and m.kz == 900, "the main road goes to the farthest place, the longer way first")
-- An order the bot refuses (busy with another tool) is not taken as given: the same piece goes again next time
local real_open = io.open
io.open = function(path, mode) if tostring(path):match("roadbot_job") then return nil end return real_open(path, mode) end
local rr = { bot = "Bot1", origin = { x = 0, y = 70, z = 0, name = "Castle" }, done = {}, laid = 0, item = "cobblestone", sign = "spruce_sign" }
local rc = { sx = 0, sz = 0, px = 0, pz = 0, kx = 90, kz = 0, x = 90, z = 60, y = 70, leg = 1, has_turn = true, first = true, d0 = 0, name = "Farm", what = "wheat" }
refusals, given = 1, nil
rail_tick(rr, rc, { position = { x = 0, y = 70, z = 0 } }, 0)
assert(not given and rc.px == 0 and rc.first and not rr.busy, "a refused piece stays to do")
rail_tick(rr, rc, { position = { x = 0, y = 70, z = 0 } }, 0)
assert(given and #given.blocks > 0 and rc.px == 32 and rr.busy, "and goes the next time")
-- Out of sight (nothing loaded there): the bot is sent there, not given a piece that gets nowhere
local blind = { sx = 0, sz = 0, px = 0, pz = 0, kx = 90, kz = 0, x = 90, z = 60, y = 70, leg = 1, has_turn = true, first = false, d0 = 0, name = "Farm", what = "" }
local real_call = bot.call
bot.call = function(tool, a) if tool == "get_block" and a.x >= 1 then error("position is not loaded") end return real_call(tool, a) end
given = nil
rail_tick(rr, blind, { position = { x = 0, y = 70, z = 300 } }, 0)
assert(given and #given.blocks == 1 and given.blocks[1][4] == "torch" and blind.px == 0, "a torch there takes the bot there")
bot.call = real_call
-- A railway road that reaches its end joins the network, for the branches
roadman = { bot = "Bot1", origin = { x = 0, y = 70, z = 0, name = "Castle" }, done = {}, laid = 0, item = "cobblestone", sign = "spruce_sign", rail = true, places = {},
    cur = { px = 90, pz = 60, x = 90, z = 60, kx = 90, kz = 0, leg = 2, has_turn = true, segs = segs } }
real_call = bot.call
bot.call = function(tool, a) if tool == "status" then return { position = { x = 90, y = 70, z = 60 } } end return real_call(tool, a) end
road_tick()
bot.call = real_call
assert(#roadman.net == 2 and roadman.done["90 60"], "the road's two legs are in the network")
io.open = real_open
print("railroad planner OK: " .. #rails .. " rails, branches, cuts, milestones, a refused order")
