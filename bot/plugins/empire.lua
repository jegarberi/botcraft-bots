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
local TRADES = { "bakers", "farmers", "butchers", "miners", "builders", "soldiers", "blacksmiths", "explorers", "fishers", "lumberjacks", "carpenters" }
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
local NEED = { food = { "farmers", "bakers", "fishers" }, wood = { "lumberjacks", "carpenters" }, stone = { "builders", "miners" }, ore = { "miners" } }
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
local seen_stored = nil     -- resource -> what the known chests held at the last look
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
            if w.road and w.road_plan ~= 3 then w.done = nil end
            if not w.done then w.handed, w.started, w.recheck, w.offset = nil, false, w.road, nil end
        end
        for _, w in pairs(state.shops or {}) do if not w.done then w.handed, w.started = 0, false end end
        for _, w in pairs(state.walls or {}) do if not w.done then w.handed, w.started = 0, false end end
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

-- What the team holds of each resource, by item counts ("minecraft:" or not)
local function totals(items)
    local t = { food = 0, wood = 0, stone = 0, ore = 0 }
    for item, c in pairs(type(items) == "table" and items or {}) do
        local k = kind((tostring(item):gsub("^minecraft:", "")))
        if k then t[k] = t[k] + (tonumber(c) or 0) end
    end
    return t
end

-- What the team gathered since the last look goes to the stock: how much all that it holds grew (every bot's
-- inventory and what the known chests hold), so cooking, handing items to another bot or storing them moves things
-- without adding any. A bot not seen at the last look counts from the next one. Eating or building in the same
-- minute as gathering offsets it (never below nothing)
local function gather(names)
    looks = {}
    local gained, stored = { food = 0, wood = 0, stone = 0, ore = 0 }, nil
    for _, name in ipairs(names) do
        local st = call("status", { bot = name })
        if type(st) == "table" and type(st.inventory) == "table" then
            looks[name] = st
            local now = totals(st.inventory)
            if seen[name] then
                for _, r in ipairs(RESOURCES) do gained[r] = gained[r] + now[r] - seen[name][r] end
            end
            seen[name] = now
            -- The chests the bots know of are the same in every bot's status
            if not stored and type(st.stored) == "table" then stored = totals(st.stored) end
        end
    end
    if stored then
        if seen_stored then
            for _, r in ipairs(RESOURCES) do gained[r] = gained[r] + stored[r] - seen_stored[r] end
        end
        seen_stored = stored
    end
    for _, r in ipairs(RESOURCES) do state.stock[r] = state.stock[r] + math.max(0, gained[r]) end
end

-- How many bots in each trade: the age's mix, weighted toward what the next age lacks most, over the bots there are
local function targets(n)
    local mix = {}
    for t, w in pairs(AGES[state.age].mix) do mix[t] = w end
    mix.lumberjacks, mix.carpenters = 2, 1
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
    -- Enough food for everyone first (the food plugin's plan): the food trades get at least that many, taken from
    -- the other trades, the largest first
    local food = type(FOOD_PLAN) == "table" and FOOD_PLAN or {}
    for _, t in ipairs({ "fishers", "butchers", "farmers", "bakers" }) do
        local short = math.max(0, math.min(n, tonumber(food[t]) or 0) - out[t])
        while short > 0 do
            local from, most = nil, 0
            for _, u in ipairs(TRADES) do
                if not food[u] and out[u] > most then from, most = u, out[u] end
            end
            if not from then break end
            out[from], out[t], short = out[from] - 1, out[t] + 1, short - 1
        end
    end
    return out
end

-- What makes a bot fit for a trade: the tools and things it carries (ends of item names)
local FIT = {
    lumberjacks = { "_axe", "_log", "_sapling" }, carpenters = { "_planks", "_stairs", "chest", "stick" },
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

-- Assigned bots keep their trades; new bots fill the targets of the age by fit.
-- For new assignments the leader is a builder, the berry picker a farmer, the explorer
-- (castle { action = "explore" }) an explorer.
-- No leader yet: the first bot leads.
local support = {}
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
    for n, role in pairs(support) do keep[n] = role end
    local was = {}
    for t, list in pairs(state.groups or {}) do for _, n in ipairs(list) do was[n] = t end end
    local g, placed, pairs_ = {}, {}, {}
    for _, t in ipairs(TRADES) do g[t] = {} end
    -- Bots keep their trades, but when a food trade is short of the food plan (the team eats more than it makes),
    -- up to 5 bots move to it from the other trades with most over their share (never the leader, the berry picker,
    -- the explorer, a supplier)
    local food = type(FOOD_PLAN) == "table" and FOOD_PLAN or {}
    local have = {}
    for _, t in ipairs(TRADES) do have[t] = 0 end
    for _, n in ipairs(names) do if was[n] then have[was[n]] = have[was[n]] + 1 end end
    local moved = 0
    local order = {}
    for t in pairs(food) do if have[t] then order[#order + 1] = t end end
    table.sort(order, function(a, b) return (tonumber(food[a]) or 0) > (tonumber(food[b]) or 0) end)   -- what the plan wants most first
    for _, t in ipairs(order) do
        while moved < 5 and have[t] < (want[t] or 0) do
            local from, over = nil, 0
            for _, u in ipairs(TRADES) do
                if not food[u] and have[u] - (want[u] or 0) > over then from, over = u, have[u] - (want[u] or 0) end
            end
            if not from then break end
            local who
            for _, n in ipairs(names) do
                if was[n] == from and not keep[n] and n ~= leader then who = n end
            end
            if not who then break end
            was[who], have[from], have[t], moved = t, have[from] - 1, have[t] + 1, moved + 1
        end
    end
    for _, n in ipairs(names) do
        -- The roles given (the leader, the berry picker, the explorer, the village's supply crew, which follows the
        -- food plan) before the trades they had: with them after, a crew's new food roles never reached a bot
        if keep[n] then
            g[keep[n]][#g[keep[n]] + 1] = n; placed[n] = true
        elseif was[n] then
            g[was[n]][#g[was[n]] + 1] = n; placed[n] = true
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
    if not open then
        put(w // 2, 0, d - 1, "oak_door[facing=north,half=lower]")
        put(w // 2, 1, d - 1, "oak_door[facing=north,half=upper]")
        put(w // 2, 2, 1, "torch")
        put(w // 2 + 1, 2, d - 2, "torch")
    end
    return b, put
end

local function with(blocks, o, list)
    for _, e in ipairs(list) do blocks[#blocks + 1] = { o.x + e[1], o.y + e[2], o.z + e[3], e[4] } end
    return blocks
end

-- Every village lot gets a campfire with log seats and a walled garden plot (dirt with a composter and corner torches)
local function farm_and_campfire(b, o, with_water)
    local yard = {
        { 1, -1, 9, "cobblestone" }, { 1, 0, 9, "campfire" },
        { 1, -1, 8, "cobblestone" }, { 1, 0, 8, "oak_log" },
        { 0, -1, 9, "cobblestone" }, { 0, 0, 9, "oak_log" },
        { 1, -1, 10, "cobblestone" }, { 1, 0, 10, "oak_log" },
        { 2, -1, 8, "cobblestone" }, { 2, 0, 8, "oak_fence" }, { 2, 1, 8, "torch" },
    }
    for x = 8, 14 do
        for z = 0, 6 do
            yard[#yard + 1] = { x, -1, z, "cobblestone" }
            for y = 1, 3 do yard[#yard + 1] = { x, y, z, "air" } end
            if x == 8 or x == 14 or z == 0 or z == 6 then
                yard[#yard + 1] = { x, 0, z, "cobblestone" }
            elseif with_water and x == 11 and z == 3 then
                yard[#yard + 1] = { x, 0, z, "water" }
            else
                yard[#yard + 1] = { x, 0, z, "dirt" }
            end
        end
    end
    yard[#yard + 1] = { 8, 1, 3, "composter" }
    yard[#yard + 1] = { 8, 1, 0, "torch" }; yard[#yard + 1] = { 14, 1, 0, "torch" }
    yard[#yard + 1] = { 8, 1, 6, "torch" }; yard[#yard + 1] = { 14, 1, 6, "torch" }
    return with(b, o, yard)
end

local BUILDINGS = {
    { name = "House", age = 1, cost = { wood = 16 }, house = true,
      make = function(o) return hut(o, 5, 5, 3, "oak_planks", "oak_log", "spruce_planks") end },
    { name = "Lumber Camp", age = 1, cost = { wood = 32 },
      make = function(o) return with(hut(o, 7, 5, 3, nil, "oak_log", "spruce_planks", true), o,
          { { 2, 0, 2, "oak_log" }, { 3, 0, 2, "oak_log" }, { 4, 0, 2, "oak_log" }, { 3, 1, 2, "oak_log" }, { 1, 0, 1, "chest" } }) end },
    { name = "Mill", age = 1, cost = { wood = 32 },
      -- with sails: a cross of fences on the north face, plus an irrigated farm plot and campfire
      make = function(o)
          local s = { { 2, 0, 2, "crafting_table" } }
          for i = 1, 3 do
              s[#s + 1] = { 2, 3 + i, -1, "oak_fence" }; s[#s + 1] = { 2, 3 - i, -1, "oak_fence" }
              s[#s + 1] = { 2 + i, 3, -1, "oak_fence" }; s[#s + 1] = { 2 - i, 3, -1, "oak_fence" }
          end
          s[#s + 1] = { 2, 3, -1, "oak_log" }
          return farm_and_campfire(with(hut(o, 5, 5, 4, "cobblestone", "oak_log", "oak_planks"), o, s), o, true) end },
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
local function observer(x, z)
    local best, distance
    for name, st in pairs(looks) do
        local p = st.position
        if p then
            local d = (p.x - x) ^ 2 + (p.z - z) ^ 2
            if not distance or d < distance then best, distance = name, d end
        end
    end
    return best
end

local function block_at(x, y, z)
    local r = call("get_block", { bot = observer(x, z), x = x, y = y, z = z })
    return type(r) == "table" and tostring(r.block or "") or ""
end

-- (not cobblestone: the bots' floors and roads, a house's half-built floor up in the air among them)
local function is_ground(name)
    return name:match("dirt") or name:match("grass_block") or (name:match("stone") and not name:match("cobblestone")) or name:match("sand") or name:match("gravel")
        or name:match("clay") or name:match("podzol") or name:match("mud") or name:match("snow_block") or name:match("terracotta")
end

-- The ground's height at x z (the first block of ground going down), whether it was found, and whether it lies
-- under water or ice (a lake: a house there was built 20 blocks under water, the builders drowning)
local function ground(x, z, from)
    -- From high up: the town's height can be a cave's (where the leader stood), and from just over it the first
    -- "ground" was a cave floor: houses were planned 30 blocks under the surface
    local y, bottom = math.max(from + 24, 200), from - 80
    local wet = false
    while y >= bottom do
        local r = call("get_block", { bot = observer(x, z), x = x, y = y, z = z, y_min = bottom })
        if type(r) ~= "table" then return from, false, wet end
        y = tonumber(r.y) or y
        local name = tostring(r.block or "")
        if is_ground(name) then
            local over = block_at(x, y + 1, z)
            return y + 1, true, wet or over:match("water") ~= nil or over:match("seagrass") ~= nil or over:match("kelp") ~= nil
        end
        wet = wet or name:match("ice") ~= nil or name:match("water") ~= nil
        y = y - 1
    end
    return from, false, wet
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
        -- Every bot votes where (settle plugin); east of the castle, or by the leader, only if none could
        local voted = call("settle", { vote = true })
        local f = io.open("../castle_site.txt", "r")
        local cx, cy, cz
        if f then cx, cy, cz = f:read("a"):match("(-?%d+) (-?%d+) (-?%d+)"); f:close() end
        if type(voted) == "table" and voted.winner then
            state.town = { x = voted.winner.x, y = voted.winner.y, z = voted.winner.z }
            note(string.format("%d bots voted: the town goes at %d %d", voted.voters, state.town.x, state.town.z))
        elseif cx then
            state.town = { x = tonumber(cx) + 144, y = tonumber(cy), z = tonumber(cz) - 30 }
        else
            local st = call("status", { bot = leader })
            local p = type(st) == "table" and st.position or { x = 0, y = 64, z = 0 }
            state.town = { x = math.floor(p.x) + 30, y = math.floor(p.y), z = math.floor(p.z) }
        end
    end
    -- The next free lot that is not under water (a lot under water stays taken: it is not offered again)
    local x, z, y
    for _ = 1, 64 do
        local i = free_lot()
        state.cells[(i % COLS) .. "," .. (i // COLS)] = true
        x, z = state.town.x + (i % COLS) * 18, state.town.z + (i // COLS) * 18
        local found, wet
        y, found, wet = ground(x + 4, z + 4, state.town.y)
        if not wet then break end
        note("the lot at " .. x .. " " .. z .. " is under water: left empty")
    end
    return { x = x, y = y, z = z }
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
local function supply_crew(names)
    support = {}
    if not state.village then return end
    local pending, candidates = 0, {}
    for _, n in ipairs(names) do
        local h = state.village[n] or {}
        if not h.done then pending = pending + 1 end
        candidates[#candidates + 1] = n
    end
    if pending == 0 then return end
    -- Finished homeowners take over supply work, letting the last suppliers build their own homes.
    table.sort(candidates, function(a, b)
        local ah, bh = state.village[a] or {}, state.village[b] or {}
        if not not ah.done ~= not not bh.done then return not not ah.done end
        local ap, bp = state.support and state.support[a], state.support and state.support[b]
        if not not ap ~= not not bp then return not not ap end
        return a > b
    end)
    local crew = 4 * math.max(1, math.ceil(#names / 15))
    local roles = {}
    local plan = type(FOOD_PLAN) == "table" and FOOD_PLAN or nil
    if plan then
        -- Food first, as the food plugin's plan says (the house builders eat too and make none): house builders are
        -- drafted until the crew holds every food worker the plan wants, two more bringing wood and stone. Their
        -- houses wait; as the hunger goes down the plan shrinks and they go back to them
        local total = (plan.fishers or 0) + (plan.butchers or 0) + (plan.farmers or 0) + (plan.bakers or 0)
        local available = 0
        for _, n in ipairs(candidates) do
            if (state.village[n] or {}).done or available < #candidates - 1 then available = available + 1 end
        end
        crew = math.max(crew, math.min(available, total + 2))
        local slots = math.min(crew - 2, total)
        for _, t in ipairs({ "fishers", "butchers", "farmers", "bakers" }) do
            for _ = 1, math.floor(slots * (plan[t] or 0) / math.max(1, total) + 0.5) do roles[#roles + 1] = t end
        end
        while #roles > slots do table.remove(roles) end
        while #roles < crew do roles[#roles + 1] = #roles % 2 == 0 and "lumberjacks" or "miners" end
    else
        for _ = 1, crew / 4 do
            for _, role in ipairs({ "farmers", "bakers", "lumberjacks", "miners" }) do roles[#roles + 1] = role end
        end
    end
    -- Who supplies (as before: finished homeowners first, then all but the last builder), each in its role of the
    -- last minute while that role is still wanted: the plan moves a little each minute, the bots should not
    local crew_members, unfinished = {}, 0
    for _, n in ipairs(candidates) do
        if #crew_members == #roles then break end
        local done = (state.village[n] or {}).done
        if done or unfinished < pending - 1 then
            crew_members[#crew_members + 1] = n
            if not done then unfinished = unfinished + 1 end
        end
    end
    local left = {}
    for _, r in ipairs(roles) do left[r] = (left[r] or 0) + 1 end
    for _, n in ipairs(crew_members) do
        local before = state.support and state.support[n]
        if before and (left[before] or 0) > 0 then support[n], left[before] = before, left[before] - 1 end
    end
    for _, n in ipairs(crew_members) do
        if not support[n] then
            for _, r in ipairs(roles) do
                if (left[r] or 0) > 0 then support[n], left[r] = r, left[r] - 1; break end
            end
        end
    end
    state.support = support
end

local function homeless(n)
    local h = state.village and state.village[n]
    for _, w in pairs(state.shops or {}) do if w.bot == n and not w.done then return true end end
    for _, w in pairs(state.walls or {}) do if w.bot == n and not w.done then return true end end
    return h and not h.done and not support[n]
end

local function hand(w)
    -- Later blocks replace earlier ones at the same spot: the air cleared first, the building after
    local seen, blocks = {}, {}
    for i = #w.blocks, 1, -1 do
        local k = w.blocks[i][1] .. " " .. w.blocks[i][2] .. " " .. w.blocks[i][3]
        if not seen[k] then seen[k] = true; table.insert(blocks, 1, w.blocks[i]) end
    end
    local part = {}
    local first = (w.offset or 0) + 1
    local last = math.min(#blocks, first + 4095)
    for i = first, last do part[#part + 1] = blocks[i] end
    local r, err = call("team_member", { bot = w.bot, objective = "build", plan = "blocks", members = { w.bot }, leader = w.bot, count = 1,
        solo = true, name = w.name, blocks = part })
    if r == nil then note("could not assign " .. w.name .. ": " .. tostring(err)); return false end
    w.handed, w.started, w.batch_end, w.batch_total = os.time(), false, last, #blocks
    return true
end

local function completed(w, st)
    if st.build_done ~= true or st.build_name ~= w.name then return false end
    if (w.batch_end or 0) < (w.batch_total or 0) then
        w.offset = w.batch_end
        hand(w)
        return false
    end
    return true
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
            if here[n] and n ~= leader and n ~= picker and n ~= explorer and not homeless(n) and not support[n] then return n end
        end
    end
end

-- A bot's own house: 7 by 7, with a bed, a double chest, a furnace, a table (a fence under a pressure plate) between
-- two chairs (stairs, their backs away from it), and a sign by the door with its name. The door is on the south side.
-- (Chests and furnaces are given no facing: they face the bot that places them, from inside)
-- A plains villager's house: walls 4 high on a cobblestone base row, oak logs at the corners, and a pitched roof of
-- oak stairs (any wood the bot carries does, see OtherWood) over the north and south sides, overhanging by one,
-- with plank gables east and west. HOME_PLAN: houses planned before it are planned again
-- Clear any trees (_log or _leaves) standing on the 18x18 village lot before building
local function clear_lot(b, o)
    for dx = 0, 17 do
        for dz = 0, 17 do
            local r = call("get_block", { bot = observer(o.x + dx, o.z + dz), x = o.x + dx, y = o.y + 18, z = o.z + dz, y_min = o.y })
            local name = type(r) == "table" and tostring(r.block or "") or ""
            if name:match("_log$") or name:match("_leaves$") then
                for y = (tonumber(r.y) or o.y), o.y, -1 do
                    table.insert(b, 1, { o.x + dx, y, o.z + dz, "air" })
                end
            end
        end
    end
end

local HOME_PLAN = 5
local function home(o, owner)
    local H = 4
    local b, put = hut(o, 7, 7, H, "oak_planks", "oak_log", "air")
    for x = 1, 5 do
        if x ~= 3 then put(x, 0, 6, "cobblestone") end      -- the doorway in the middle of the south side
        put(x, 0, 0, "cobblestone"); put(0, 0, x, "cobblestone"); put(6, 0, x, "cobblestone")
    end
    for k = 0, 3 do
        for x = -1, 7 do
            -- A stair's high side is the way it faces: toward the ridge
            put(x, H - 1 + k, k - 1, "oak_stairs[facing=south,half=bottom]")
            put(x, H - 1 + k, 7 - k, "oak_stairs[facing=north,half=bottom]")
        end
        for z = k, 6 - k do
            if k == 0 then break end      -- the walls' top row
            put(0, H - 1 + k, z, "oak_planks"); put(6, H - 1 + k, z, "oak_planks")
            for x = 1, 5 do put(x, H - 1 + k, z, k < 3 and "air" or "oak_planks") end    -- the ridge on top
        end
    end
    -- The ground levelled under it first: o.y is the ground's height at its middle, which stays as it was (the house
    -- rests on it); under the floor, wherever the ground is lower, dirt down to it. Above, hut clears the air
    for x = 0, 6 do
        for z = 0, 6 do
            for y = o.y - 2, o.y - 12, -1 do
                local name = block_at(o.x + x, y, o.z + z)
                if name == "" or is_ground(name) then break end     -- the ground (or a chunk no bot sees)
                table.insert(b, 1, { o.x + x, y, o.z + z, "dirt" })
            end
        end
    end
    clear_lot(b, o)
    b = with(b, o, {
        { 1, 0, 1, "white_bed[facing=north,part=head]" }, { 1, 0, 2, "white_bed[facing=north,part=foot]" },
        { 3, 0, 1, "furnace" }, { 4, 0, 1, "chest" }, { 5, 0, 1, "chest" },
        { 3, 0, 4, "oak_stairs[facing=west]" }, { 4, 0, 4, "oak_fence" }, { 4, 1, 4, "oak_pressure_plate" }, { 5, 0, 4, "oak_stairs[facing=east]" },
        { 2, -1, 7, "cobblestone" }, { 2, 0, 7, "oak_sign|" .. owner .. "'s|house" },
        { 4, -1, 7, "cobblestone" }, { 4, 0, 7, "oak_fence" }, { 4, 1, 7, "torch" } })
    -- The way out of the door: 3 blocks straight out, 3 high, on a cobblestone path. On a hillside the ground came
    -- up to the door, and its owner was shut in with it while building
    for z = 7, 9 do
        b[#b + 1] = { o.x + 3, o.y - 1, o.z + z, "cobblestone" }
        for y = 0, 2 do b[#b + 1] = { o.x + 3, o.y + y, o.z + z, "air" } end
    end
    return farm_and_campfire(b, o, false)
end

-- Each trade's shop, in the village: a hut with its sign and what the trade works with
local SHOPS = {
    lumberjacks = { "Lumber Yard", { { 1, 0, 1, "chest" }, { 3, 0, 1, "crafting_table" }, { 5, 0, 1, "oak_log" } } },
    carpenters = { "Carpentry", { { 1, 0, 1, "crafting_table" }, { 3, 0, 1, "chest" }, { 5, 0, 1, "barrel" } } },
    bakers = { "Bakery", { { 1, 0, 1, "furnace" }, { 2, 0, 1, "smoker" }, { 4, 0, 1, "crafting_table" }, { 5, 0, 1, "barrel" },
        { 1, 0, 3, "hay_block" }, { 5, 0, 3, "chest" } } },
    farmers = { "Farm Store", { { 1, 0, 1, "composter" }, { 2, 0, 1, "hay_block" }, { 3, 0, 1, "hay_block" }, { 5, 0, 1, "barrel" },
        { 1, 0, 3, "carved_pumpkin" }, { 5, 0, 3, "chest" } } },
    butchers = { "Butcher's", { { 1, 0, 1, "smoker" }, { 3, 0, 1, "crafting_table" }, { 5, 0, 1, "barrel" }, { 1, 0, 3, "chest" },
        { 5, 0, 3, "white_wool" } } },
    miners = { "Mining Office", { { 1, 0, 1, "furnace" }, { 3, 0, 1, "crafting_table" }, { 5, 0, 1, "chest" }, { 1, 0, 3, "raw_iron_block" },
        { 5, 0, 3, "coal_block" } } },
    fishers = { "Fish Market", { { 1, 0, 1, "smoker" }, { 2, 0, 1, "barrel" }, { 3, 0, 1, "barrel" }, { 5, 0, 1, "chest" },
        { 1, 0, 3, "cauldron" }, { 5, 0, 3, "crafting_table" } } },
    blacksmiths = { "Smithy", { { 1, 0, 1, "blast_furnace" }, { 2, 0, 1, "furnace" }, { 3, 0, 1, "anvil" }, { 4, 0, 1, "smithing_table" },
        { 5, 0, 1, "grindstone" }, { 1, 0, 3, "chest" }, { 5, 0, 3, "crafting_table" } } },
}

-- Shared columns are saved with the town: every builder uses the same heights and stairs.
local ROAD_PLAN = 3
local function road(o)
    local t = state.town
    local function plan(switchback)
        local nodes, route = {}, {}
        local function key(x, z) return x .. "," .. z end
        local function node(x, z)
            local k = key(x, z)
            if not nodes[k] then
                local old = (state.roads or {})[k]
                local h, known
                if old then h, known = old.h, true else h, known = ground(x, z, t.y) end
                if not known then error("road ground is not loaded at " .. x .. " " .. z, 0) end
                nodes[k] = { x = x, z = z, h = h, ground = old and old.ground or h, fixed = old and old.h, item = old and old.item, edges = {} }
            end
            route[k] = true
            return nodes[k]
        end
        local function link(a, b)
            a.edges[key(b.x, b.z)], b.edges[key(a.x, a.z)] = b, a
        end
        local function line(x1, z1, x2, z2)
            local n = math.max(math.abs(x2 - x1), math.abs(z2 - z1))
            local prev
            for i = 0, n do
                local p = node(x1 + (x2 - x1) * i // math.max(1, n), z1 + (z2 - z1) * i // math.max(1, n))
                if prev then link(prev, p) end
                prev = p
            end
        end
        -- Streets have two lanes, with cross-links so both sides remain walkable.
        for z = t.z + 14, o.z + 15 do
            local a, b = node(t.x - 2, z), node(t.x - 1, z)
            link(a, b)
            if z > t.z + 14 then link(a, node(t.x - 2, z - 1)); link(b, node(t.x - 1, z - 1)) end
        end
        local end_x = o.x + (switchback and 15 or 3)
        for x = t.x - 2, end_x do
            local a, b = node(x, o.z + 14), node(x, o.z + 15)
            link(a, b)
            if x > t.x - 2 then link(a, node(x - 1, o.z + 14)); link(b, node(x - 1, o.z + 15)) end
        end
        if switchback then
            line(o.x + 3, o.z + 6, o.x + 3, o.z + 8)
            line(o.x + 3, o.z + 8, o.x + 15, o.z + 8)
            line(o.x + 15, o.z + 8, o.x + 15, o.z + 10)
            line(o.x + 15, o.z + 10, o.x + 3, o.z + 10)
            line(o.x + 3, o.z + 10, o.x + 3, o.z + 12)
            line(o.x + 3, o.z + 12, o.x + 15, o.z + 12)
            line(o.x + 15, o.z + 12, o.x + 15, o.z + 14)
        else
            line(o.x + 3, o.z + 6, o.x + 3, o.z + 14)
        end
        local door = node(o.x + 3, o.z + 6)
        if door.fixed and door.fixed ~= o.y then error("existing road disagrees with the doorway", 0) end
        door.fixed = o.y
        -- Lower/upper bounds spread from saved roads and the doorway, one block per step.
        for _, p in pairs(nodes) do p.lo, p.hi = p.fixed or -math.huge, p.fixed or math.huge end
        local changed = true
        while changed do
            changed = false
            for _, p in pairs(nodes) do
                for _, q in pairs(p.edges) do
                    local lo, hi = math.max(p.lo, q.lo - 1), math.min(p.hi, q.hi + 1)
                    if lo > hi then error("slope needs a longer approach", 0) end
                    if lo ~= p.lo or hi ~= p.hi then p.lo, p.hi, changed = lo, hi, true end
                end
            end
        end
        for _, p in pairs(nodes) do p.h = math.max(p.lo, math.min(p.hi, p.h)) end
        changed = true
        while changed do
            changed = false
            for _, p in pairs(nodes) do
                for _, q in pairs(p.edges) do
                    if p.h > q.h + 1 then p.h, changed = q.h + 1, true end
                end
            end
        end
        -- A stair cannot face two uphill exits: flatten that junction and its approaches.
        changed = true
        while changed do
            changed = false
            for _, p in pairs(nodes) do
                local high, required = 0, p.h
                for _, q in pairs(p.edges) do
                    if q.h > p.h then high = high + 1 end
                    required = math.max(required, q.h - 1)
                end
                if high > 1 and not p.fixed then required = math.max(required, p.h + 1) end
                if required > p.h then
                    if required > p.hi then error("slope needs a longer approach", 0) end
                    p.h, changed = required, true
                end
            end
        end
        local blocks, saved = {}, {}
        local keys = {}
        for k in pairs(route) do keys[#keys + 1] = k end
        table.sort(keys)
        local function add_lamp(lx, ly, lz)
            local lamp = key(lx, lz)
            if not route[lamp] and not (state.roads or {})[lamp] then
                blocks[#blocks + 1] = { lx, ly - 1, lz, "stone" }
                blocks[#blocks + 1] = { lx, ly, lz, "cobblestone_wall" }
                blocks[#blocks + 1] = { lx, ly + 1, lz, "torch" }
            end
        end
        for _, k in ipairs(keys) do
            local p = nodes[k]
            local item = p.item or "air"
            if not p.item then
                for _, q in pairs(p.edges) do
                    if q.h > p.h then
                        local facing = q.x > p.x and "east" or q.x < p.x and "west" or q.z > p.z and "south" or "north"
                        item = "stone_stairs[facing=" .. facing .. ",half=bottom]"
                        break
                    end
                end
            end
            for y = math.min(p.ground - 1, p.h - 1), p.h - 1 do blocks[#blocks + 1] = { p.x, y, p.z, "stone" } end
            blocks[#blocks + 1] = { p.x, p.h, p.z, item }
            for y = p.h + 1, math.max(p.h + 2, p.ground + 1) do blocks[#blocks + 1] = { p.x, y, p.z, "air" } end
            if p.z >= o.z + 14 and (p.x - t.x) % 7 == 0 then
                add_lamp(p.x, p.h, p.z + 1)
            end
            if p.x == t.x - 2 and (p.z - t.z) % 7 == 0 then
                add_lamp(p.x - 1, p.h, p.z)
            end
            if p.x == o.x + 3 and p.z == o.z + 10 then
                add_lamp(p.x - 1, p.h, p.z)
            end
            saved[k] = { h = p.h, ground = p.ground, item = item }
        end
        return blocks, saved
    end
    local ok, blocks, columns = pcall(plan, false)
    if not ok then ok, blocks, columns = pcall(plan, true) end
    if not ok then return nil, blocks end
    state.roads = state.roads or {}
    for k, v in pairs(columns) do state.roads[k] = v end
    return blocks
end

local function shop(o, trade)
    local name, inside = SHOPS[trade][1], SHOPS[trade][2]
    local b = with(hut(o, 7, 5, 3, "oak_planks", "stripped_oak_log", "spruce_planks"), o, inside)
    clear_lot(b, o)
    b = with(b, o, {
        { 2, -1, 5, "cobblestone" }, { 2, 0, 5, "oak_sign|" .. name .. "|" },
        { 4, -1, 5, "cobblestone" }, { 4, 0, 5, "oak_fence" }, { 4, 1, 5, "torch" } })
    if trade == "farmers" then b = farm_and_campfire(b, o, true) end
    local r = state.town and road(o)
    if r then for _, e in ipairs(r) do b[#b + 1] = e end end
    return b, name
end

-- Where a farmer's animal pen goes on its lot: 6x6, south of the garden plot (lot x 9..14, z 8..13), clear of
-- the road. On a steep lot the road's switchback crosses that part: then there is no room for one
local function pen_spot(o)
    for dx = 9, 14 do
        for dz = 8, 13 do
            if (state.roads or {})[(o.x + dx) .. "," .. (o.z + dz)] then return nil end
        end
    end
    return { x = o.x + 9, y = o.y, z = o.z + 8 }
end

local WALLS = { "North Wall", "South Wall", "West Wall", "East Wall" }

local function village_bounds()
    local t = state.town
    local max_col, max_row = 0, 0
    local cells = {}
    for k in pairs(state.cells or {}) do
        cells[k] = true
        local c, r = k:match("^(-?%d+),(-?%d+)$")
        if c and r then max_col, max_row = math.max(max_col, tonumber(c)), math.max(max_row, tonumber(r)) end
    end
    for _, h in pairs(state.village or {}) do
        if h.at then
            local c, r = (h.at.x - t.x) // 18, (h.at.z - t.z) // 18
            cells[c .. "," .. r] = true
            max_col, max_row = math.max(max_col, c), math.max(max_row, r)
        end
    end
    local missing_shops = 0
    for trade in pairs(SHOPS) do
        local w = (state.shops or {})[trade]
        if w and w.at then
            local c, r = (w.at.x - t.x) // 18, (w.at.z - t.z) // 18
            cells[c .. "," .. r] = true
            max_col, max_row = math.max(max_col, c), math.max(max_row, r)
        else
            missing_shops = missing_shops + 1
        end
    end
    local i = 0
    while missing_shops > 0 do
        local c, r = i % COLS, i // COLS
        if not cells[c .. "," .. r] then
            cells[c .. "," .. r] = true
            max_col, max_row = math.max(max_col, c), math.max(max_row, r)
            missing_shops = missing_shops - 1
        end
        i = i + 1
    end
    return t.x - 5, t.x + (max_col + 1) * 18, t.z - 3, t.z + (max_row + 1) * 18, max_row
end

-- A perimeter wall with arched gates and torches along one side of the village border
local function village_wall(side)
    local t = state.town
    local x1, x2, z1, z2, max_row = village_bounds()
    local b = {}
    local function put(x, y, z, item) b[#b + 1] = { x, y, z, item } end
    local function column(x, z, edge_dist, gate_dist)
        local g, known = ground(x, z, t.y)
        local h = known and g or t.y
        local r = call("get_block", { bot = observer(x, z), x = x, y = h + 18, z = z, y_min = h })
        local top_name = type(r) == "table" and tostring(r.block or "") or ""
        if top_name:match("_log$") or top_name:match("_leaves$") then
            for y = (tonumber(r.y) or h), h, -1 do put(x, y, z, "air") end
        end
        for y = math.min(g - 1, h - 1), h - 1 do put(x, y, z, "cobblestone") end
        for y = h, h + 4 do put(x, y, z, "air") end
        if gate_dist and math.abs(gate_dist) <= 2 then
            if math.abs(gate_dist) <= 1 then
                put(x, h, z, "oak_fence_gate")
                put(x, h + 3, z, "cobblestone")
                put(x, h + 4, z, "cobblestone_wall")
                if gate_dist == 0 then put(x, h + 5, z, "torch") end
            else
                for y = h, h + 3 do put(x, y, z, "oak_log") end
                put(x, h + 4, z, "torch")
            end
        elseif edge_dist == 0 then
            for y = h, h + 2 do put(x, y, z, "oak_log") end
            put(x, h + 3, z, "cobblestone_wall")
            put(x, h + 4, z, "torch")
        else
            put(x, h, z, "cobblestone")
            put(x, h + 1, z, "cobblestone")
            if edge_dist % 6 == 0 then
                put(x, h + 2, z, "oak_log")
                put(x, h + 3, z, "torch")
            else
                put(x, h + 2, z, "cobblestone_wall")
            end
        end
    end
    local function gate_offset(pos, gates)
        local best
        for _, g in ipairs(gates) do
            local d = pos - g
            if not best or math.abs(d) < math.abs(best) then best = d end
        end
        return best
    end
    if side == "North Wall" or side == "South Wall" then
        local z = side == "North Wall" and z1 or z2
        local gates = { t.x - 1, (x1 + x2) // 2 }
        for x = x1, x2 do
            column(x, z, math.min(x - x1, x2 - x), gate_offset(x, gates))
        end
        return b, { x = (x1 + x2) // 2, y = t.y, z = z }
    else
        local x = side == "West Wall" and x1 or x2
        local gates = {}
        for row = 0, max_row do gates[#gates + 1] = t.z + row * 18 + 14 end
        for z = z1, z2 do
            column(x, z, math.min(z - z1, z2 - z), gate_offset(z, gates))
        end
        return b, { x = x, y = t.y, z = (z1 + z2) // 2 }
    end
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
                h.blocks, h.handed, h.started, h.homed = home(h.at, n), nil, false, nil
                note(n .. "'s house moves nearer, to " .. h.at.x .. " " .. h.at.z)
            end
        end
    end
    -- A house not built yet, planned before the house it is now: planned again
    for n, h in pairs(state.village) do
        if not h.road and not h.done and h.plan ~= HOME_PLAN then
            h.plan, h.blocks, h.handed, h.started, h.homed = HOME_PLAN, home(h.at, n), nil, false, nil
        end
    end
    -- A house not built yet, checked once against the ground seen from the sky: on a lot under water (a frozen
    -- lake) it moves to a dry lot, the wet one staying taken; planned in a cave or a rock pocket under the surface,
    -- it moves up to it
    for n, h in pairs(state.village) do
        if not h.road and not h.done and not h.surface then
            local y, found, wet = ground(h.at.x + 4, h.at.z + 4, h.at.y)
            if wet then
                h.at = lot()
                h.blocks, h.handed, h.started, h.homed = home(h.at, n), nil, false, nil
                note(n .. "'s lot is under water: its house moves to " .. h.at.x .. " " .. h.at.z)
                h.surface = true
            elseif found then
                if y ~= h.at.y then
                    note(n .. "'s house was planned at y " .. h.at.y .. ", the ground is at y " .. y .. ": it moves there")
                    h.at.y = y
                    h.blocks, h.handed, h.started, h.homed = home(h.at, n), nil, false, nil
                end
                h.surface, h.grounded = true, true
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
            h = { name = n .. "'s house", bot = n, at = at, blocks = home(at, n), plan = HOME_PLAN }
            state.village[n] = h
        end
        if h.recheck then
            h.road, h.recheck, h.name, h.blocks, h.offset = nil, nil, n .. "'s house", home(h.at, n), nil
        end
        local st = looks[n]
        if (h.road or h.done) and not h.homed then
            -- Its bed (the head, at the house's north-west corner): slept in every night from now on
            h.homed = call("home", { bot = n, x = h.at.x + 1, y = h.at.y, z = h.at.z + 1 }) ~= nil
        end
        if support[n] then
            h.handed, h.started = nil, false
        elseif h.done or (state.work and state.work.bot == n) then
            -- built, or putting up the empire's building first
        elseif not h.handed then
            hand(h)
            note(n .. " builds its house at " .. h.at.x .. " " .. h.at.z)
        elseif type(st) == "table" then
            if st.building then h.started = true end
            if completed(h, st) then
                if not h.road then
                    local blocks, err = road(h.at)
                    if blocks then
                        h.road, h.name, h.blocks, h.road_plan, h.offset = true, n .. "'s road", blocks, ROAD_PLAN, nil
                        hand(h)
                        announce(n .. " built its house at " .. h.at.x .. " " .. h.at.z .. ", now its road")
                    else
                        note(n .. " waits for its road: " .. tostring(err))
                        local x, z = tostring(err):match("not loaded at (-?%d+) (-?%d+)")
                        if x and now - (h.survey_at or 0) >= 600 then
                            h.survey_at = now
                            call("travel", { bot = n, x = tonumber(x), z = tonumber(z) })
                        end
                    end
                else
                    h.done, h.blocks, last_split = true, nil, 0       -- the blocks only weigh on the saved file now
                    note(n .. " paved its road")
                end
            elseif now - h.handed >= 600 and (not st.building or st.build_name ~= h.name) then
                hand(h)     -- the order was lost: again
            end
        end
    end
    -- A trade can open its shop as soon as one of its homeowners is free.
    state.shops = state.shops or {}
    local leader = call("leader", {})
    for trade in pairs(SHOPS) do
        local w, who = state.shops[trade], nil
        for _, n in ipairs((state.groups or {})[trade] or {}) do
            if n ~= leader and (state.village[n] or {}).done and not homeless(n) and not (state.work and state.work.bot == n) then who = n; break end
        end
        if not w and who then
            local at = lot(leader)
            local blocks, name = shop(at, trade)
            w = { name = name, bot = who, at = at, blocks = blocks }
            state.shops[trade] = w
            hand(w)
            announce(who .. " builds the " .. name .. " at " .. at.x .. " " .. at.z)
            last_split = 0
        elseif w and not w.done then
            if not looks[w.bot] and who then
                w.bot, w.handed, w.started = who, nil, false
                hand(w)
            end
            local st = looks[w.bot]
            if type(st) == "table" then
                if st.building then w.started = true end
                if completed(w, st) then
                    w.done, w.blocks, last_split = true, nil, 0
                    announce("the " .. w.name .. " is open at " .. w.at.x .. " " .. w.at.z)
                elseif now - (w.handed or 0) >= 600 and (not st.building or st.build_name ~= w.name) then
                    hand(w)
                end
            end
        end
    end
    -- Free homeowners also enclose the village border with a wall and gates on all four sides.
    if state.town then
        state.walls = state.walls or {}
        for _, side in ipairs(WALLS) do
            local w, who = state.walls[side], nil
            for _, n in ipairs(names) do
                if n ~= leader and (state.village[n] or {}).done and not homeless(n) and not support[n] and not (state.work and state.work.bot == n) then
                    who = n; break
                end
            end
            if not w and who then
                local blocks, at = village_wall(side)
                w = { name = side, bot = who, at = at, blocks = blocks }
                state.walls[side] = w
                hand(w)
                announce(who .. " builds the " .. side .. " at " .. at.x .. " " .. at.z)
                last_split = 0
            elseif w and not w.done then
                if not looks[w.bot] and who then
                    w.bot, w.handed, w.started = who, nil, false
                    hand(w)
                end
                local st = looks[w.bot]
                if type(st) == "table" then
                    if st.building then w.started = true end
                    if completed(w, st) then
                        w.done, w.blocks, last_split = true, nil, 0
                        announce("the " .. w.name .. " stands at " .. w.at.x .. " " .. w.at.z)
                    elseif now - (w.handed or 0) >= 600 and (not st.building or st.build_name ~= w.name) then
                        hand(w)
                    end
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
            if completed(w, st) then
                state.built[w.name] = (state.built[w.name] or 0) + 1
                announce(w.bot .. " built " .. (w.name == "Wonder" and "a Wonder: we win!" or ("a " .. w.name)) ..
                    " at " .. w.at.x .. " " .. w.at.z)
                state.work, last_split = nil, 0
                return
            end
            if now - (w.handed or 0) >= 600 and (not st.building or st.build_name ~= w.name) then hand(w) end     -- the order was lost: again
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
    EMPIRE_TOWN = state.town          -- where the food plugin looks for food around
    local names = {}
    for _, b in ipairs(call("bots", {}) or {}) do names[#names + 1] = b.name end
    if #names == 0 then return end
    gather(names)
    supply_crew(names)
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
    if CENTRAL_ACTIVE then return end
    if MIGRATING then return end      -- the bots are moving to a new place (migrate plugin)
    if (auto == "1" or auto == "village") and not state.enabled and os.time() - loaded_at >= 150 then start() end
    if auto == "village" and state.enabled and not state.village then state.village, last_split = {}, 0 end
    if not state.enabled or os.time() - loaded_at < 150 then return end
    play(os.time())
end)

bot.tool{
    name = "empire",
    description = "An Age of Empires II style computer player for the team, off unless started. It counts the food, wood, stone and ore the bots gather, pays for ages (Dark, Feudal, Castle, Imperial: each announced in the chat) and techs (better tools and armor, made by the blacksmiths and soldiers), has the leader decide each bot's trade by the age's split, what the next age lacks and the tools and things each bot carries (the groups tool carries it out; the brain rests meanwhile), has a builder put up Age of Empires buildings in a town east of the castle (houses, lumber camp, mill, mining camp, barracks, blacksmith, market, monastery, university, a wonder; each age needs two of the age before), village=true has every bot build a house of its own in the town first (bed, double chest, furnace, table and chairs, a sign with its name; it sleeps there every night), then a road joining it to the others, and then take a trade; once every house stands, each trade's shop goes up (bakery, farm store, butcher's, mining office, smithy) and the village border wall and gates (north, south, west, east) are built (starting play if off), and from the Castle Age sends the soldiers out together after hostile mobs every 10 minutes. With no argument: age, resources, next age's cost, the split, the last events. start=true plays (kept after a restart), stop=true stops (the groups stay, the brain works again).",
    params = { start = "boolean", stop = "boolean", village = "boolean", residence = "string", town = "object" },
    run = function(args)
        -- The bots moved away (migrate plugin): a new town there, everything built in the old one left behind
        if type(args.town) == "table" and tonumber(args.town.x) and tonumber(args.town.z) then
            state.town = { x = math.floor(args.town.x), y = math.floor(tonumber(args.town.y) or 64), z = math.floor(args.town.z) }
            state.cells, state.lots, state.built, state.work, state.shops, state.walls, state.roads = {}, 0, {}, nil, nil, nil, nil
            if state.village then state.village = {} end
            last_split = 0
            note("the town moves to " .. state.town.x .. " " .. state.town.z)
            save()
        end
        if args.residence then
            local h = state.village and state.village[args.residence]
            local ready = h and (h.road or h.done) or false
            return h and { at = h.at, ready = ready, pen = ready and pen_spot(h.at) or nil } or {}
        end
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
                for _, w in pairs(state.walls or {}) do v[w.name] = (w.done and "standing at " or (w.bot .. " building it at ")) .. w.at.x .. " " .. w.at.z end
                return v end)() }
    end,
}
