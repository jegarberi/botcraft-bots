-- An Age of Empires II style AI, optional (off until started): it plays the team like a computer player.
--   resources  food, wood, stone and ore: what the bots gathered (their inventories rising, looked at every minute),
--              spent on ages and techs
--   ages       Dark -> Feudal -> Castle -> Imperial: each one paid for, announced in the chat, and with its own
--              split of the bots ("villagers") over the trades, shifted toward the resource the next age lacks most
--   techs      paid for once their age is reached: better tools and armor, made by the crafters and soldiers
--   the leader decides every bot's trade (the first bot leads if no one does): the age's split, filled with the bots
--              whose tools and things fit each trade best, the moves told in the chat
--   attacks    from the Castle Age, every 10 minutes the soldiers go out together after hostile mobs
-- It decides the groups (the groups plugin carries them out, each trade's network still picks what a bot does), so the
-- brain rests while it plays and works again when it stops.
--   empire {}                   age, resources, what the next age costs, the split, what happened
--   empire { start = true }     play (taken up again after a restart)
--   empire { stop = true }      stop: the groups stay, the brain works again
local FILE = "../empire_job.lua"
local TRADES = { "cooks", "farmers", "breeders", "miners", "builders", "soldiers", "crafters", "explorers" }
local RESOURCES = { "food", "wood", "stone", "ore" }
-- Each age: its cost (to reach it) and how many bots of every 15 work in each trade
local AGES = {
    { name = "Dark Age", cost = {},
      mix = { farmers = 3, cooks = 2, breeders = 2, builders = 4, miners = 2, crafters = 1, explorers = 1, soldiers = 0 } },
    { name = "Feudal Age", cost = { food = 128, wood = 128 },
      mix = { farmers = 3, cooks = 2, breeders = 2, builders = 3, miners = 3, crafters = 1, explorers = 0, soldiers = 1 } },
    { name = "Castle Age", cost = { food = 192, stone = 256, ore = 64 },
      mix = { farmers = 2, cooks = 2, breeders = 2, builders = 3, miners = 3, crafters = 1, explorers = 0, soldiers = 2 } },
    { name = "Imperial Age", cost = { food = 256, stone = 256, ore = 192 },
      mix = { farmers = 2, cooks = 2, breeders = 1, builders = 3, miners = 2, crafters = 1, explorers = 1, soldiers = 3 } },
}
-- The trades that bring in each resource: the one the next age lacks most gets 2 more bots' weight in each
local NEED = { food = { "farmers", "cooks" }, wood = { "builders", "explorers" }, stone = { "builders", "miners" }, ore = { "miners" } }
-- who: the trade that makes it; item and count: what each of them is told to make
local TECHS = {
    { name = "Stone Tools", age = 2, cost = { wood = 32, stone = 48 }, who = "crafters", item = "stone_pickaxe", count = 4 },
    { name = "Bloomery", age = 2, cost = { wood = 48, ore = 16 }, who = "crafters", item = "iron_ingot", count = 16 },
    { name = "Iron Tools", age = 3, cost = { ore = 48 }, who = "crafters", item = "iron_pickaxe", count = 3 },
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

local state = { enabled = false, age = 1, stock = { food = 0, wood = 0, stone = 0, ore = 0 }, techs = {}, groups = nil, log = {} }
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
    farmers = { "_hoe", "wheat_seeds", "wheat", "potato" }, breeders = { "carrot", "shears", "wool" },
    cooks = { "beef", "porkchop", "mutton", "chicken", "bread" }, builders = { "cobblestone", "_planks", "_log" },
    crafters = { "iron_ingot", "stick", "crafting_table", "furnace" }, explorers = { "boat", "map" },
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
-- in its trade counts for 1: a move costs the work under way). It is a builder itself, the berry picker a farmer.
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

-- One look a minute: the rules, in order, like an AI script's
local function play(now)
    local names = {}
    for _, b in ipairs(call("bots", {}) or {}) do names[#names + 1] = b.name end
    if #names == 0 then return end
    gather(names)
    local next_age = AGES[state.age + 1]
    if next_age and affords(next_age.cost) then
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
        local g, leader = split(names)
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

bot.every(60, function()
    if not state.enabled or os.time() - loaded_at < 150 then return end
    play(os.time())
end)

bot.tool{
    name = "empire",
    description = "An Age of Empires II style computer player for the team, off unless started. It counts the food, wood, stone and ore the bots gather, pays for ages (Dark, Feudal, Castle, Imperial: each announced in the chat) and techs (better tools and armor, made by the crafters and soldiers), splits the bots over the trades by the age and what the next age lacks (the groups tool carries it out; the brain rests meanwhile), and from the Castle Age sends the soldiers out together after hostile mobs every 10 minutes. With no argument: age, resources, next age's cost, the split, the last events. start=true plays (kept after a restart), stop=true stops (the groups stay, the brain works again).",
    params = { start = "boolean", stop = "boolean" },
    run = function(args)
        if args.start and not state.enabled then
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
            next_age = next_age and { name = next_age.name, cost = next_age.cost } or "none: Imperial is the last",
            techs = techs, groups = state.groups, events = state.log }
    end,
}
