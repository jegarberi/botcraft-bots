-- Food for everyone: enough bots in the food trades to keep up with what the team eats, put where the food comes
-- fastest around the town, and the hungry sent to them.
--   every minute   what the team eats: the drop of every bot's food bar (a starving bot adds what it lacks)
--   every 10 min   what there is around the town, from the bots nearest it: animals, sweet berry bushes, open water,
--                  farmland and crops, fishing rods and string
--   the plan       food points a worker brings in a minute in each food trade (fishers: fish, cooked; butchers:
--                  animals, bred and slaughtered; farmers: crops and berries; bakers turn the farmers' wheat into
--                  bread, one for three farmers), workers added where the next one brings most, until the team's
--                  eating is covered (60% of the bots at most). FOOD_PLAN: the empire splits the bots with it
--   the hungry     a bot at 6 food or less with nothing to eat walks to the bot carrying most food, which hands it
--                  16 (once in 3 minutes for each bot, one at a time for each giver); a bot carrying more than 64
--                  shares it out, down to 32, with the bots carrying little
--   food {}        the plan: what the team eats, what each trade brings in, how many in each, the last feeds
local EDIBLE = NN and NN.edible or {}
local SURVEY_EVERY, LOOK_EVERY = 600, 60
local MAX_SHARE = 0.6                -- of the bots, in the food trades at most
local DIMINISH = 0.85                -- each more worker on the same source brings this much of the one before

local state = {
    eaten_per_min = nil,             -- food points the team eats a minute (running average)
    last_food = {},                  -- bot -> its food bar at the last look
    survey = nil, surveyed_at = 0,
    rates = {}, plan = {}, need = 0,
    fed_at = {}, feeds = {},
}
local looked_at = 0

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil
end

local function edible_count(st)
    local n = 0
    for item, count in pairs(type(st.inventory) == "table" and st.inventory or {}) do
        if EDIBLE[item] then n = n + (tonumber(count) or 0) end
    end
    return n
end

local function statuses()
    local list = {}
    for _, b in ipairs(call("bots", {}) or {}) do
        local name = type(b) == "table" and b.name or b
        local st = type(name) == "string" and call("status", { bot = name }) or nil
        if type(st) == "table" and st.name and type(st.position) == "table" then list[#list + 1] = st end
    end
    return list
end

-- What the bots nearest the town (or the leader, with no town) see around them
local function survey(list)
    local center
    local town = EMPIRE_TOWN
    if type(town) == "table" and town.x then center = town end
    if not center then
        local leader = call("leader", {})
        for _, st in ipairs(list) do if st.name == leader then center = st.position end end
    end
    center = center or (list[1] and list[1].position)
    if not center then return nil end
    table.sort(list, function(a, b)
        local da = (a.position.x - center.x) ^ 2 + (a.position.z - center.z) ^ 2
        local db = (b.position.x - center.x) ^ 2 + (b.position.z - center.z) ^ 2
        return da < db
    end)
    local s = { animals = 0, bushes = 0, water = 0, fields = 0, rods = 0, biomes = {} }
    for i = 1, math.min(3, #list) do
        local who = list[i].name
        local animals = call("entities", { bot = who, type = "animal" }) or {}
        local n = 0
        for _, e in ipairs(animals) do
            local kind = tostring(e.type or e.name or "")
            if kind:match("cow") or kind:match("pig") or kind:match("sheep") or kind:match("chicken") or kind:match("rabbit") then n = n + 1 end
        end
        local function count(block) local r = call("find_blocks", { bot = who, block = block, radius = 32, max = 64 }); return type(r) == "table" and #r or 0 end
        s.animals = math.max(s.animals, n)
        s.bushes = math.max(s.bushes, count("sweet_berry_bush"))
        s.water = math.max(s.water, count("water"))
        s.fields = math.max(s.fields, count("farmland") + count("wheat") + count("carrots") + count("potatoes"))
        s.biomes[#s.biomes + 1] = list[i].biome
    end
    for _, st in ipairs(list) do
        local inv = type(st.inventory) == "table" and st.inventory or {}
        s.rods = s.rods + (tonumber(inv.fishing_rod) or 0) + (tonumber(inv.string) or 0) // 2
    end
    return s
end

-- Food points a worker brings in a minute in each trade, and how many of them the place can keep busy.
-- ponytail: rough figures (a cooked fish is 5, a cow 2 cooked beef of 8, bread 5 from 3 wheat, a berry 2), not
-- measured per trade; measure what each trade actually brings in if the split comes out wrong
local function rates(s)
    local r, cap = {}, {}
    local open_water = s.water >= 16
    r.fishers = open_water and (s.rods > 0 and 6 or 2) or 0           -- with no rod, string from spiders first
    cap.fishers = open_water and math.min(8, s.water // 8) or 0
    r.butchers = s.animals >= 4 and 8 or s.animals * 1.5
    cap.butchers = math.max(0, math.ceil(s.animals / 6))
    r.farmers = 2 + math.min(4, s.bushes * 0.25) + math.min(4, s.fields / 20)
    cap.farmers = 40
    -- Nothing seen (no bot near the town): what the biome usually has
    if s.animals + s.bushes + s.water + s.fields == 0 then
        for _, biome in ipairs(s.biomes) do
            biome = tostring(biome)
            if biome:match("ocean") or biome:match("river") or biome:match("beach") or biome:match("swamp") then r.fishers, cap.fishers = 4, 6 end
            if biome:match("plains") or biome:match("savanna") or biome:match("meadow") or biome:match("forest") then r.butchers, cap.butchers = 4, 4 end
            if biome:match("taiga") then r.farmers = math.max(r.farmers, 4) end
        end
    end
    return r, cap
end

-- Workers, one at a time where the next brings most, until the eating is covered
local function plan(need, r, cap, most)
    local p, made = { fishers = 0, butchers = 0, farmers = 0, bakers = 0 }, 0
    while made < need and p.fishers + p.butchers + p.farmers + p.bakers < most do
        local best, gain = nil, 0
        for _, t in ipairs({ "fishers", "butchers", "farmers" }) do
            local g = (r[t] or 0) * DIMINISH ^ p[t]
            if p[t] < (cap[t] or 0) and g > gain then best, gain = t, g end
        end
        if not best then break end
        p[best] = p[best] + 1
        made = made + gain
        -- Bread from the farmers' wheat: a baker for three farmers
        if best == "farmers" and p.bakers < math.ceil(p.farmers / 3) and p.fishers + p.butchers + p.farmers + p.bakers < most then
            p.bakers = p.bakers + 1
        end
    end
    return p, made
end

-- The hungry with nothing to eat go to the bot carrying most food, which gives them some. A bot carrying more
-- than RICH shares it out (down to KEEP) with the bots carrying little, hungry first: each of them can feed others
local GIVE, RICH, KEEP = 16, 64, 32
local function feed(list, now)
    local donors, stock = {}, {}
    for _, st in ipairs(list) do
        stock[st.name] = edible_count(st)
        if stock[st.name] >= 8 and (tonumber(st.food) or 20) > 10 then donors[#donors + 1] = st end
    end
    table.sort(donors, function(a, b) return stock[a.name] > stock[b.name] end)
    local rich = donors[1] ~= nil and stock[donors[1].name] > RICH
    local takers = {}
    for _, st in ipairs(list) do
        local hungry = (tonumber(st.food) or 20) <= 6 and stock[st.name] == 0
        if hungry or (rich and stock[st.name] < 8) then takers[#takers + 1] = { st = st, hungry = hungry } end
    end
    table.sort(takers, function(a, b) return (tonumber(a.st.food) or 20) < (tonumber(b.st.food) or 20) end)
    -- Each giver gets a round of takers and walks it in one go (a giver still on its last round is left out)
    local rounds, order, sent = {}, {}, 0
    for _, t in ipairs(takers) do
        local st = t.st
        if sent >= 20 then break end
        if now - (state.fed_at[st.name] or 0) >= 180 then
            for _, d in ipairs(donors) do
                -- The rich give to anyone short; the others only to the hungry
                local can = t.hungry or stock[d.name] - GIVE >= KEEP
                if d.name ~= st.name and can and stock[d.name] >= GIVE and not bot.busy("feed:" .. d.name) then
                    state.fed_at[st.name] = now
                    stock[d.name] = stock[d.name] - GIVE
                    sent = sent + 1
                    if not rounds[d.name] then rounds[d.name] = {}; order[#order + 1] = d end
                    table.insert(rounds[d.name], t)
                    break
                end
            end
        end
    end
    for _, d in ipairs(order) do
        local from, at, round = d.name, d.position, rounds[d.name]
        -- The hungry walk toward the giver meanwhile (it comes to them too); the others just wait for it
        for _, t in ipairs(round) do
            if t.hungry then
                local to = t.st.name
                bot.spawn("walk:" .. to, function()
                    call("goto", { bot = to, x = math.floor(at.x), y = math.floor(at.y), z = math.floor(at.z), tolerance = 3 })
                end)
            end
        end
        bot.spawn("feed:" .. from, function()
            for _, t in ipairs(round) do
                local to = t.st.name
                local ok = call("give", { bot = from, to = to, item = "food", count = GIVE }) ~= nil
                table.insert(state.feeds, 1, os.date("%H:%M ") .. from .. (ok and " fed " or " could not feed ") .. to)
                while #state.feeds > 20 do table.remove(state.feeds) end
            end
        end)
    end
end

local function look()
    local now = os.time()
    local list = statuses()
    if #list == 0 then return end
    -- What the team ate since the last look: food bars that went down (eating fills them back up)
    local dropped, lacking = 0, 0
    for _, st in ipairs(list) do
        local food = tonumber(st.food) or 20
        local before = state.last_food[st.name]
        if before and food < before then dropped = dropped + (before - food) end
        state.last_food[st.name] = food
        lacking = lacking + math.max(0, 10 - food)
    end
    local minutes = looked_at > 0 and math.max(1, (now - looked_at) / 60) or nil
    if minutes then
        local rate = dropped / minutes
        state.eaten_per_min = state.eaten_per_min and (0.8 * state.eaten_per_min + 0.2 * rate) or math.max(rate, 0.5 * #list)
    end
    looked_at = now
    if not state.survey or now - state.surveyed_at >= SURVEY_EVERY then
        state.survey, state.surveyed_at = survey(list), now
    end
    if state.survey then
        local r, cap = rates(state.survey)
        -- Eating covered with a quarter to spare, and those starving back to 10 within a quarter of an hour
        state.need = (state.eaten_per_min or 0.5 * #list) * 1.25 + lacking / 15
        state.rates = r
        state.plan = plan(state.need, r, cap, math.floor(MAX_SHARE * #list))
        FOOD_PLAN = state.plan
    end
    feed(list, now)
end

bot.every(LOOK_EVERY, function()
    if MIGRATING then return end
    bot.spawn("food", look)       -- on its own thread: a status for every bot takes a while
end)

bot.tool{
    name = "food",
    description = "Food for everyone: every minute, what the team eats (the drop of every bot's food bar), what each food trade brings in a minute around the town (fishers with open water and rods, butchers with the animals there, farmers with crops and berries, a baker for three farmers), and how many bots each trade needs to keep up (60% of the bots at most); the empire splits the bots with it. The hungry with nothing to eat walk to the bot carrying most food, which hands them some. With no argument: what the team eats, the plan, the rates, what was seen around the town, the last feeds.",
    params = {},
    run = function()
        return { eaten_per_min = state.eaten_per_min, need_per_min = state.need, plan = state.plan, rates = state.rates,
            seen = state.survey, feeds = state.feeds }
    end,
}

FOOD = { rates = rates, plan = plan, feed = feed, state = state }     -- for the tests
