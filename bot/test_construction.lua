-- Run in an empty temporary directory: lua5.4 /path/to/test_construction.lua /path/to/plugins/empire.lua
local tools, orders = {}, {}
local terrain = function(x, z) return 70 + math.floor((x + z) / 8) end
bot = {
    call = function(name, a)
        if name == "get_block" then
            local h = terrain(a.x, a.z)
            if not h then error("unloaded") end
            local y = a.y_min and math.max(a.y_min, math.min(a.y, h)) or a.y
            return { block = y <= h and "minecraft:grass_block" or "minecraft:air", y = y }
        end
        if name == "team_member" then orders[#orders + 1] = a end
        if name == "bots" then return {} end
        return {}
    end,
    tool = function(t) tools[t.name] = t end,
    every = function() end, on_chat = function() end,
}
local f = assert(io.open(arg[1])); local src = f:read("a"); f:close()
for _, name in ipairs({ "road", "supply_crew", "homes", "hand", "completed", "worker" }) do src = src:gsub("local function " .. name .. "%(", "function " .. name .. "(") end
for _, name in ipairs({ "state", "support", "looks" }) do src = src:gsub("local " .. name .. " =", name .. " =") end
assert(load(src, "empire"))()
state.town = { x = 0, y = 71, z = 0 }
local function map(blocks)
    local m = {}
    for _, b in ipairs(blocks) do m[b[1] .. "," .. b[2] .. "," .. b[3]] = b[4] end
    return m
end
local function check_road(o)
    local blocks, err = road(o)
    assert(blocks, err)
    assert(#blocks <= 4096, "road fits a build order")
    local stairs = 0
    for _, b in ipairs(blocks) do if b[4]:match("stairs") then stairs = stairs + 1 end end
    local m = map(blocks)
    for k, p in pairs(state.roads) do
        for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
            local q = state.roads[(tonumber(k:match("^(-?%d+),")) + d[1]) .. "," .. (tonumber(k:match(",(-?%d+)$")) + d[2])]
            -- Adjacent parallel switchback runs may be separated by a wall; only test ordinary streets here.
            if q and not o.switchback then assert(math.abs(p.h - q.h) <= 1, "walkable grade") end
        end
    end
    return m, stairs
end
local a, stairs = check_road({ x = 18, y = 74, z = 0 })
assert(stairs > 0, "slopes use stairs")
local b = check_road({ x = 36, y = 77, z = 0 })
for k, item in pairs(a) do if b[k] then assert(item == b[k], "shared streets agree: " .. k) end end
local again = check_road({ x = 18, y = 74, z = 0 })
for k, item in pairs(a) do assert(again[k] == item, "replanning is stable: " .. k) end
local steep, steep_err = road({ x = 0, y = 50, z = 0 })
assert(steep, "steep approach uses a switchback: " .. tostring(steep_err))
assert(#steep <= 4096, "switchback fits a build order")
terrain = function() return nil end
local blocked, err = road({ x = 54, y = 71, z = 18 })
assert(not blocked and err:match("not loaded"), "unloaded terrain is deferred")
-- A supplier always remains available to finish its own home, even for a small team.
for n = 1, 15 do
    local names = {}; state.village, state.support = {}, nil
    for i = 1, n do names[i] = "Bot" .. i; state.village[names[i]] = {} end
    supply_crew(names)
    local count = 0; for _ in pairs(support) do count = count + 1 end
    assert(count < n, "not every unfinished homeowner becomes a supplier")
end
state.village.Bot1.done = true
supply_crew({ "Bot1", "Bot2", "Bot3", "Bot4", "Bot5" })
assert(support.Bot1, "finished homeowner takes over supply work")
state.village.Bot2.done = true
state.groups = { builders = { "Bot1", "Bot2" } }
assert(worker({ "Bot1", "Bot2" }) == "Bot2", "supply crew member is not reassigned to empire building")
-- An idle, cancelled order is not a finished house. One unfinished neighbor does not block a shop.
state.village = {
    Bot1 = { done = true, road = true, homed = true, at = { x = 0, y = 71, z = 0 }, plan = 2, grounded = true },
    Bot2 = { started = true, handed = os.time(), at = { x = 18, y = 71, z = 0 }, plan = 2, grounded = true },
}
state.groups, state.cells, state.lots, state.shops = { bakers = { "Bot1" } }, {}, 2, {}
support = {}
looks = { Bot1 = {}, Bot2 = { building = false, build_left = 0, build_done = false } }
homes({ "Bot1", "Bot2" }, os.time())
assert(not state.village.Bot2.road, "cancelled order is not completion")
assert(state.shops.bakers and state.shops.bakers.bot == "Bot1", "shop starts before every home finishes")
local large = { bot = "Bot2", name = "long road", blocks = {} }
for x = 1, 4100 do large.blocks[x] = { x, 70, 0, "stone" } end
assert(hand(large) and #orders[#orders].blocks == 4096, "large plans use bounded batches")
assert(not completed(large, { build_done = true, build_name = "other" }), "another build cannot finish this order")
assert(not completed(large, { build_done = true, build_name = large.name }), "first batch is not the entire road")
assert(#orders[#orders].blocks == 4, "remaining blocks are handed out")
assert(completed(large, { build_done = true, build_name = large.name }), "last verified batch completes the plan")
print("construction OK: stairs, shared streets, unloaded terrain, suppliers, cancelled orders, early shops")
