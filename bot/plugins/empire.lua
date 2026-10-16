-- An Age of Empires II style AI, optional (off until started): it plays the team like a computer player.
--   resources  food, wood, stone and ore: what the bots gathered (their inventories rising, looked at every minute),
--              spent on ages and techs
--   ages       Dark -> Feudal -> Castle -> Imperial: each one paid for, announced in the chat, and with its own
--              split of the bots ("villagers") over the trades, shifted toward the resource the next age lacks most
--   techs      paid for once their age is reached: better tools and armor, made by the blacksmiths and soldiers
--   the leader decides every bot's trade (the first bot leads if no one does): the age's split, filled with the bots
--              whose tools and things fit each trade best, the moves told in the chat
--   buildings  houses (one per 5 bots), lumber camp, mill, mining camp, barracks, blacksmith, market, monastery,
--              university and a wonder, in blocks, put up by one builder at a time in a town east of the castle;
--              each age needs two buildings of the age before
--   village    empire { village = true }: every bot builds a house of its own in the town first (all at once: a bed,
--              a double chest, a furnace, a table and chairs, a sign with its name), then a road joining it to the
--              others, then the leader gives it a trade; a bot that joins later does the same. The shared houses are left out then
--              Each bot sleeps in its own bed every night from then on; once every house stands, each trade builds its shop
--   attacks    from the Castle Age, every 10 minutes the soldiers go out together after hostile mobs
-- It decides the groups (the groups plugin carries them out, each trade's network still picks what a bot does), so the
-- brain rests while it plays and works again when it stops.
--   empire {}                   age, resources, what the next age costs, the split, what happened
--   empire { start = true }     play (taken up again after a restart)
--   ./run.sh --empire           plays from the start (BOTCRAFT_EMPIRE=1 in the environment)
--   ./run.sh --village          the same, in village mode (BOTCRAFT_EMPIRE=village)
--   empire { village = true }   a house for every bot, then its trade (starts playing if it was not)
--   empire { stop = true }      stop: the groups stay, the brain works again
local FILE = "../empire_job.lua"
local TRADES = { "bakers", "farmers", "butchers", "miners", "builders", "soldiers", "blacksmiths", "explorers", "fishers" }
local RESOURCES = { "food", "wood", "stone", "ore" }
-- Each age: its cost (to reach it) and how many bots of every 15 work in each trade
local AGES = {
    { name = "Dark Age", cost = {},
      mix = { farmers = 3, bakers = 2, butchers = 2, builders = 4, miners = 2, blacksmiths = 1, explorers = 1, soldiers = 0, fishers = 1 } },
    { name = "Feudal Age", cost = { food = 128, wood = 128 },
      mix = { farmers = 3, bakers = 2, butchers = 2, builders = 3, miners = 3, blacksmiths = 1, explorers = 0, soldiers = 1, fishers = 1 } },
    { name = "Castle Age", cost = { food = 192, stone = 256, ore = 64 },
      mix = { farmers = 2, bakers = 2, butchers = 2, builders = 3, miners = 3, blacksmiths = 1, explorers = 0, soldiers = 2, fishers = 1 } },
    { name = "Imperial Age", cost = { food = 256, stone = 256, ore = 192 },
      mix = { farmers = 2, bakers = 2, butchers = 1, builders = 3, miners = 2, blacksmiths = 1, explorers = 1, soldiers = 3, fishers = 1 } },
}
-- The trades that bring in each resource: the one the next age lacks most gets 2 more bots' weight in each
local NEED = { food = { "farmers", "bakers", "fishers" }, wood = { "builders", "explorers" }, stone = { "builders", "miners" }, ore = { "miners" } }
-- who: the trade that makes it; item and count: what each of them is told to make
local TECHS = {
    { name = "Stone Tools", age = 2, cost = { wood = 32, stone = 48 }, who = "blacksmiths", item = "stone_pickaxe", count = 4 },
    { name = "Bloomery", age = 2, cost = { wood = 48, ore = 16 }, who = "blacksmiths", item = "iron_ingot", count = 16 },
    { name = "Iron Tools", age = 3, cost = { ore = 48 }, who = "blacksmiths", item = "iron_pickaxe", count = 3 },
    { name = "Scale Mail", age = 3, cost = { food = 64, ore = 32 }, who = "soldiers", item = "iron_chestplate", count = 1 },
    { name = "Blast Furnace", age = 4, cost = { food = 64, ore = 64 }, who = "soldiers", item = "iron_sword", count = 1 },
    { name = "Plate Mail", age = 4, cost = { ore = 96 }, who = "soldiers", item = "iron_leggings", count = 1 },
}
local REBALANCE, ATTACK = 300, 600     -- seconds

local FOOD = { bread = 1, apple = 1, carrot = 1, potato = 1, beetroot = 1, wheat = 1, sweet_berries = 1, melon_slice = 1,
    beef = 1, porkchop = 1, mutton = 1, chicken = 1, rabbit = 1, cod = 1, salmon = 1 }
local STONE = { cobblestone = 1, cobbled_deepslate = 1, stone = 1, deepslate = 1, granite = 1, diorite = 1, andesite = 1 }
local ORE = { coal = 1, raw_iron = 1, raw_copper = 1, raw_gold = 1, diamond = 1, redstone = 1, lapis_lazuli = 1, emerald = 1 }
-- ponytail: gathered = inventories rising, so cooking or a bot handing items to another counts them twice; a count
-- of the chests would be exact, add one if the ages come too fast
local function kind(item)
    if FOOD[item] or item:match("^cooked_") then return "food" end
    if item:match("_log$") or item:match("_stem$") then return "wood" end
    if STONE[item] then return "stone" end
    if ORE[item] then return "ore" end
end

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil, (not ok) and tostring(r) or nil
end

local state = { enabled = false, age = 1, stock = { food = 0, wood = 0, stone = 0, ore = 0 }, techs = {}, groups = nil, log = {},
    built = {}, work = nil, town = nil, lots = 0 }
local seen = {}             -- bot -> resource -> what it carried at the last look
local looks = {}            -- bot -> its status at the last look (what the leader judges it by)
local last_split, last_attack, loaded_at = 0, 0, os.time()

local function save()
    local function ser(v)
        if type(v) == "table" then
            local p = {}
            for k, x in pairs(v) do p[#p + 1] = "[" .. ser(k) .. "]=" .. ser(x) end
            return "{" .. table.concat(p, ",") .. "}"
        end
        return type(v) == "string" and string.format("%q", v) or tostring(v)
    end
    local f = io.open(FILE, "w")
    if f then f:write("return ", ser(state)); f:close() end
end

do
    local f = io.open(FILE, "r")
    if f then
        local ok, s = pcall(load(f:read("a"), FILE, "t", {}) or function() end)
        f:close()
        if ok and type(s) == "table" then for k, v in pairs(s) do state[k] = v end end
        -- Groups saved under the trades' old names
        for old, new in pairs({ cooks = "bakers", breeders = "butchers", crafters = "blacksmiths" }) do
            if state.groups and state.groups[old] then state.groups[new], state.groups[old] = state.groups[old], nil end
        end
        -- After a restart no bot has its build order any more, and "not building, nothing left" looked like a building
        -- finished: every unfinished one is handed out again, a house whose road was under way checked first
        for _, w in pairs(state.village or {}) do
            if not w.done then w.handed, w.started, w.recheck = nil, false, w.road end
        end
        for _, w in pairs(state.shops or {}) do if not w.done then w.handed, w.started = 0, false end end
        if state.work then state.work.handed, state.work.started = 0, false end
    end
end

local function note(text)
    table.insert(state.log, 1, os.date("%H:%M ") .. text)
    state.log[11] = nil
end

local function announce(text)
    note(text)
    local bots = call("bots", {}) or {}
    if bots[1] then call("chat", { bot = bots[1].name, message = "[empire] " .. text }) end
end

local function affords(cost)
    for r, n in pairs(cost) do if (state.stock[r] or 0) < n then return false end end
    return true
end

local function pay(cost) for r, n in pairs(cost) do state.stock[r] = state.stock[r] - n end end

-- The bots' inventories now: what rose since the last look goes to the stock
local function gather(names)
    for _, name in ipairs(names) do
        local st = call("status", { bot = name })
        if type(st) == "table" and type(st.inventory) == "table" then
            looks[name] = st
            local now = { food = 0, wood = 0, stone = 0, ore = 0 }
            for item, c in pairs(st.inventory) do
                local k = kind(item)
                if k then now[k] = now[k] + (tonumber(c) or 0) end
            end
            if seen[name] then
                for _, r in ipairs(RESOURCES) do state.stock[r] = state.stock[r] + math.max(0, now[r] - seen[name][r]) end
            end
            seen[name] = now
        end
    end
end

-- How many bots in each trade: the age's mix, weighted toward what the next age lacks most, over the bots there are
local function targets(n)
    local mix = {}
    for t, w in pairs(AGES[state.age].mix) do mix[t] = w end
    local next_age = AGES[state.age + 1]
    if next_age then
        local worst, short = nil, 0
        for _, r in ipairs(RESOURCES) do
            local c = next_age.cost[r]
            local s = c and (c - state.stock[r]) / c or 0
            if s > short then worst, short = r, s end
        end
        for _, t in ipairs(worst and NEED[worst] or {}) do mix[t] = mix[t] + 2 end
    end
    local sum = 0
    for _, w in pairs(mix) do sum = sum + w end
    local out, rest, given = {}, {}, 0
    for _, t in ipairs(TRADES) do
        local q = mix[t] * n / sum
        out[t] = math.floor(q)
        given = given + out[t]
        rest[#rest + 1] = { t, q - out[t] }
    end
    table.sort(rest, function(a, b) return a[2] > b[2] end)       -- largest remainders get the bots left
    for i = 1, n - given do out[rest[i][1]] = out[rest[i][1]] + 1 end
    return out
end

-- What makes a bot fit for a trade: the tools and things it carries (ends of item names)
local FIT = {
    soldiers = { "_sword", "_chestplate", "_helmet", "shield" }, miners = { "_pickaxe", "torch", "raw_iron", "coal" },
    farmers = { "_hoe", "wheat_seeds", "egg", "milk_bucket", "pumpkin" }, butchers = { "beef", "porkchop", "mutton", "leather", "shears", "wool" },
    bakers = { "wheat", "bread", "pumpkin_pie", "cake" }, builders = { "cobblestone", "_planks", "_log" },
    blacksmiths = { "iron_ingot", "stick", "crafting_table", "furnace" }, explorers = { "boat", "map" }, fishers = { "fishing_rod", "cod", "salmon" },
}
local function fit(st, trade)
    local score = 0
    for item in pairs(st and st.inventory or {}) do
        for _, end_ in ipairs(FIT[trade]) do if item:sub(-#end_) == end_ then score = score + 1 end end
    end
    if trade == "soldiers" then score = score + (tonumber(st and st.health) or 20) / 10 end
    return score
end

-- The leader decides every bot's trade: the targets of the age, filled with the bots that fit each best (staying
-- in its trade counts for 1: a move costs the work under way). It is a builder itself, the berry picker a farmer, the explorer
-- (castle { action = "explore" }) an explorer.
-- No leader yet: the first bot leads.
local function split(names)
    local want = targets(#names)
    local leader = call("leader", {})
    if type(leader) ~= "string" or leader == "" then
        leader = names[1]
        call("leader", { name = leader })
    end
    local keep = { [leader] = "builders" }
    local picker = call("berries", {})
    if type(picker) == "table" and picker.bot and picker.bot ~= leader then keep[picker.bot] = "farmers" end
    local explorer = call("castle", { action = "explore" })      -- the road builder of castle { action = "explore" }
    if type(explorer) == "table" and explorer.bot and explorer.bot ~= leader then keep[explorer.bot] = "explorers" end
    local was = {}
    for t, list in pairs(state.groups or {}) do for _, n in ipairs(list) do was[n] = t end end
    local g, placed, pairs_ = {}, {}, {}
    for _, t in ipairs(TRADES) do g[t] = {} end
    for _, n in ipairs(names) do
        if keep[n] then
            g[keep[n]][#g[keep[n]] + 1] = n; placed[n] = true
        else
            for _, t in ipairs(TRADES) do
                pairs_[#pairs_ + 1] = { n, t, fit(looks[n], t) + (was[n] == t and 1 or 0) }
            end
        end
    end
    table.sort(pairs_, function(a, b)
        if a[3] ~= b[3] then return a[3] > b[3] end
        return a[1] .. a[2] < b[1] .. b[2]
    end)
    for _, p in ipairs(pairs_) do
        if not placed[p[1]] and #g[p[2]] < want[p[2]] then g[p[2]][#g[p[2]] + 1] = p[1]; placed[p[1]] = true end
    end
    for _, n in ipairs(names) do if not placed[n] then g.builders[#g.builders + 1] = n end end
    -- The leader tells the bots it moved
    local moves = {}
    for _, t in ipairs(TRADES) do
        for _, n in ipairs(g[t]) do if was[n] ~= t then moves[#moves + 1] = n .. " -> " .. t end end
    end
    for i = 1, #moves, 6 do
        call("chat", { bot = leader, message = "[empire] " .. table.concat(moves, ", ", i, math.min(i + 5, #moves)) })
    end
    return g, leader
end

local function same(a, b)
    if not a or not b then return false end
    for _, t in ipairs(TRADES) do
        if #(a[t] or {}) ~= #(b[t] or {}) then return false end
        for i, n in ipairs(a[t]) do if b[t][i] ~= n then return false end end
    end
    return true
end

-- ---------------------------------------------------------------- buildings
-- Age of Empires buildings, in Minecraft blocks: each one paid for, then put up by a builder taken off the trades
-- (never the leader), block by block (a solo build order: the castle goes on meanwhile), in a town east of the
-- castle's moat. As in the game, each age needs two buildings of the age before it (houses aside), and there is a
-- house for every 5 bots.
-- A hut: foundation, the space cleared, corner posts, walls (none when open) with a doorway on the south side,
-- a flat roof and, inset by one, a second layer over it. o = { x, y, z } its north-west corner at floor level
local function hut(o, w, d, h, wall, post, roof, open)
    local b = {}
    local function put(x, y, z, item) b[#b + 1] = { o.x + x, o.y + y, o.z + z, item } end
    for x = 0, w - 1 do
        for z = 0, d - 1 do
            put(x, -1, z, "cobblestone")
            for y = 0, h + 2 do put(x, y, z, "air") end
            local edge_x, edge_z = x == 0 or x == w - 1, z == 0 or z == d - 1
            for y = 0, h - 1 do
                if edge_x and edge_z then put(x, y, z, post)
                elseif (edge_x or edge_z) and not open and not (z == d - 1 and x == w // 2 and y < 2) then put(x, y, z, wall) end
            end
            put(x, h, z, roof)
            if x > 0 and x < w - 1 and z > 0 and z < d - 1 then put(x, h + 1, z, roof) end
        end
    end
    return b, put
end

local function with(blocks, o, list)
    for _, e in ipairs(list) do blocks[#blocks + 1] = { o.x + e[1], o.y + e[2], o.z + e[3], e[4] } end
    return blocks
end

local BUILDINGS = {
    { name = "House", age = 1, cost = { wood = 16 }, house = true,
      make = function(o) return hut(o, 5, 5, 3, "oak_planks", "oak_log", "spruce_planks") end },
    { name = "Lumber Camp", age = 1, cost = { wood = 32 },
      make = function(o) return with(hut(o, 7, 5, 3, nil, "oak_log", "spruce_planks", true), o,
          { { 2, 0, 2, "oak_log" }, { 3, 0, 2, "oak_log" }, { 4, 0, 2, "oak_log" }, { 3, 1, 2, "oak_log" }, { 1, 0, 1, "chest" } }) end },
    { name = "Mill", age = 1, cost = { wood = 32 },
      -- with sails: a cross of fences on the north face
      make = function(o)
          local s = { { 2, 0, 2, "crafting_table" } }
          for i = 1, 3 do
              s[#s + 1] = { 2, 3 + i, -1, "oak_fence" }; s[#s + 1] = { 2, 3 - i, -1, "oak_fence" }
              s[#s + 1] = { 2 + i, 3, -1, "oak_fence" }; s[#s + 1] = { 2 - i, 3, -1, "oak_fence" }
          end
          s[#s + 1] = { 2, 3, -1, "oak_log" }
          return with(hut(o, 5, 5, 4, "cobblestone", "oak_log", "oak_planks"), o, s) end },
    { name = "Mining Camp", age = 1, cost = { wood = 32 },
      make = function(o) return with(hut(o, 7, 5, 3, nil, "cobblestone", "cobblestone", true), o,
          { { 2, 0, 2, "cobblestone" }, { 3, 0, 2, "cobblestone" }, { 3, 1, 2, "cobblestone" }, { 1, 0, 1, "chest" } }) end },
    { name = "Barracks", age = 2, cost = { wood = 64, stone = 32 },
      make = function(o) return with(hut(o, 9, 7, 4, "cobblestone", "stone_bricks", "oak_planks"), o,
          { { 2, 0, 1, "oak_fence" }, { 2, 1, 1, "carved_pumpkin" }, { 6, 0, 1, "oak_fence" }, { 6, 1, 1, "carved_pumpkin" } }) end },
    { name = "Blacksmith", age = 2, cost = { wood = 48, stone = 32 },
      -- a forge and a chimney
      make = function(o) return with(hut(o, 7, 7, 3, "cobblestone", "cobblestone", "stone_bricks"), o,
          { { 1, 0, 1, "furnace" }, { 2, 0, 1, "furnace" }, { 5, 0, 1, "crafting_table" },
            { 1, 4, 1, "cobblestone" }, { 1, 5, 1, "cobblestone" }, { 1, 6, 1, "cobblestone" } }) end },
    { name = "Market", age = 2, cost = { wood = 64 },
      make = function(o) return with(hut(o, 9, 9, 3, nil, "oak_log", "oak_planks", true), o,
          { { 2, 0, 2, "barrel" }, { 6, 0, 2, "barrel" }, { 2, 0, 6, "chest" }, { 6, 0, 6, "barrel" }, { 4, 0, 4, "oak_fence" }, { 4, 1, 4, "torch" } }) end },
    { name = "Monastery", age = 3, cost = { stone = 96, wood = 32 },
      -- a bell tower over its north-east corner
      make = function(o)
          local b, put = hut(o, 9, 9, 5, "stone_bricks", "stone_bricks", "oak_planks")
          for y = 5, 10 do for x = 6, 8 do for z = 0, 2 do
              if y == 10 or x ~= 7 or z ~= 1 then put(x, y, z, "stone_bricks") end
          end end end
          put(7, 9, 1, "lantern")
          return b end },
    { name = "University", age = 3, cost = { wood = 64, stone = 64 },
      -- two floors, the upper one of wood
      make = function(o)
          local b = hut(o, 9, 9, 4, "stone_bricks", "stone_bricks", "stone_bricks")
          return with(b, o, (function()
              local up = {}
              for x = 1, 7 do for z = 1, 7 do for y = 5, 7 do
                  if x == 1 or x == 7 or z == 1 or z == 7 or y == 7 then up[#up + 1] = { x, y, z, y == 7 and "spruce_planks" or "oak_planks" } end
              end end end
              return up end)()) end },
    { name = "Wonder", age = 4, cost = { food = 128, wood = 128, stone = 256, ore = 128 },
      -- a stepped pyramid with a light on top
      make = function(o)
          local b = {}
          for t = 0, 5 do
              for x = t, 12 - t do for z = t, 12 - t do b[#b + 1] = { o.x + x, o.y + t, o.z + z, "stone_bricks" } end end
          end
          b[#b + 1] = { o.x + 6, o.y + 6, o.z + 6, "lantern" }
          return b end },
}

-- Where the next building goes: a grid of lots 18 blocks apart, 8 to a row, east of the castle (or of the leader),
-- the first free one in reading order
-- (Down to 80 below: the town's height came from where the leader stood, once 50 up on a hill.) Also whether it
-- found the ground: not in a chunk no bot sees
local function ground(x, z, from)
    for y = from + 24, from - 80, -1 do
        local r = call("get_block", { x = x, y = y, z = z })
        local name = type(r) == "table" and tostring(r.block or "") or ""
        -- (not cobblestone: the bots' floors and roads, a house's half-built floor up in the air among them)
        if name:match("dirt") or name:match("grass_block") or (name:match("stone") and not name:match("cobblestone")) or name:match("sand") or name:match("gravel")
            or name:match("clay") or name:match("podzol") or name:match("mud") or name:match("snow_block") or name:match("terracotta") then
            return y + 1, true
        end
    end
    return from, false
end

local COLS = 8
-- The lots taken, "column,row"; a town laid out before was three to a row, its lots in order
local function free_lot()
    if not state.cells then
        state.cells = {}
        for s = 0, (state.lots or 0) - 1 do state.cells[(s % 3) .. "," .. (s // 3)] = true end
    end
    local i = 0
    while state.cells[(i % COLS) .. "," .. (i // COLS)] do i = i + 1 end
    return i
end

-- A lot's place in reading order
local function lot_index(at) return (at.z - state.town.z) // 18 * COLS + (at.x - state.town.x) // 18 end

local function lot(leader)
    if not state.town then
        local f = io.open("../castle_site.txt", "r")
        local cx, cy, cz
        if f then cx, cy, cz = f:read("a"):match("(-?%d+) (-?%d+) (-?%d+)"); f:close() end
        if cx then
            state.town = { x = tonumber(cx) + 144, y = tonumber(cy), z = tonumber(cz) - 30 }
        else
            local st = call("status", { bot = leader })
            local p = type(st) == "table" and st.position or { x = 0, y = 64, z = 0 }
            state.town = { x = math.floor(p.x) + 30, y = math.floor(p.y), z = math.floor(p.z) }
        end
    end
    local i = free_lot()
    state.cells[(i % COLS) .. "," .. (i // COLS)] = true
    local x, z = state.town.x + (i % COLS) * 18, state.town.z + (i // COLS) * 18
    return { x = x, y = ground(x + 4, z + 4, state.town.y), z = z }
end

local function houses_needed(n) return math.ceil(n / 5) end

-- The next building to put up: in order, houses as the bots need them, anything of an age reached
local function next_building(n)
    for _, b in ipairs(BUILDINGS) do
        local have = state.built[b.name] or 0
        local want = b.house and houses_needed(n) or 1
        if have < want and state.age >= b.age and not (b.house and state.village) then return b end
    end
end

-- How many buildings (houses aside) of an age stand
local function built_of(age)
    local c = 0
    for _, b in ipairs(BUILDINGS) do if b.age == age and not b.house then c = c + (state.built[b.name] or 0) end end
    return c
end

-- A bot of the village still building its house: no trade, no other building for it until then
-- (or putting up its trade's shop)
local function homeless(n)
    local h = state.village and state.village[n]
    for _, w in pairs(state.shops or {}) do if w.bot == n and not w.done then return true end end
    return h and not h.done
end

local function hand(w)
    -- Later blocks replace earlier ones at the same spot: the air cleared first, the building after
    local seen, blocks = {}, {}
    for i = #w.blocks, 1, -1 do
        local k = w.blocks[i][1] .. " " .. w.blocks[i][2] .. " " .. w.blocks[i][3]
        if not seen[k] then seen[k] = true; table.insert(blocks, 1, w.blocks[i]) end
    end
    call("team_member", { bot = w.bot, objective = "build", plan = "blocks", members = { w.bot }, leader = w.bot, count = 1,
        solo = true, name = w.name, blocks = blocks })
    w.handed, w.started = os.time(), false
end

-- A builder (never the leader nor the berry picker) for the building under way, the one it had if still here
local function worker(names, leader)
    local here = {}
    for _, n in ipairs(names) do here[n] = true end
    local picker = call("berries", {})
    picker = type(picker) == "table" and picker.bot
    local explorer = call("castle", { action = "explore" })
    explorer = type(explorer) == "table" and explorer.bot
    for _, t in ipairs({ "builders", "explorers", "blacksmiths", "miners" }) do
        local list = (state.groups or {})[t] or {}
        for i = #list, 1, -1 do
            local n = list[i]
            if here[n] and n ~= leader and n ~= picker and n ~= explorer and not homeless(n) then return n end
        end
    end
end

-- A bot's own house: 7 by 7, with a bed, a double chest, a furnace, a table (a fence under a pressure plate) between
-- two chairs (stairs, their backs away from it), and a sign by the door with its name. The door is on the south side.
-- (Chests and furnaces are given no facing: they face the bot that places them, from inside)
local function home(o, owner)
    local b = hut(o, 7, 7, 3, "oak_planks", "oak_log", "spruce_planks")
    return with(b, o, {
        { 1, 0, 1, "white_bed[facing=north,part=head]" }, { 1, 0, 2, "white_bed[facing=north,part=foot]" },
        { 3, 0, 1, "furnace" }, { 4, 0, 1, "chest" }, { 5, 0, 1, "chest" },
        { 3, 0, 4, "oak_stairs[facing=west]" }, { 4, 0, 4, "oak_fence" }, { 4, 1, 4, "oak_pressure_plate" }, { 5, 0, 4, "oak_stairs[facing=east]" },
        { 2, -1, 7, "cobblestone" }, { 2, 0, 7, "oak_sign|" .. owner .. "'s|house" } })
end

-- Each trade's shop, in the village: a hut with its sign and what the trade works with
local SHOPS = {
    bakers = { "Bakery", { { 1, 0, 1, "furnace" }, { 2, 0, 1, "smoker" }, { 4, 0, 1, "crafting_table" }, { 5, 0, 1, "barrel" },
        { 1, 0, 3, "hay_block" }, { 5, 0, 3, "chest" } } },
    farmers = { "Farm Store", { { 1, 0, 1, "composter" }, { 2, 0, 1, "hay_block" }, { 3, 0, 1, "hay_block" }, { 5, 0, 1, "barrel" },
        { 1, 0, 3, "carved_pumpkin" }, { 5, 0, 3, "chest" } } },
    butchers = { "Butcher's", { { 1, 0, 1, "smoker" }, { 3, 0, 1, "crafting_table" }, { 5, 0, 1, "barrel" }, { 1, 0, 3, "chest" },
        { 5, 0, 3, "white_wool" } } },
    miners = { "Mining Office", { { 1, 0, 1, "furnace" }, { 3, 0, 1, "crafting_table" }, { 5, 0, 1, "chest" }, { 1, 0, 3, "iron_ore" },
        { 5, 0, 3, "coal_block" } } },
    fishers = { "Fish Market", { { 1, 0, 1, "smoker" }, { 2, 0, 1, "barrel" }, { 3, 0, 1, "barrel" }, { 5, 0, 1, "chest" },
        { 1, 0, 3, "cauldron" }, { 5, 0, 3, "crafting_table" } } },
    blacksmiths = { "Smithy", { { 1, 0, 1, "blast_furnace" }, { 2, 0, 1, "furnace" }, { 3, 0, 1, "anvil" }, { 4, 0, 1, "smithing_table" },
        { 5, 0, 1, "grindstone" }, { 1, 0, 3, "chest" }, { 5, 0, 3, "crafting_table" } } },
}
local function shop(o, trade)
    local name, inside = SHOPS[trade][1], SHOPS[trade][2]
    local b = with(hut(o, 7, 5, 3, "oak_planks", "stripped_oak_log", "spruce_planks"), o, inside)
    return with(b, o, { { 2, -1, 5, "cobblestone" }, { 2, 0, 5, "oak_sign|" .. name .. "|" } }), name
end

-- A bot's road, once its house stands: from its door south to the street in front of its row of lots (past the
-- deepest building, the Wonder), along the street west to the town's edge and up that edge to the first row, so
-- every house is joined to every other. Cobblestone with the space over it cleared.
-- ponytail: at the height of its own house, so two houses' roads meet with a step on a slope; a ground() per
-- column would follow the land at 48 get_block calls each
local function road(o)
    local b, t = {}, state.town
    local function pave(x, z)
        b[#b + 1] = { x, o.y - 1, z, "stone" }; b[#b + 1] = { x, o.y, z, "air" }; b[#b + 1] = { x, o.y + 1, z, "air" }
    end
    -- A lamp post beside the road every 7 blocks: a stone fence (cobblestone wall) with a torch on top, on the
    -- town's grid so that the roads sharing a stretch put theirs on the same spots
    local function post(x, z)
        b[#b + 1] = { x, o.y - 1, z, "stone" }; b[#b + 1] = { x, o.y, z, "cobblestone_wall" }; b[#b + 1] = { x, o.y + 1, z, "torch" }
    end
    for z = o.z + 7, o.z + 13 do pave(o.x + 3, z) end
    post(o.x + 4, o.z + 10)
    for x = t.x - 2, o.x + 3 do
        pave(x, o.z + 14); pave(x, o.z + 15)
        if (x - t.x) % 7 == 0 then post(x, o.z + 16) end
    end
    for z = t.z + 14, o.z + 15 do
        pave(t.x - 2, z); pave(t.x - 1, z)
        if (z - t.z) % 7 == 0 then post(t.x - 3, z) end
    end
    return b
end

-- The village: a house for each bot (a lot of the town), handed as its own solo build order, watched like the
-- building under way, then its road the same way; once that is paved the bot is split into a trade
local function homes(names, now)
    if not state.village then return end
    -- A house not built yet out past the town's lots (a town laid out three to a row stretched far south) moves to
    -- a free lot nearer in. ponytail: what was built of it stays where it was
    if state.town then
        local taken = 0
        for _ in pairs(state.cells or {}) do taken = taken + 1 end
        for n, h in pairs(state.village) do
            local i = lot_index(h.at)
            if not h.road and not h.done and i >= taken and free_lot() < i then
                state.cells[(i % COLS) .. "," .. (i // COLS)] = nil
                h.at = lot()
                h.blocks, h.handed, h.started = home(h.at, n), nil, false
                note(n .. "'s house moves nearer, to " .. h.at.x .. " " .. h.at.z)
            end
        end
    end
    -- A house not built yet that is not on the ground (planned 50 blocks up in the air): down to it.
    -- ponytail: what was built of it up there stays
    for n, h in pairs(state.village) do
        if not h.road and not h.done and not h.grounded then
            local y, found = ground(h.at.x + 4, h.at.z + 4, h.at.y)
            if found then
                h.grounded = true
                if y ~= h.at.y then
                    h.at.y, h.homed = y, nil
                    h.blocks, h.handed, h.started = home(h.at, n), nil, false
                    note(n .. "'s house moves down to the ground, at y " .. y)
                end
            end
        end
    end
    for _, n in ipairs(names) do
        local h = state.village[n]
        if not h then
            local at = lot(call("leader", {}))
            h = { name = n .. "'s house", bot = n, at = at, blocks = home(at, n) }
            state.village[n] = h
        end
        if h.recheck then
            h.road, h.recheck, h.name, h.blocks = nil, nil, n .. "'s house", home(h.at, n)
        end
        local st = looks[n]
        if (h.road or h.done) and not h.homed then
            -- Its bed (the head, at the house's north-west corner): slept in every night from now on
            h.homed = call("home", { bot = n, x = h.at.x + 1, y = h.at.y, z = h.at.z + 1 }) ~= nil
        end
        if h.done or (state.work and state.work.bot == n) then
            -- built, or putting up the empire's building first
        elseif not h.handed then
            hand(h)
            note(n .. " builds its house at " .. h.at.x .. " " .. h.at.z)
        elseif type(st) == "table" then
            if st.building then h.started = true end
            if h.started and not st.building and (st.build_left or 0) == 0 then
                if not h.road then
                    h.road, h.name, h.blocks = true, n .. "'s road", road(h.at)
                    hand(h)
                    announce(n .. " built its house at " .. h.at.x .. " " .. h.at.z .. ", now its road")
                else
                    h.done, h.blocks, last_split = true, nil, 0       -- the blocks only weigh on the saved file now
                    note(n .. " paved its road")
                end
            elseif now - h.handed >= 600 and not st.building then
                hand(h)     -- the order was lost: again
            end
        end
    end
    -- Every house up: each trade's shop, built by the first bot of that trade
    for _, n in ipairs(names) do if not (state.village[n] or {}).done then return end end
    state.shops = state.shops or {}
    for trade in pairs(SHOPS) do
        local w, who = state.shops[trade], ((state.groups or {})[trade] or {})[1]
        if not w and who then
            local at = lot(call("leader", {}))
            local blocks, name = shop(at, trade)
            w = { name = name, bot = who, at = at, blocks = blocks }
            state.shops[trade] = w
            hand(w)
            announce(who .. " builds the " .. name .. " at " .. at.x .. " " .. at.z)
            last_split = 0
        elseif w and not w.done then
            local st = looks[w.bot]
            if type(st) == "table" then
                if st.building then w.started = true end
                if w.started and not st.building and (st.build_left or 0) == 0 then
                    w.done, w.blocks, last_split = true, nil, 0
                    announce("the " .. w.name .. " is open at " .. w.at.x .. " " .. w.at.z)
                elseif now - w.handed >= 600 and not st.building then
                    hand(w)
                end
            end
        end
    end
end

-- The rule for buildings: the one under way watched, the next one paid for and handed out
local function construct(names, now)
    local w = state.work
    if w then
        local here = false
        for _, n in ipairs(names) do here = here or n == w.bot end
        local st = looks[w.bot]
        if here and type(st) == "table" then
            if st.building then w.started = true end
            if w.started and not st.building and (st.build_left or 0) == 0 then
                state.built[w.name] = (state.built[w.name] or 0) + 1
                announce(w.bot .. " built " .. (w.name == "Wonder" and "a Wonder: we win!" or ("a " .. w.name)) ..
                    " at " .. w.at.x .. " " .. w.at.z)
                state.work, last_split = nil, 0
                return
            end
            if now - w.handed >= 600 and not st.building then hand(w) end     -- the order was lost: again
        else
            local leader = call("leader", {})
            w.bot = worker(names, leader)
            if w.bot then hand(w) end
        end
        return
    end
    local b = next_building(#names)
    if not b or not affords(b.cost) then return end
    local leader = call("leader", {})
    local who = worker(names, leader)
    if not who then return end
    pay(b.cost)
    local at = lot(leader)
    state.work = { name = b.name, bot = who, at = at, blocks = b.make(at) }
    hand(state.work)
    last_split = 0
    announce(who .. " builds a " .. b.name .. " at " .. at.x .. " " .. at.z)
end

-- One look a minute: the rules, in order, like an AI script's
local function play(now)
    local names = {}
    for _, b in ipairs(call("bots", {}) or {}) do names[#names + 1] = b.name end
    if #names == 0 then return end
    gather(names)
    homes(names, now)
    construct(names, now)
    local next_age = AGES[state.age + 1]
    if next_age and affords(next_age.cost) and built_of(state.age) >= 2 then
        pay(next_age.cost)
        state.age = state.age + 1
        announce("we reached the " .. next_age.name .. "!")
        last_split = 0
    end
    for _, tech in ipairs(TECHS) do
        if not state.techs[tech.name] and state.age >= tech.age and affords(tech.cost) then
            local who = (state.groups or {})[tech.who] or {}
            if #who > 0 then
                pay(tech.cost)
                state.techs[tech.name] = true
                for _, n in ipairs(who) do call("make", { bot = n, item = tech.item, count = tech.count }) end
                announce("researched " .. tech.name .. " (" .. tech.who .. " make " .. tech.item .. ")")
            end
        end
    end
    if now - last_split >= REBALANCE then
        last_split = now
        local traders = {}
        for _, n in ipairs(names) do
            if not (state.work and state.work.bot == n) and not homeless(n) then traders[#traders + 1] = n end
        end
        -- Nobody free (every bot at its house or road): no trades at all, not the ones from before
        local g, leader = {}, "the leader"
        for _, t in ipairs(TRADES) do g[t] = {} end
        if #traders > 0 then g, leader = split(traders) end
        if not same(g, state.groups) then
            state.groups = g
            call("groups", { assign = g })
            local counts = {}
            for _, t in ipairs(TRADES) do if #g[t] > 0 then counts[#counts + 1] = #g[t] .. " " .. t end end
            note(leader .. " splits the villagers: " .. table.concat(counts, ", "))
        end
    end
    local soldiers = (state.groups or {}).soldiers or {}
    if state.age >= 3 and #soldiers >= 2 and now - last_attack >= ATTACK then
        last_attack = now
        for _, n in ipairs(soldiers) do
            call("team_member", { bot = n, objective = "attack", type = "hostile", members = soldiers, leader = soldiers[1], count = 8, solo = true })
        end
        note(#soldiers .. " soldiers attack")
    end
    save()
end

local function start()
    local brain = call("brain", {})
    state.brain_was = not (type(brain) == "table" and brain.enabled == false)
    call("brain", { stop = true })
    -- From the groups there are (names with what they did last: "Bot6: cook meat: ..."), so few bots move
    local cur = call("groups", { show = true })
    if type(cur) == "table" then
        state.groups = {}
        for _, t in ipairs(TRADES) do
            state.groups[t] = {}
            for _, n in ipairs(cur[t] or {}) do state.groups[t][#state.groups[t] + 1] = n:match("^[^:]+") end
        end
    end
    state.enabled, last_split, loaded_at = true, 0, 0
    note("started in the " .. AGES[state.age].name)
    save()
end

-- ./run.sh --empire (BOTCRAFT_EMPIRE=1): plays from the start, once the bots are in; --village (=village) in village mode
local auto = os.getenv("BOTCRAFT_EMPIRE")
bot.every(60, function()
    if (auto == "1" or auto == "village") and not state.enabled and os.time() - loaded_at >= 150 then start() end
    if auto == "village" and state.enabled and not state.village then state.village, last_split = {}, 0 end
    if not state.enabled or os.time() - loaded_at < 150 then return end
    play(os.time())
end)

bot.tool{
    name = "empire",
    description = "An Age of Empires II style computer player for the team, off unless started. It counts the food, wood, stone and ore the bots gather, pays for ages (Dark, Feudal, Castle, Imperial: each announced in the chat) and techs (better tools and armor, made by the blacksmiths and soldiers), has the leader decide each bot's trade by the age's split, what the next age lacks and the tools and things each bot carries (the groups tool carries it out; the brain rests meanwhile), has a builder put up Age of Empires buildings in a town east of the castle (houses, lumber camp, mill, mining camp, barracks, blacksmith, market, monastery, university, a wonder; each age needs two of the age before), village=true has every bot build a house of its own in the town first (bed, double chest, furnace, table and chairs, a sign with its name; it sleeps there every night), then a road joining it to the others, and then take a trade; once every house stands, each trade's shop goes up (bakery, farm store, butcher's, mining office, smithy) (starting play if off), and from the Castle Age sends the soldiers out together after hostile mobs every 10 minutes. With no argument: age, resources, next age's cost, the split, the last events. start=true plays (kept after a restart), stop=true stops (the groups stay, the brain works again).",
    params = { start = "boolean", stop = "boolean", village = "boolean" },
    run = function(args)
        if args.village then
            state.village = state.village or {}
            last_split = 0
            if not state.enabled then start() end
            save()
        end
        if args.start and not state.enabled then
            start()
        elseif args.stop and state.enabled then
            state.enabled = false
            if state.brain_was then call("brain", { start = true }) end
            note("stopped")
            save()
        end
        local next_age = AGES[state.age + 1]
        local techs = {}
        for _, t in ipairs(TECHS) do techs[#techs + 1] = t.name .. (state.techs[t.name] and " (done)" or "") end
        return { playing = state.enabled, age = AGES[state.age].name, resources = state.stock,
            next_age = next_age and { name = next_age.name, cost = next_age.cost, needs = "2 buildings of the " .. AGES[state.age].name ..
                " (" .. built_of(state.age) .. " built)" } or "none: Imperial is the last",
            buildings = state.built, building = state.work and (state.work.bot .. ": " .. state.work.name .. " at " ..
                state.work.at.x .. " " .. state.work.at.z) or "none", town = state.town,
            techs = techs, groups = state.groups, events = state.log, village = state.village and (function()
                local v = {}
                for n, h in pairs(state.village) do v[n] = (h.done and "house at " or h.road and "paving its road from " or "building its house at ") .. h.at.x .. " " .. h.at.z end
                for _, w in pairs(state.shops or {}) do v[w.name] = (w.done and "open at " or (w.bot .. " building it at ")) .. w.at.x .. " " .. w.at.z end
                return v end)() }
    end,
}
