-- Each trade's own neural network: within its trade (the brain plugin picks the trade), what a bot does next.
-- One network per trade (bakers, farmers, butchers, miners, builders, soldiers, blacksmiths, explorers, fishers), shared by
-- the bots in it, so it learns from all of them. Same network code as the brain (the global NN it defines).
--   bakers       bread, pumpkin pies and cakes from the farmers' wheat, eggs, milk and pumpkins; feed the hungry
--   farmers      wheat fields, eggs from the chickens, pumpkins, milk from the cows
--   butchers     breed the animals, slaughter some: meat (cooked) and leather
--   miners       ores: coal and iron for the blacksmiths
--   blacksmiths  smelt the iron, make tools, buckets, torches and armor (iron, or leather from the butchers)
--   fishers      fish with a rod at open water, cook the catch (coal from the miners), feed the hungry
-- The trades trade: each needs things the others make (NEEDS, PROVIDES). Its "trade" action asks a bot of the
-- trade that makes what it lacks to hand it over, or else hands its own surplus to the bot that lacks it most.
--   in   15 numbers about the bot: health, hunger, food, wood, stone, iron, coal it carries, its tools, night,
--        hostile mobs and animals near, how much of what its trade needs it lacks
--   ->   8 hidden units (tanh)
--   out  the trade's actions (softmax: drawn from, so it keeps trying the others)
-- Each time the groups plugin gives a bot its turn, the bot's network draws an action and the bot does it;
-- at its next turn, what it achieved since (its stats, weighted for the trade, minus deaths and getting
-- stuck) is the reward the network learns from (actor-critic).
--   roles {}                                   each trade's network: actions, how often, last rewards
--   roles { act = "Bot3", role = "miners" }    the bot's turn (the groups plugin)
--   roles { network = "miners", bot = "Bot3" } the network, and what it makes of the bot now (the panels)
local FILE = "../roles_state.lua"
local HID = 8
local INPUTS = { "bias", "health", "food level", "food carried", "wood carried", "stone carried", "iron carried", "coal carried",
    "has pickaxe", "has sword", "has hoe", "night", "hostiles near", "animals near", "needs unmet" }
local ORES = { coal = "coal_ore", iron = "iron_ore", copper_gold = "copper_ore,gold_ore", deep = "redstone_ore,lapis_ore,diamond_ore" }
local FOOD = { bread = true, pumpkin_pie = true, cake = true, cooked_beef = true, cooked_porkchop = true, cooked_mutton = true, cooked_chicken = true, baked_potato = true,
    carrot = true, apple = true, golden_carrot = true, sweet_berries = true, cooked_cod = true, cooked_salmon = true }
local RAW = { beef = "cooked_beef", porkchop = "cooked_porkchop", mutton = "cooked_mutton", chicken = "cooked_chicken", potato = "baked_potato",
    cod = "cooked_cod", salmon = "cooked_salmon" }

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil, (not ok) and tostring(r) or nil
end

local function inv(st) return type(st.inventory) == "table" and st.inventory or {} end
local function carried(st, test)
    local n = 0
    for item, c in pairs(inv(st)) do if test(item) then n = n + (tonumber(c) or 0) end end
    return n
end
local function has(st, suffix) return carried(st, function(i) return i:sub(-#suffix) == suffix end) > 0 end

-- The hungriest other bot (food 14 or less), and the bots lacking a tool
local function team(ctx)
    if ctx.team then return ctx.team end
    ctx.team = {}
    for _, b in ipairs(call("bots", {}) or {}) do
        if b.name ~= ctx.bot then
            local st = call("status", { bot = b.name })
            if type(st) == "table" and st.name then ctx.team[#ctx.team + 1] = st end
        end
    end
    return ctx.team
end

local function walk_off(ctx, distance)
    local p = ctx.st.position
    if not p then return "nowhere to start from" end
    local a = math.random() * 2 * math.pi
    local x, z = math.floor(p.x + distance * math.cos(a)), math.floor(p.z + distance * math.sin(a))
    local _, err = call("travel", { bot = ctx.bot, x = x, z = z })
    return err and ("could not walk off: " .. err) or ("walked to " .. x .. " " .. z)
end

local function make(ctx, item, count)
    local _, err = call("make", { bot = ctx.bot, item = item, count = count })
    return err and ("can't make " .. item .. ": " .. err) or ("making " .. count .. " " .. item)
end

local function gather(ctx, blocks, count)
    local _, err = call("autopilot", { bot = ctx.bot, enabled = true, gather = blocks, count = count })
    return err and ("can't gather: " .. err) or ("gathering " .. blocks)
end

local function store(ctx)
    local _, err = call("store", { bot = ctx.bot })
    return err and ("could not store: " .. err) or "stored its things"
end

local function give(ctx, to, item, count)
    if not to then return "nobody to give to" end
    local _, err = call("give", { bot = ctx.bot, to = to, item = item, count = count })
    return err and ("could not give " .. item .. " to " .. to .. ": " .. err) or ("gave " .. item:gsub("minecraft:", "") .. " to " .. to)
end

local function mine(ctx, ores)
    local members = ctx.members and #ctx.members > 0 and ctx.members or { ctx.bot }
    local _, err = call("team_member", { bot = ctx.bot, objective = "gather", block = ores, members = members, leader = members[1], count = 64, solo = true })
    return err and ("can't mine: " .. err) or ("mining " .. ores)
end

-- ---------------------------------------------------------------- the exchange between trades
-- What a bot of each trade wants to carry to do its work, and which trade makes each of those things
local NEEDS = {
    bakers = { wheat = 9, egg = 2, milk_bucket = 3, pumpkin = 2 },
    farmers = { bucket = 1, bread = 4 },
    butchers = { wheat = 8, wheat_seeds = 8, carrot = 4, bread = 4 },
    miners = { iron_pickaxe = 1, torch = 16, bread = 4 },
    blacksmiths = { raw_iron = 9, coal = 8, leather = 4, bread = 4 },
    fishers = { coal = 4, bread = 2 },
}
local PROVIDES = { wheat = "farmers", egg = "farmers", milk_bucket = "farmers", pumpkin = "farmers", wheat_seeds = "farmers",
    carrot = "farmers", bread = "bakers", leather = "butchers", raw_iron = "miners", coal = "miners", iron_pickaxe = "blacksmiths",
    bucket = "blacksmiths", torch = "blacksmiths", cooked_cod = "fishers", cooked_salmon = "fishers" }
local function count(st, item) return carried(st, function(i) return i == item end) end
local function sorted(t) local k = {} for x in pairs(t) do k[#k + 1] = x end table.sort(k) return k end

-- How much of what its trade needs the bot lacks: 0 has it all, 1 has none of it
local function unmet(st, role)
    local needs, lack, n = NEEDS[role] or {}, 0, 0
    for item, want in pairs(needs) do lack, n = lack + math.max(0, want - count(st, item)) / want, n + 1 end
    return n > 0 and lack / n or 0
end

local function status(name)
    local st = call("status", { bot = name })
    return type(st) == "table" and st.name and st or nil
end

local function exchange(ctx)
    local needs = NEEDS[ctx.role] or {}
    -- What it lacks, from a bot of the trade that makes it and has more than its own trade keeps
    for _, item in ipairs(sorted(needs)) do
        local lack = needs[item] - count(ctx.st, item)
        local from = PROVIDES[item]
        if lack > 0 and from ~= ctx.role then
            for _, b in ipairs(ctx.groups and ctx.groups[from] or {}) do
                local st = status(b)
                local spare = st and count(st, item) - ((NEEDS[from] or {})[item] or 0) or 0
                if spare > 0 then
                    local _, err = call("give", { bot = b, to = ctx.bot, item = "minecraft:" .. item, count = math.min(spare, lack) })
                    return err and ("asked " .. b .. " for " .. item .. ": " .. err) or ("got " .. item .. " from " .. b)
                end
            end
        end
    end
    -- What it makes beyond its own needs, to the bot of another trade lacking it most
    for _, item in ipairs(sorted(PROVIDES)) do
        local spare = PROVIDES[item] == ctx.role and count(ctx.st, item) - (needs[item] or 0) or 0
        if spare > 0 then
            local who, most = nil, 0
            for _, trade in ipairs(sorted(NEEDS)) do
                local want = trade ~= ctx.role and NEEDS[trade][item]
                for _, b in ipairs(want and ctx.groups and ctx.groups[trade] or {}) do
                    local st = status(b)
                    local lack = st and want - count(st, item) or 0
                    if lack > most then who, most = b, lack end
                end
            end
            if who then return give(ctx, who, "minecraft:" .. item, math.min(spare, most)) end
        end
    end
    return "nothing to trade"
end

local function hand_out(ctx, kinds)
    for _, kind in ipairs(kinds) do
        for item, c in pairs(inv(ctx.st)) do
            if item:sub(-#kind) == kind and c >= 1 then
                for _, st in ipairs(team(ctx)) do
                    if not has(st, kind) then return give(ctx, st.name, "minecraft:" .. item, 1) end
                end
            end
        end
    end
    return "everyone has its " .. table.concat(kinds, " "):gsub("_", "")
end

local function butcher(ctx)
    local _, err = call("kill", { bot = ctx.bot, type = ({ "cow", "pig", "sheep", "chicken" })[math.random(4)], count = 1 })
    return err and ("slaughter: " .. err) or "slaughtered an animal"
end

-- ---------------------------------------------------------------- the trades: actions and what counts
-- goal = true: the action sets a goal the bot works toward on its own; drawn again, it is left running
local ROLES = {
    bakers = {
        weights = { given = 2, made = 1, needs = 4 },
        actions = {
            { name = "bake bread", run = function(ctx)
                if count(ctx.st, "wheat") < 3 then return "no wheat: the farmers have it" end
                return make(ctx, "bread", count(ctx.st, "wheat") // 3 + count(ctx.st, "bread")) end },
            { name = "bake pumpkin pie", run = function(ctx)
                if count(ctx.st, "pumpkin") == 0 or count(ctx.st, "egg") == 0 then return "no pumpkin or egg: the farmers have them" end
                return make(ctx, "pumpkin_pie", count(ctx.st, "pumpkin_pie") + 1) end },
            { name = "bake a cake", run = function(ctx)
                if count(ctx.st, "milk_bucket") < 3 or count(ctx.st, "egg") == 0 or count(ctx.st, "wheat") < 3 then
                    return "a cake needs 3 milk, an egg and 3 wheat" end
                return make(ctx, "cake", count(ctx.st, "cake") + 1) end },
            { name = "feed the hungriest", run = function(ctx)
                local who, low = nil, 15
                for _, st in ipairs(team(ctx)) do if (st.food or 20) < low then who, low = st.name, st.food end end
                if not who then return "nobody is hungry" end
                if carried(ctx.st, function(i) return FOOD[i] end) == 0 then return "no food to give" end
                return give(ctx, who, "food", 8) end },
            { name = "trade", run = exchange },
            { name = "store", run = store },
        },
    },
    farmers = {
        weights = { harvested = 1, given = 1, collected = 0.2, needs = 4 },
        actions = {
            { name = "harvest", run = function(ctx)
                local r = call("farm", { bot = ctx.bot, radius = 16 })
                return r and ("farm: " .. (r.harvested or 0) .. " harvested, " .. (r.replanted or 0) .. " replanted") or "nothing ripe" end },
            { name = "make a farm", run = function(ctx)
                if not has(ctx.st, "_hoe") then return make(ctx, "stone_hoe", 1) end
                local r, err = call("make_farm", { bot = ctx.bot })
                if r then return "made a farm: " .. (r.tilled or 0) .. " tilled" end
                if tostring(err):match("no water") then return "no water near: " .. walk_off(ctx, 24) end
                return "could not make a farm: " .. tostring(err) end },
            { name = "collect eggs", run = function(ctx)
                -- The chickens lay them where they stand: a walk among them picks them up
                local before = count(ctx.st, "egg")
                local list = call("entities", { bot = ctx.bot, type = "chicken" }) or {}
                for i = 1, math.min(4, #list) do
                    local p = list[i].position
                    if p then call("goto", { bot = ctx.bot, x = math.floor(p.x), y = math.floor(p.y), z = math.floor(p.z), tolerance = 1 }) end
                end
                if #list == 0 then return "no chickens near" end
                local st = status(ctx.bot)
                return "walked among " .. math.min(4, #list) .. " chickens: " .. ((st and count(st, "egg") or before) - before) .. " eggs" end },
            { name = "milk cows", run = function(ctx)
                local r, err = call("milk", { bot = ctx.bot, count = 3 })
                return r and ("milked " .. (r.milked or 0) .. " cows") or ("could not milk: " .. tostring(err)) end },
            { name = "gather pumpkins", run = function(ctx)
                local _, err = call("collect", { bot = ctx.bot, block = "pumpkin", count = 4 })
                return err and ("no pumpkins: " .. err) or "gathered pumpkins" end },
            { name = "trade", run = exchange },
            { name = "store", run = store },
        },
    },
    butchers = {
        weights = { bred = 5, killed = 2, made = 0.5, given = 0.5, needs = 4 },
        actions = {},
    },
    miners = {
        weights = { collected = 1, stored = 0.05, needs = 4 },
        actions = {
            { name = "mine coal", goal = true, run = function(ctx) return mine(ctx, ORES.coal) end },
            { name = "mine iron", goal = true, run = function(ctx) return mine(ctx, ORES.iron) end },
            { name = "mine copper and gold", goal = true, run = function(ctx) return mine(ctx, ORES.copper_gold) end },
            { name = "mine deep ores", goal = true, run = function(ctx) return mine(ctx, ORES.deep) end },
            { name = "trade", run = exchange },
            { name = "store", run = store },
        },
    },
    builders = {
        weights = { built = 1, collected = 0.2 },
        actions = {
            { name = "build the castle", run = function() return "building the castle" end },
            { name = "fetch stone", goal = true, run = function(ctx) return make(ctx, "cobblestone", 64) end },
            { name = "fetch wood", goal = true, run = function(ctx) return make(ctx, "oak_planks", 64) end },
            { name = "store", run = store },
        },
    },
    soldiers = {
        weights = { killed = 3, assisted = 5, answered = 3 },
        actions = {
            { name = "hunt hostiles", goal = true, run = function(ctx)
                local _, err = call("team_member", { bot = ctx.bot, objective = "attack", type = "hostile", members = { ctx.bot }, leader = ctx.bot, count = 4, solo = true })
                return err and ("can't hunt hostiles: " .. err) or "hunting hostile mobs" end },
            { name = "guard the leader", goal = true, run = function(ctx)
                local leader = call("leader", {})
                if type(leader) ~= "string" or leader == "" or leader == ctx.bot then return "no leader to guard" end
                local _, err = call("team_member", { bot = ctx.bot, objective = "follow", members = { leader, ctx.bot }, leader = leader, count = 1, solo = true })
                return err and ("can't guard: " .. err) or ("guarding " .. leader) end },
            { name = "gear up", run = function(ctx)
                call("autopilot", { bot = ctx.bot, enabled = true, gear = true })
                return has(ctx.st, "_sword") and make(ctx, "shield", 1) or make(ctx, "stone_sword", 1) end },
            { name = "store", run = store },
        },
    },
    blacksmiths = {
        weights = { given = 3, made = 1, needs = 4 },
        actions = {
            { name = "smelt iron", run = function(ctx)
                if count(ctx.st, "raw_iron") == 0 then return "no raw iron: the miners have it" end
                return make(ctx, "iron_ingot", count(ctx.st, "iron_ingot") + count(ctx.st, "raw_iron")) end },
            { name = "make tools", run = function(ctx)
                local tool = ({ "iron_pickaxe", "iron_axe", "iron_sword", "iron_hoe", "shears" })[math.random(5)]
                return make(ctx, tool, count(ctx.st, tool) + 1) end },
            { name = "make armor", run = function(ctx)
                local metal = count(ctx.st, "iron_ingot") >= 8 and "iron" or "leather"
                if metal == "leather" and count(ctx.st, "leather") < 5 then return "no iron nor leather: the miners and butchers have them" end
                local piece = ({ "helmet", "chestplate", "leggings", "boots" })[math.random(4)]
                return make(ctx, metal .. "_" .. piece, count(ctx.st, metal .. "_" .. piece) + 1) end },
            { name = "make buckets", run = function(ctx) return make(ctx, "bucket", count(ctx.st, "bucket") + 1) end },
            { name = "make torches", goal = true, run = function(ctx) return make(ctx, "torch", 32) end },
            { name = "hand out tools", run = function(ctx) return hand_out(ctx, { "_pickaxe", "_axe", "_sword", "_hoe", "_chestplate", "_helmet" }) end },
            { name = "trade", run = exchange },
            { name = "store", run = store },
        },
    },
    fishers = {
        weights = { fished = 2, made = 1, given = 1, needs = 2 },
        actions = {
            { name = "fish", run = function(ctx)
                if not has(ctx.st, "fishing_rod") then return make(ctx, "fishing_rod", 1) end
                local r, err = call("fish", { bot = ctx.bot, count = 5 })
                if r then return "fished: " .. (r.caught or 0) .. " caught" end
                if tostring(err):match("no open water") then return "no water near: " .. walk_off(ctx, 32) end
                return "could not fish: " .. tostring(err) end },
            { name = "cook fish", run = function(ctx)
                for _, raw in ipairs({ "cod", "salmon" }) do
                    local n = count(ctx.st, raw)
                    if n > 0 then return make(ctx, RAW[raw], n + count(ctx.st, RAW[raw])) end
                end
                return "no raw fish to cook" end },
            { name = "feed the hungriest", run = function(ctx)
                local who, low = nil, 15
                for _, st in ipairs(team(ctx)) do if (st.food or 20) < low then who, low = st.name, st.food end end
                if not who then return "nobody is hungry" end
                if count(ctx.st, "cooked_cod") + count(ctx.st, "cooked_salmon") == 0 then return "no cooked fish to give" end
                return give(ctx, who, "food", 8) end },
            { name = "trade", run = exchange },
            { name = "store", run = store },
        },
    },
    explorers = {
        weights = { collected = 0.3, explored = 1 },
        actions = {
            { name = "seek other woods", goal = true, run = function(ctx) return gather(ctx, "birch_log,jungle_log,dark_oak_log,acacia_log,cherry_log", 8) end },
            { name = "seek sand and clay", goal = true, run = function(ctx) return gather(ctx, "sand,clay", 16) end },
            { name = "seek pumpkins, melons, cane", goal = true, run = function(ctx) return gather(ctx, "pumpkin,melon,sugar_cane", 8) end },
            { name = "wander", run = function(ctx) return walk_off(ctx, 48) end },
            { name = "store", run = store },
        },
    },
}
for _, kind in ipairs({ { "cow", "wheat", "cows" }, { "sheep", "wheat", "sheep" }, { "pig", "carrot", "pigs" }, { "chicken", "wheat_seeds", "chickens" } }) do
    table.insert(ROLES.butchers.actions, { name = "breed " .. kind[3], run = function(ctx)
        if carried(ctx.st, function(i) return i == kind[2] end) < 2 then return "no " .. kind[2] .. " for the " .. kind[3] end
        local r, err = call("breed", { bot = ctx.bot, type = kind[1], food = "minecraft:" .. kind[2], radius = 24 })
        return r and ("bred " .. kind[3]) or ("could not breed " .. kind[3] .. ": " .. tostring(err)) end })
end
for _, a in ipairs({
    { name = "slaughter", run = butcher },
    { name = "cook meat", run = function(ctx)
        for raw, cooked in pairs(RAW) do
            local n = count(ctx.st, raw)
            if n > 0 and raw ~= "potato" then return make(ctx, cooked, n + count(ctx.st, cooked)) end
        end
        return "no raw meat to cook" end },
    { name = "shear sheep", run = function(ctx)
        local _, err = call("shear", { bot = ctx.bot, count = 4, radius = 24 })
        return err and ("could not shear: " .. err) or "sheared sheep" end },
    { name = "trade", run = exchange },
    { name = "store", run = store },
}) do table.insert(ROLES.butchers.actions, a) end
local ORDER = { "bakers", "farmers", "butchers", "miners", "builders", "soldiers", "blacksmiths", "explorers", "fishers" }

-- ---------------------------------------------------------------- state
local function save(state)
    local f = io.open(FILE, "w")
    if f then f:write("return ", NN.serialize(state)); f:close() end
end

local function restore()
    local f = io.open(FILE, "r")
    if not f then return nil end
    local chunk = load(f:read("a"), FILE, "t", { math = math })
    f:close()
    local ok, s = pcall(chunk or function() end)
    return ok and type(s) == "table" and s or nil
end

local state = restore() or {}
state.nets = state.nets or {}          -- trade -> its network
state.scale = state.scale or {}        -- trade -> running size of its rewards, to keep them near 1
state.prev = state.prev or {}          -- bot -> { role, x, action, snapshot } of its last turn
state.counts = state.counts or {}      -- trade -> action -> times drawn
state.rewards = state.rewards or {}    -- trade -> action -> running average reward
for i, role in ipairs(ORDER) do
    local n = #ROLES[role].actions
    -- A new trade, or its actions changed: a new network
    if not state.nets[role] or #state.nets[role].w2 ~= n or #state.nets[role].w1[1] ~= #INPUTS then
        state.nets[role] = NN.new(#INPUTS, HID, n, 1000 + i)
        state.counts[role], state.rewards[role] = {}, {}
    end
end

local function features(st, ctx)
    local function near(kind, n)
        local list = call("entities", { bot = st.name, type = kind }) or {}
        local k = 0
        for _, e in ipairs(list) do if (e.distance or 99) <= 16 then k = k + 1 end end
        return math.min(1, k / n)
    end
    local time = tonumber(st.day_time) or 0
    return {
        1, (st.health or 20) / 20, (st.food or 20) / 20,
        math.min(1, carried(st, function(i) return FOOD[i] or RAW[i] end) / 16),
        math.min(1, carried(st, function(i) return i:match("_log$") or i:match("_planks$") end) / 64),
        math.min(1, carried(st, function(i) return i == "cobblestone" or i == "stone" end) / 64),
        math.min(1, carried(st, function(i) return i == "iron_ingot" or i == "raw_iron" end) / 16),
        math.min(1, carried(st, function(i) return i == "coal" or i == "charcoal" end) / 16),
        has(st, "_pickaxe") and 1 or 0, has(st, "_sword") and 1 or 0, has(st, "_hoe") and 1 or 0,
        (time % 24000) >= 13000 and 1 or 0,
        near("hostile", 4), near("animal", 8), unmet(st, ctx.role),
    }
end

-- What a bot's figures are now, to judge its next turn by
local MADE = { torch = true, chest = true, iron_ingot = true, bread = true, pumpkin_pie = true, cake = true, bucket = true, leather = true,
    shears = true, cooked_cod = true, cooked_salmon = true, cooked_beef = true, cooked_porkchop = true, cooked_mutton = true, cooked_chicken = true }
local function snapshot(st, role)
    local s = { stats = {}, deaths = st.deaths or 0, stuck = st.times_stuck or 0, memory = 0, kinds = 0, made = 0, unmet = unmet(st, role) }
    for k, v in pairs(st.stats or {}) do s.stats[k] = tonumber(v) or 0 end
    for _, n in pairs(type(st.memory) == "table" and st.memory or {}) do s.memory, s.kinds = s.memory + (tonumber(n) or 0), s.kinds + 1 end
    s.made = carried(st, function(i) return MADE[i] or i:match("_pickaxe$") or i:match("_axe$") or i:match("_sword$") or i:match("_hoe$")
        or i:match("_helmet$") or i:match("_chestplate$") or i:match("_leggings$") or i:match("_boots$") end)
    return s
end

local function reward(role, before, now)
    local r = 0
    for what, w in pairs(ROLES[role].weights) do
        if what == "explored" then
            r = r + w * (5 * math.max(0, now.kinds - before.kinds) + math.max(0, now.memory - before.memory) / 100)
        elseif what == "made" then
            r = r + w * math.max(0, now.made - before.made)
        elseif what == "needs" then
            r = r + w * (before.unmet - now.unmet)       -- what it lacked, got (from the others or by itself)
        else
            local a, b = now.stats[what] or 0, before.stats[what] or 0
            r = r + w * (a >= b and a - b or a)        -- a counter that went down: a restart, all of it is new
        end
    end
    local function grew(a, b) return a >= b and a - b or a end
    return r - 20 * grew(now.deaths, before.deaths) - 2 * grew(now.stuck, before.stuck)
end

local function act(name, role, members, groups)
    local spec, net = ROLES[role], state.nets[role]
    if not spec then error("no trade " .. tostring(role)) end
    local st = call("status", { bot = name })
    if type(st) ~= "table" or not st.name then return { note = "no answer from " .. name } end
    local now = snapshot(st, role)
    -- The last turn's lesson, if it was in the same trade
    local prev, learnt = state.prev[name], nil
    if prev and prev.role == role and prev.x then
        local r = reward(role, prev.snap, now)
        state.scale[role] = 0.9 * (state.scale[role] or 5) + 0.1 * math.max(1, math.abs(r))
        NN.learn(net, prev.x, prev.action, r / state.scale[role])
        local name_of = spec.actions[prev.action].name
        state.rewards[role][name_of] = 0.8 * (state.rewards[role][name_of] or r) + 0.2 * r
        learnt = r
    end
    local ctx = { bot = name, st = st, members = members, groups = groups, role = role }
    local x = features(st, ctx)
    local p = NN.forward(net, x)
    local pick, u = #p, math.random()
    for o = 1, #p do u = u - p[o]; if u <= 0 then pick = o; break end end
    local action = spec.actions[pick]
    local note
    if action.goal and prev and prev.role == role and prev.action == pick then
        note = action.name .. " (still on it)"
    else
        local ok, r = pcall(action.run, ctx)
        note = ok and r or ("failed: " .. tostring(r))
    end
    state.counts[role][action.name] = (state.counts[role][action.name] or 0) + 1
    state.prev[name] = { role = role, x = x, action = pick, snap = now }
    save(state)
    return { action = action.name, note = note, want_percent = math.floor(p[pick] * 100 + 0.5), last_reward = learnt }
end

bot.tool{
    name = "roles",
    description = "Each trade's own neural network (bakers, farmers, butchers, miners, builders, soldiers, blacksmiths, explorers, fishers): within its trade, it picks what a bot does next from 15 numbers about the bot (health, hunger, what it carries, its tools, night, hostiles and animals near, how much of what its trade needs it lacks), the trades handing each other what they make (wheat, eggs, milk, pumpkins, bread, leather, ore, tools, buckets, torches), and learns from what the bot achieved by its next turn (actor-critic). With no argument: each trade's actions, how often drawn and their average reward. act=bot with role: the bot's turn (the groups plugin). network=trade (bot=name optional): the network's weights and what it makes of that bot now.",
    params = { act = "string", role = "string", members = "array", groups = "object", network = "string", bot = "string" },
    run = function(args)
        if type(args.act) == "string" then return act(args.act, args.role, args.members, args.groups) end
        if type(args.network) == "string" then
            local role = args.network
            local spec, net = ROLES[role], state.nets[role]
            if not spec then error("no trade " .. role) end
            local st = type(args.bot) == "string" and call("status", { bot = args.bot }) or nil
            local x = (type(st) == "table" and st.name) and features(st, { role = role }) or { 1 }
            local p, hidden, value = NN.forward(net, x)
            local inputs, outputs = {}, {}
            for i = 1, #INPUTS do inputs[i] = { name = INPUTS[i], value = x[i] or 0 } end
            for o, a in ipairs(spec.actions) do
                outputs[o] = { name = a.name, percent = math.floor(p[o] * 1000 + 0.5) / 10, bias = net.b2[o],
                    drawn = state.counts[role][a.name] or 0, avg_reward = state.rewards[role][a.name] }
            end
            local prev = type(args.bot) == "string" and state.prev[args.bot] or nil
            return { role = role, bot = args.bot, inputs = inputs, hidden = hidden, hidden_bias = net.b1, outputs = outputs, value = value,
                w1 = net.w1, w2 = net.w2, v = net.v, vb = net.vb,
                last_action = prev and prev.role == role and spec.actions[prev.action].name or nil }
        end
        local out = {}
        for _, role in ipairs(ORDER) do
            local actions = {}
            for _, a in ipairs(ROLES[role].actions) do
                actions[a.name] = { drawn = state.counts[role][a.name] or 0, avg_reward = state.rewards[role][a.name] }
            end
            out[role] = actions
        end
        return out
    end,
}
