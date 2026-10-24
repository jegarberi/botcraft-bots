-- Where to settle, by vote of every bot. Each bot on the surface of the overworld looks around where it stands and
-- puts that place forward: the trees, ores, water, lava, berry bushes and crops it sees within 32 blocks, the
-- animals and hostile mobs near it, and the biome. Places within 48 blocks of each other are one. Then every bot
-- scores every place by what it cares about (its trade: miners the ores, lumberjacks the wood, farmers the food and
-- water, fishers the water, soldiers mind the monsters less) and how far it is, and votes for the best. More than
-- half the votes wins; else the two with the most votes go to a second round, which one of them wins.
--   settle {}                  the last vote
--   settle { vote = true }     look and vote now (the empire's town moves to the winner with go = true)
-- The empire calls it when its town or village has no place yet.
local MERGE = 48            -- places closer than this are one
local SURFACE = 58          -- a bot below this height stands in a cave: it votes, it puts no place forward

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil, (not ok) and tostring(r) or nil
end

local function count(r) return type(r) == "table" and #r or 0 end

-- How good a biome is to live in, 0 to 1 (unknown ones 0.5)
local BIOMES = {
    plains = 1, sunflower_plains = 1, meadow = 0.9, forest = 0.9, flower_forest = 0.9, birch_forest = 0.9, cherry_grove = 0.9,
    savanna = 0.8, taiga = 0.8, dark_forest = 0.6, old_growth_birch_forest = 0.8, river = 0.7, beach = 0.6, mushroom_fields = 0.7,
    jungle = 0.5, sparse_jungle = 0.6, bamboo_jungle = 0.5, windswept_hills = 0.5, snowy_plains = 0.3, snowy_taiga = 0.4,
    swamp = 0.3, mangrove_swamp = 0.2, desert = 0.2, badlands = 0.3, eroded_badlands = 0.2, wooded_badlands = 0.4,
    ocean = 0, deep_ocean = 0, frozen_ocean = 0, ice_spikes = 0.1, stony_peaks = 0.1, jagged_peaks = 0.1, frozen_peaks = 0.1,
}

-- What each trade cares about: weights on wood, ore, water, food, biome, monsters, lava, distance
local BASE = { wood = 1, ore = 1, water = 1, food = 1, biome = 1, hostile = -1, lava = -1, distance = -0.5 }
local TRADE = {
    miners = { ore = 2 }, blacksmiths = { ore = 2 }, lumberjacks = { wood = 2 }, carpenters = { wood = 2 }, builders = { wood = 1.5, ore = 1.5 },
    farmers = { food = 2, water = 1.5 }, bakers = { food = 2 }, butchers = { food = 2 }, fishers = { water = 2.5 },
    soldiers = { hostile = -0.5 }, explorers = { distance = 0 },
}

local last = nil

local function both(name, id)   -- an ore and its deepslate kind
    return count(call("find_blocks", { bot = name, block = id, radius = 32, max = 64 })) +
        count(call("find_blocks", { bot = name, block = "_" .. id, radius = 32, max = 64 }))
end

-- What a bot sees where it stands, each between 0 and 1; nil for a bot in a cave or another dimension
local function survey(name, st)
    local p = st.position
    if st.dimension ~= "minecraft:overworld" or p.y < SURFACE then return nil end
    local logs = count(call("find_blocks", { bot = name, block = "_log", radius = 32, max = 64 }))
    local ores = count(call("find_blocks", { bot = name, block = "_ore", radius = 32, max = 64 }))
    local value = ores + 2 * both(name, "iron_ore") + 6 * both(name, "diamond_ore")
    local water = count(call("find_blocks", { bot = name, block = "water", radius = 32, max = 20 }))
    local lava = count(call("find_blocks", { bot = name, block = "lava", radius = 32, max = 5 }))
    local food = count(call("find_blocks", { bot = name, block = "sweet_berry_bush", radius = 32, max = 20 })) +
        count(call("find_blocks", { bot = name, block = "wheat", radius = 32, max = 20 })) +
        2 * count(call("entities", { bot = name, type = "animal", radius = 32 }))
    local hostile = count(call("entities", { bot = name, type = "hostile", radius = 32 }))
    return {
        x = math.floor(p.x), y = math.floor(p.y), z = math.floor(p.z), biome_name = st.biome or "",
        wood = math.min(logs, 40) / 40, ore = math.min(value, 40) / 40, water = math.min(water, 20) / 20,
        food = math.min(food, 20) / 20, biome = BIOMES[(st.biome or ""):gsub("^minecraft:", "")] or 0.5,
        hostile = math.min(hostile, 8) / 8, lava = lava > 0 and 1 or 0,
    }
end

-- Places within MERGE blocks are one: what was seen there averaged, at the spot of the first that put it forward
local function merge(places)
    local out = {}
    for _, p in ipairs(places) do
        local into = nil
        for _, o in ipairs(out) do
            if (o.x - p.x) ^ 2 + (o.z - p.z) ^ 2 < MERGE * MERGE then into = o break end
        end
        if into then
            into.n = into.n + 1
            for _, k in ipairs({ "wood", "ore", "water", "food", "biome", "hostile", "lava" }) do
                into[k] = into[k] + (p[k] - into[k]) / into.n
            end
            into.by[#into.by + 1] = p.by
        else
            p.n, p.by = 1, { p.by }
            out[#out + 1] = p
        end
    end
    return out
end

-- How much a bot of this trade, standing at `at`, likes a place
local function score(trade, at, place)
    local w = setmetatable(TRADE[trade] or {}, { __index = BASE })
    local d = math.sqrt((place.x - at.x) ^ 2 + (place.z - at.z) ^ 2)
    return w.wood * place.wood + w.ore * place.ore + w.water * place.water + w.food * place.food + w.biome * place.biome +
        w.hostile * place.hostile + w.lava * place.lava + w.distance * math.min(d, 500) / 500
end

local function vote()
    local voters, places = {}, {}
    for _, b in ipairs(call("bots", {}) or {}) do
        local st = call("status", { bot = b.name })
        if type(st) == "table" and type(st.position) == "table" then
            voters[#voters + 1] = { name = b.name, trade = st.trade, at = st.position }
            local s = survey(b.name, st)
            if s then s.by = b.name; places[#places + 1] = s end
        end
    end
    if #voters == 0 then return nil, "no bot in the world" end
    places = merge(places)
    if #places == 0 then return nil, "no bot stands on the surface to put a place forward" end
    for i, p in ipairs(places) do p.id = i end
    local scores = {}
    for _, v in ipairs(voters) do
        scores[v.name] = {}
        for _, p in ipairs(places) do scores[v.name][p.id] = score(v.trade, v.at, p) end
    end
    -- Each votes for its best among `among`; returns the tally and the leader
    local function round(among)
        local tally = {}
        for _, p in ipairs(among) do tally[p.id] = 0 end
        for _, v in ipairs(voters) do
            local best = among[1]
            for _, p in ipairs(among) do if scores[v.name][p.id] > scores[v.name][best.id] then best = p end end
            tally[best.id] = tally[best.id] + 1
        end
        local ranked = {}
        for _, p in ipairs(among) do ranked[#ranked + 1] = p end
        table.sort(ranked, function(a, b) return tally[a.id] > tally[b.id] or (tally[a.id] == tally[b.id] and a.id < b.id) end)
        return tally, ranked
    end
    local tally, ranked = round(places)
    local rounds = { {} }
    for _, p in ipairs(ranked) do rounds[1][#rounds[1] + 1] = { at = p.x .. " " .. p.z, votes = tally[p.id] } end
    local winner = ranked[1]
    if tally[winner.id] * 2 <= #voters and ranked[2] then
        -- No majority: the two with the most votes, again (a tie goes to the first put forward)
        local t2, r2 = round({ ranked[1], ranked[2] })
        rounds[2] = { { at = r2[1].x .. " " .. r2[1].z, votes = t2[r2[1].id] }, { at = r2[2].x .. " " .. r2[2].z, votes = t2[r2[2].id] } }
        winner = r2[1]
    end
    last = {
        at = os.date("%H:%M"), voters = #voters, places = #places, rounds = rounds,
        winner = { x = winner.x, y = winner.y, z = winner.z, biome = winner.biome_name, put_forward_by = winner.by,
            seen = { wood = winner.wood, ore = winner.ore, water = winner.water, food = winner.food, biome = winner.biome,
                hostile = winner.hostile, lava = winner.lava } },
    }
    local bots = call("bots", {}) or {}
    if bots[1] then
        call("chat", { bot = bots[1].name, message = string.format("[settle] %d bots voted on %d places: we settle at %d %d (%s)",
            #voters, #places, winner.x, winner.z, winner.biome_name) })
    end
    return last
end

bot.tool{
    name = "settle",
    description = "Where the bots settle, by vote of all of them. Each bot on the surface puts forward the place where it stands, with what it sees within 32 blocks: trees, ores (iron and diamonds count more), water, lava, berry bushes and wheat, animals and hostile mobs, and the biome; places within 48 blocks are one. Every bot scores every place by its trade (miners: ores; lumberjacks and carpenters: wood; farmers, bakers, butchers: food and water; fishers: water; soldiers mind monsters less; explorers don't mind the distance) and votes for the best; more than half the votes wins, else the top two go to a second round. With no argument: the last vote. vote=true: vote now; go=true too: the empire's town (and village) moves to the winner. The empire calls it by itself when its town has no place yet.",
    params = { vote = "boolean", go = "boolean" },
    run = function(args)
        if not args.vote then return last or "no vote yet" end
        local r, err = vote()
        if not r then error(err) end
        if args.go then call("empire", { town = { x = r.winner.x, y = r.winner.y, z = r.winner.z } }) end
        return r
    end,
}
