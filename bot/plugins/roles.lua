-- Each bot has a separate action policy for each trade, using the brain plugin's global NN implementation.
-- Every five game days, the two best policies per trade seed lightly mutated copies for the other bots.
--   bakers       bread, pumpkin pies and cakes from the farmers' wheat, eggs, milk and pumpkins; feed the hungry
--   farmers      wheat fields, eggs from the chickens, pumpkins, milk from the cows
--   butchers     breed the animals, slaughter some: meat (cooked) and leather
--   miners       ores: coal and iron for the blacksmiths
--   blacksmiths  smelt the iron, make tools, buckets, torches and armor (iron, or leather from the butchers)
--   fishers      fish with a rod at open water, cook the catch (coal from the miners), feed the hungry
-- The trades trade: each needs things the others make (NEEDS, PROVIDES). Its "trade" action asks a bot of the
-- trade that makes what it lacks to hand it over, or else hands its own surplus to the bot that lacks it most.
--   in   20 numbers about the bot: health, hunger, food, wood, stone, iron, coal it carries, its tools, night,
--        hostile mobs and animals near, how much of what its trade needs it lacks
--   ->   three hidden layers of 8 units (tanh with residual connections)
--   out  the trade's actions (softmax: drawn from, so it keeps trying the others)
-- Every minute the groups plugin gives each bot its turn: the bot's network draws an action and the bot does it;
-- at its next turn, what it achieved since (its stats, weighted for the trade, minus deaths and getting
-- stuck) is the reward the network learns from (actor-critic).
--   roles {}                                   each trade's network: actions, how often, last rewards
--   roles { act = "Bot3", role = "miners" }    the bot's turn (the groups plugin)
--   roles { network = "miners", bot = "Bot3" } the network, and what it makes of the bot now (the panels)
local FILE = "../roles_state.lua"
local HID = 8
local INPUTS = {
    "bias", "health", "food level", "air", "pos x", "pos y", "pos z", "night",
    "needs unmet", "team hungry",
    "building", "build remaining", "build idle", "has objective",
    "armor worn", "gear quality", "has pickaxe", "has axe", "has sword", "has hoe",
    "food carried", "food variety", "wood carried", "stone carried", "iron carried", "coal carried",
    "hostiles near", "animals near", "players near",
    "chest resources", "furnaces known", "world resources",
}
for _, name in ipairs(NN.environment_inputs) do INPUTS[#INPUTS + 1] = name end
local ORES = { coal = "coal_ore", iron = "iron_ore", copper_gold = "copper_ore,gold_ore", deep = "redstone_ore,lapis_ore,diamond_ore" }
local FOOD = NN.edible or { bread = true, pumpkin_pie = true, cake = true, cooked_beef = true, cooked_porkchop = true, cooked_mutton = true, cooked_chicken = true, baked_potato = true,
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
    builders = { oak_log = 16, cobblestone = 32, chest = 2, bread = 4 },
    lumberjacks = { stone_axe = 1, bread = 4 },
    carpenters = { oak_log = 16, bread = 4 },
    bakers = { wheat = 9, egg = 2, milk_bucket = 3, pumpkin = 2 },
    farmers = { bucket = 1, bread = 4 },
    butchers = { wheat = 8, wheat_seeds = 8, carrot = 4, bread = 4 },
    miners = { iron_pickaxe = 1, torch = 16, bread = 4 },
    blacksmiths = { raw_iron = 9, coal = 8, leather = 4, bread = 4 },
    fishers = { coal = 4, bread = 2 },
}
local PROVIDES = { oak_log = "lumberjacks", spruce_log = "lumberjacks", birch_log = "lumberjacks", jungle_log = "lumberjacks",
    acacia_log = "lumberjacks", dark_oak_log = "lumberjacks", mangrove_log = "lumberjacks", cherry_log = "lumberjacks", pale_oak_log = "lumberjacks",
    chest = "carpenters", oak_planks = "carpenters", oak_stairs = "carpenters", oak_fence = "carpenters", stone_axe = "blacksmiths",
    cobblestone = "miners", wheat = "farmers", egg = "farmers", milk_bucket = "farmers", pumpkin = "farmers", wheat_seeds = "farmers",
    carrot = "farmers", bread = "bakers", leather = "butchers", raw_iron = "miners", coal = "miners", iron_pickaxe = "blacksmiths",
    bucket = "blacksmiths", torch = "blacksmiths", cooked_cod = "fishers", cooked_salmon = "fishers" }
local function count(st, item) return carried(st, function(i) return i == item end) end
local function logs_carried(st) return carried(st, function(i) return i:sub(-4) == "_log" end) end
local function have_need(st, item) return item == "oak_log" and logs_carried(st) or count(st, item) end
local function sorted(t) local k = {} for x in pairs(t) do k[#k + 1] = x end table.sort(k) return k end

-- How much of what its trade needs the bot lacks: 0 has it all, 1 has none of it
local function unmet(st, role)
    local needs, lack, n = NEEDS[role] or {}, 0, 0
    for item, want in pairs(needs) do lack, n = lack + math.max(0, want - have_need(st, item)) / want, n + 1 end
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
        local lack = needs[item] - have_need(ctx.st, item)
        local from = PROVIDES[item]
        if lack > 0 and from ~= ctx.role then
            for _, b in ipairs(ctx.groups and ctx.groups[from] or {}) do
                local st = status(b)
                local give_item = item
                local spare = st and count(st, item) - ((NEEDS[from] or {})[item] or 0) or 0
                if item == "oak_log" and spare <= 0 and st then
                    for k, v in pairs(inv(st)) do
                        if k:sub(-4) == "_log" and (tonumber(v) or 0) > spare then give_item, spare = k, tonumber(v) end
                    end
                end
                if spare > 0 then
                    local _, err = call("give", { bot = b, to = ctx.bot, item = "minecraft:" .. give_item, count = math.min(spare, lack) })
                    return err and ("asked " .. b .. " for " .. give_item .. ": " .. err) or ("got " .. give_item .. " from " .. b)
                end
            end
        end
    end
    -- What it makes beyond its own needs, to the bot of another trade (or active house builder) lacking it most
    for _, item in ipairs(sorted(PROVIDES)) do
        local need_key = item:sub(-4) == "_log" and "oak_log" or item
        local spare = PROVIDES[item] == ctx.role and count(ctx.st, item) - (needs[need_key] or 0) or 0
        if spare > 0 then
            local who, most = nil, 0
            for _, trade in ipairs(sorted(NEEDS)) do
                local want = trade ~= ctx.role and NEEDS[trade][need_key]
                for _, b in ipairs(want and ctx.groups and ctx.groups[trade] or {}) do
                    local st = status(b)
                    local lack = st and want - have_need(st, need_key) or 0
                    if lack > most then who, most = b, lack end
                end
                if want and trade == "builders" then
                    local seen = {}
                    for _, st in ipairs(team(ctx)) do
                        seen[st.name] = true
                        local lack = st.building and want - have_need(st, need_key) or 0
                        if lack > most then who, most = st.name, lack end
                    end
                    for _, members in pairs(ctx.groups or {}) do
                        for _, b in ipairs(members) do
                            if not seen[b] and b ~= ctx.bot then
                                seen[b] = true
                                local st = status(b)
                                local lack = st and st.building and want - have_need(st, need_key) or 0
                                if lack > most then who, most = b, lack end
                            end
                        end
                    end
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
        weights = { harvested = 1, given = 1, collected = 0.2, built = 2, bred = 5, needs = 4 },
        actions = {
            { name = "harvest", run = function(ctx)
                local r = call("farm", { bot = ctx.bot, radius = 16 })
                return r and ("farm: " .. (r.harvested or 0) .. " harvested, " .. (r.replanted or 0) .. " replanted") or "nothing ripe" end },
            { name = "make a farm", run = function(ctx)
                if not has(ctx.st, "_hoe") then return make(ctx, "stone_hoe", 1) end
                local r, err = call("make_farm", { bot = ctx.bot })
                if r then return "made a farm: " .. (r.tilled or 0) .. " tilled" end
                if tostring(err):match("needs a bucket") then return make(ctx, "bucket", 1) end
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
-- Append actions so saved output indices keep their meaning.
for _, role in ipairs({ "builders", "miners" }) do
    table.insert(ROLES[role].actions, { name = "gather construction stone", goal = true,
        run = function(ctx) return gather(ctx, "stone,cobblestone", 64) end })
    table.insert(ROLES[role].actions, { name = "gather construction logs", goal = true,
        run = function(ctx) return gather(ctx, "oak_log,spruce_log,birch_log", 32) end })
end
for _, role in ipairs({ "builders", "blacksmiths" }) do
    table.insert(ROLES[role].actions, { name = "make storage chests", goal = true,
        run = function(ctx) return make(ctx, "chest", 4) end })
end
table.insert(ROLES.bakers.actions, { name = "cook available food", run = function(ctx)
    for _, raw in ipairs(sorted(RAW)) do
        if count(ctx.st, raw) > 0 then
            return make(ctx, RAW[raw], count(ctx.st, RAW[raw]) + count(ctx.st, raw))
        end
    end
    return "no raw food to cook"
end })
table.insert(ROLES.blacksmiths.actions, { name = "make building axes", goal = true,
    run = function(ctx) return make(ctx, "stone_axe", 2) end })
table.insert(ROLES.blacksmiths.actions, { name = "hand out axes", run = function(ctx) return hand_out(ctx, { "_axe" }) end })

ROLES.lumberjacks = { weights = { collected = 1, given = 2, stored = 0.05, built = 1, needs = 4 }, actions = {
    { name = "fell trees", goal = true, run = function(ctx) return gather(ctx, "oak_log,spruce_log,birch_log,jungle_log,acacia_log,dark_oak_log", 64) end },
    { name = "make an axe", run = function(ctx) return make(ctx, "stone_axe", 1) end },
    { name = "trade", run = exchange }, { name = "store", run = store },
} }
ROLES.carpenters = { weights = { made = 1, built = 2, given = 2, needs = 4 }, actions = {
    { name = "trade", run = exchange }, { name = "store", run = store },
} }
for _, item in ipairs({ "oak_planks", "chest", "barrel", "oak_stairs", "oak_fence", "oak_pressure_plate", "oak_door", "stick" }) do
    table.insert(ROLES.carpenters.actions, { name = "craft " .. item,
        run = function(ctx) return make(ctx, item, item == "oak_planks" and 64 or item == "stick" and 16 or 4) end })
end

local function residence(ctx)
    local h = call("empire", { residence = ctx.bot })
    return type(h) == "table" and h.ready and h.at or nil
end
local function decorate(ctx)
    local o = residence(ctx)
    if not o then return "house not ready" end
    for _, part in ipairs({ { 1, 0, 4, "bookshelf" }, { 4, 0, 2, "white_carpet" }, { 2, 0, 3, "lantern" } }) do
        local x, y, z = o.x + part[1], o.y + part[2], o.z + part[3]
        local b = call("get_block", { bot = ctx.bot, x = x, y = y, z = z })
        if not b or not b.block or b.block == "" then return "decoration site not visible" end
        if b.block == "air" or b.block == "minecraft:air" then
            if count(ctx.st, part[4]) == 0 then return make(ctx, part[4], 1) end
            local _, err = call("place_block", { bot = ctx.bot, x = x, y = y, z = z, item = part[4] })
            return err and ("could not decorate: " .. err) or ("decorated with " .. part[4])
        end
    end
    return "house already decorated"
end
local function upgrade(ctx, kinds)
    local costs = { pickaxe = 3, axe = 3, sword = 2, shovel = 1, hoe = 2, chestplate = 8, helmet = 5, leggings = 7, boots = 4 }
    local slots = { chestplate = "chest", helmet = "head", leggings = "legs", boots = "feet" }
    for _, kind in ipairs(kinds) do
        local equipped = (ctx.st.equipment or {})[slots[kind]]
        local best
        for _, material in ipairs({ "netherite", "diamond", "iron" }) do
            local item = material .. "_" .. kind
            if count(ctx.st, item) > 0 or equipped == item then best = item; break end
        end
        if slots[kind] and best and equipped ~= best then
            local _, err = call("equip", { bot = ctx.bot, item = best, slot = "armor" })
            return err and tostring(err) or ("wearing " .. best)
        end
        local target = count(ctx.st, "diamond") >= costs[kind] and "diamond" or "iron"
        if not best or (target == "diamond" and best == "iron_" .. kind) then return make(ctx, target .. "_" .. kind, 1) end
    end
    return "gear already upgraded"
end
local function hunt_action(ctx)
    local hostiles = call("entities", { bot = ctx.bot, type = "hostile" }) or {}
    if #hostiles > 0 then
        local _, err = call("kill", { bot = ctx.bot, type = hostiles[1].type or "zombie", count = 1 })
        return err and ("hunt hostile: " .. err) or "hunted hostile mob"
    end
    for _, animal in ipairs({ "cow", "pig", "sheep", "chicken" }) do
        local list = call("entities", { bot = ctx.bot, type = animal }) or {}
        if #list > 0 then
            local _, err = call("kill", { bot = ctx.bot, type = animal, count = 1 })
            return err and ("hunt " .. animal .. ": " .. err) or ("hunted " .. animal)
        end
    end
    return butcher(ctx)
end
local function mine_action(ctx)
    if has(ctx.st, "_pickaxe") then return mine(ctx, ORES.coal .. "," .. ORES.iron) end
    return gather(ctx, "stone,cobblestone", 16)
end
local function eat_action(ctx)
    if (ctx.st.food or 20) < 20 and carried(ctx.st, function(i) return FOOD[i] end) > 0 then
        local _, err = call("eat", { bot = ctx.bot })
        return err and ("could not eat: " .. err) or "ate food"
    end
    for _, raw in ipairs(sorted(RAW)) do
        if count(ctx.st, raw) > 0 then return make(ctx, RAW[raw], count(ctx.st, RAW[raw]) + count(ctx.st, raw)) end
    end
    if count(ctx.st, "wheat") >= 3 then return make(ctx, "bread", count(ctx.st, "bread") + count(ctx.st, "wheat") // 3) end
    return (ctx.st.food or 20) >= 20 and "already full" or "no food to eat"
end
local function craft_action(ctx)
    if not has(ctx.st, "_pickaxe") then return make(ctx, "stone_pickaxe", 1) end
    if not has(ctx.st, "_axe") then return make(ctx, "stone_axe", 1) end
    if not has(ctx.st, "_sword") then return make(ctx, "stone_sword", 1) end
    if count(ctx.st, "torch") < 8 and count(ctx.st, "coal") + count(ctx.st, "charcoal") > 0 then
        return make(ctx, "torch", count(ctx.st, "torch") + 16)
    end
    if logs_carried(ctx.st) > 0 and count(ctx.st, "oak_planks") < 16 then
        return make(ctx, "oak_planks", count(ctx.st, "oak_planks") + 16)
    end
    return make(ctx, "chest", count(ctx.st, "chest") + 1)
end
local function rest_action(ctx)
    call("autopilot", { bot = ctx.bot, enabled = true, gather = "", count = 0 })
    return "resting"
end
local function sleep_action(ctx)
    local _, err = call("home", { bot = ctx.bot })
    return err and ("could not sleep: " .. err) or "went home to sleep"
end
local function help_players_action(ctx)
    for _, other in ipairs(team(ctx)) do
        if (other.food or 20) < 15 and carried(ctx.st, function(i) return FOOD[i] end) > 1 then
            return give(ctx, other.name, "food", 4)
        end
    end
    local r = exchange(ctx)
    if r ~= "nothing to trade" then return r end
    local players = call("entities", { bot = ctx.bot, type = "player" }) or {}
    for _, p in ipairs(players) do
        if p.name ~= ctx.bot and p.position then
            call("goto", { bot = ctx.bot, x = math.floor(p.position.x), y = math.floor(p.position.y), z = math.floor(p.position.z), tolerance = 2 })
            return "went to help " .. (p.name or "player")
        end
    end
    return "no player in need nearby"
end
for _, role in ipairs({ "bakers", "farmers", "butchers", "miners", "builders", "soldiers", "blacksmiths", "explorers", "fishers", "lumberjacks", "carpenters" }) do
    table.insert(ROLES[role].actions, { name = "decorate home", run = decorate })
    table.insert(ROLES[role].actions, { name = "upgrade tools", run = function(ctx) return upgrade(ctx, { "pickaxe", "axe", "sword", "shovel", "hoe" }) end })
    table.insert(ROLES[role].actions, { name = "upgrade armor", run = function(ctx)
        call("autopilot", { bot = ctx.bot, enabled = true, gear = true })
        return upgrade(ctx, { "chestplate", "helmet", "leggings", "boots" }) end })
    table.insert(ROLES[role].actions, { name = "move", run = function(ctx) return walk_off(ctx, 24) end })
    table.insert(ROLES[role].actions, { name = "hunt", run = hunt_action })
    table.insert(ROLES[role].actions, { name = "mine", goal = true, run = mine_action })
    table.insert(ROLES[role].actions, { name = "eat", run = eat_action })
    table.insert(ROLES[role].actions, { name = "craft", run = craft_action })
    table.insert(ROLES[role].actions, { name = "rest", run = rest_action })
    table.insert(ROLES[role].actions, { name = "sleep", run = sleep_action })
    table.insert(ROLES[role].actions, { name = "help other players", run = help_players_action })
end
for _, item in ipairs({ "baked_potato", "beetroot_soup", "mushroom_stew", "rabbit_stew", "cookie" }) do
    table.insert(ROLES.bakers.actions, { name = "cook " .. item, run = function(ctx) return make(ctx, item, 4) end })
end
table.insert(ROLES.farmers.actions, { name = "gather planting seeds", goal = true,
    run = function(ctx) return gather(ctx, "short_grass,tall_grass", 24) end })
-- Its lot's pen spot, south of the garden plot (the empire plugin keeps it clear of the road)
local function pen(ctx)
    local h = call("empire", { residence = ctx.bot })
    return type(h) == "table" and h.ready and h.pen or nil
end
table.insert(ROLES.farmers.actions, { name = "build animal pen", run = function(ctx)
    local o = pen(ctx)
    if not o then return "needs a finished home lot with room for a pen" end
    local blocks = {}
    for x = 0, 5 do for z = 0, 5 do
        blocks[#blocks + 1] = { o.x + x, o.y - 1, o.z + z, "cobblestone" }
        local edge = x == 0 or x == 5 or z == 0 or z == 5
        blocks[#blocks + 1] = { o.x + x, o.y, o.z + z, edge and (x == 2 and z == 5 and "oak_fence_gate[open=false]" or "oak_fence") or "air" }
        for y = 1, 2 do blocks[#blocks + 1] = { o.x + x, o.y + y, o.z + z, "air" } end
    end end
    local _, err = call("team_member", { bot = ctx.bot, objective = "build", plan = "blocks", blocks = blocks,
        members = { ctx.bot }, leader = ctx.bot, solo = true, name = ctx.bot .. "'s animal pen" })
    return err and tostring(err) or "building an animal pen"
end })
-- Open or shut the fence gate at `at` (the pen's, the egg collector's)
local function gate(ctx, at, open)
    local args = { bot = ctx.bot, x = at.x, y = at.y, z = at.z }
    local b = call("get_block", args)
    if not b or not tostring(b.block):match("_fence_gate") then return false end
    local state = b.state
    if type(state) ~= "table" or state.open == nil then return false end
    if tostring(state.open) ~= tostring(open) then
        local r = call("interact_block", args)
        if r == nil then return false end
    end
    return true
end
for _, animal in ipairs({ { "cow", "wheat" }, { "sheep", "wheat" }, { "pig", "carrot" }, { "chicken", "wheat_seeds" } }) do
    table.insert(ROLES.farmers.actions, { name = "gather " .. animal[1], run = function(ctx)
        local o = pen(ctx)
        if not o or count(ctx.st, animal[2]) == 0 then return "needs a pen and " .. animal[2] end
        local at = { x = o.x + 2, y = o.y, z = o.z + 5 }
        if not gate(ctx, at, true) then return "pen gate is not ready" end
        local _, err = call("lure", { bot = ctx.bot, type = animal[1], food = animal[2], x = o.x + 2, y = o.y, z = o.z + 2 })
        local closed = gate(ctx, at, false)
        return err and tostring(err) or (closed and "led animals into the pen" or "animals led; could not close the gate")
    end })
    table.insert(ROLES.farmers.actions, { name = "breed " .. animal[1], run = function(ctx)
        if count(ctx.st, animal[2]) < 2 then return "needs " .. animal[2] end
        local _, err = call("breed", { bot = ctx.bot, type = animal[1], food = animal[2], radius = 24 })
        return err and tostring(err) or "bred " .. animal[1]
    end })
end
-- Automatic farms (farms plugin): one of each plan per bot, at a free spot near it
local function farm_of(ctx, plan)
    for _, f in ipairs(call("farms", { built = true }) or {}) do
        if f.bot == ctx.bot and f.plan == plan then return f end
    end
end
local function build_farm(ctx, plan)
    local what = (plan:gsub("_", " "))
    if farm_of(ctx, plan) then return "already has a " .. what end
    -- In the village its house comes first: until it stands the house order is handed again and replaces any other
    local h = call("empire", { residence = ctx.bot })
    if type(h) == "table" and h.ready == false then return "builds its house first" end
    local r, err = call("farms", { build = plan, bot = ctx.bot })
    return err or ("building a " .. what .. " at " .. r.x .. " " .. r.y .. " " .. r.z)
end
table.insert(ROLES.farmers.actions, { name = "build cactus farm", run = function(ctx) return build_farm(ctx, "cactus_farm") end })
table.insert(ROLES.farmers.actions, { name = "build egg collector", run = function(ctx) return build_farm(ctx, "egg_collector") end })
table.insert(ROLES.farmers.actions, { name = "fill egg collector", run = function(ctx)
    local f = farm_of(ctx, "egg_collector")
    if not f or count(ctx.st, "wheat_seeds") == 0 then return "needs an egg collector and wheat_seeds" end
    -- Its gate is on the south side of the one-block pen, the hopper in the middle
    local at = { x = f.x + 1, y = f.y, z = f.z + 2 }
    if not gate(ctx, at, true) then return "egg collector gate is not ready" end
    local _, err = call("lure", { bot = ctx.bot, type = "chicken", food = "wheat_seeds", x = f.x + 1, y = f.y, z = f.z + 1 })
    local closed = gate(ctx, at, false)
    return err and tostring(err) or (closed and "led chickens into the egg collector" or "chickens led; could not close the gate")
end })
table.insert(ROLES.blacksmiths.actions, { name = "build auto smelter", run = function(ctx) return build_farm(ctx, "auto_smelter") end })

local ORDER = { "bakers", "farmers", "butchers", "miners", "builders", "soldiers", "blacksmiths", "explorers", "fishers", "lumberjacks", "carpenters" }

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
state.nets = state.nets or {}          -- legacy trade policies, used to seed individual policies
if not state.bot_nets then state.prev = {} end
state.bot_nets = state.bot_nets or {}  -- bot -> trade -> its own independently trained policy
state.fitness = state.fitness or {}    -- trade -> bot -> { reward sum, samples }
state.day = state.day or 0
state.last_day_at = state.last_day_at or os.time()
state.scale = state.scale or {}        -- trade -> running size of its rewards, to keep them near 1
state.prev = state.prev or {}          -- bot -> { role, x, action, snapshot } of its last turn
state.counts = state.counts or {}      -- trade -> action -> times drawn
state.rewards = state.rewards or {}    -- trade -> action -> running average reward
for i, role in ipairs(ORDER) do
    local n = #ROLES[role].actions
    local net = state.nets[role]
    state.nets[role] = net and NN.grow(net, #INPUTS, n) or NN.new(#INPUTS, HID, n, 1000 + i)
    state.counts[role], state.rewards[role] = state.counts[role] or {}, state.rewards[role] or {}
end

for _, policies in pairs(state.bot_nets) do
    for role, net in pairs(policies) do
        if ROLES[role] then policies[role] = NN.grow(net, #INPUTS, #ROLES[role].actions) end
    end
end

local function net_of(name, role)
    state.bot_nets[name] = state.bot_nets[name] or {}
    local policies = state.bot_nets[name]
    if not policies[role] then policies[role] = NN.mutated(state.nets[role]) end
    return policies[role]
end

-- This clock also runs in empire mode, when the trade-selection brain is stopped.
bot.every(30, function()
    local st = call("status", {})
    local time = type(st) == "table" and tonumber(st.day_time) or nil
    local new_day = (time and state.last_time and time < state.last_time) or os.time() - state.last_day_at >= 1500
    if time then state.last_time = time end
    if not new_day then return end
    state.day, state.last_day_at = state.day + 1, os.time()
    if state.day % 5 == 0 then
        state.last_generation = {}
        for _, role in ipairs(ORDER) do
            local nets = {}
            for name, policies in pairs(state.bot_nets) do if policies[role] then nets[name] = policies[role] end end
            local ranking, parents = NN.evolve(nets, state.fitness[role] or {})
            if #parents > 0 then
                state.nets[role] = NN.copy(nets[parents[1]])
                for name, net in pairs(nets) do
                    state.bot_nets[name][role] = net
                    if state.prev[name] and state.prev[name].role == role then state.prev[name] = nil end
                end
                state.last_generation[role] = { winners = parents, ranking = ranking }
            end
        end
        state.fitness = {}
        state.generation = (state.generation or 0) + 1
    end
    save(state)
end)

local function features(st, ctx)
    local function near(kind, n)
        local list = call("entities", { bot = st.name, type = kind }) or {}
        local k = 0
        for _, e in ipairs(list) do if (e.distance or 99) <= 16 then k = k + 1 end end
        if kind == "player" and k == 0 and type(st.players_online) == "table" then
            k = math.max(0, #st.players_online - 1)
        end
        return math.min(1, k / n)
    end
    local pos = type(st.position) == "table" and st.position or {}
    local cond = NN.condition(st)
    local time = tonumber(st.day_time) or 0
    local hungry_team = math.max(0, (14 - (st.food or 20)) / 14)
    if ctx and ctx.team and #ctx.team > 0 then
        local h = 0
        for _, o in ipairs(ctx.team) do if (o.food or 20) <= 12 then h = h + 1 end end
        hungry_team = math.max(hungry_team, h / #ctx.team)
    end
    local x = {
        1,
        (st.health or 20) / 20,
        (st.food or 20) / 20,
        math.max(0, math.min(1, (tonumber(st.air) or 300) / 300)),
        math.tanh((tonumber(pos.x) or 0) / 500),
        math.max(0, math.min(1, ((tonumber(pos.y) or 64) + 64) / 384)),
        math.tanh((tonumber(pos.z) or 0) / 500),
        (time % 24000) >= 13000 and 1 or 0,
        unmet(st, ctx and ctx.role),
        hungry_team,
        st.building and 1 or 0,
        math.min(1, (st.build_left or 0) / 1000),
        math.min(1, (st.build_idle or 0) / 600),
        NN.has_objective(st),
        NN.armor_worn(st),
        cond.quality / 45,
        has(st, "_pickaxe") and 1 or 0,
        has(st, "_axe") and 1 or 0,
        has(st, "_sword") and 1 or 0,
        has(st, "_hoe") and 1 or 0,
        math.min(1, carried(st, function(i) return FOOD[i] or RAW[i] end) / 16),
        math.min(1, cond.variety / 8),
        math.min(1, carried(st, function(i) return i:match("_log$") or i:match("_planks$") end) / 64),
        math.min(1, carried(st, function(i) return i == "cobblestone" or i == "stone" end) / 64),
        math.min(1, carried(st, function(i) return i == "iron_ingot" or i == "raw_iron" end) / 16),
        math.min(1, carried(st, function(i) return i == "coal" or i == "charcoal" end) / 16),
        near("hostile", 4),
        near("animal", 8),
        near("player", 4),
        NN.stored_resources(st),
        NN.stations_known(st),
        NN.world_resources(st),
    }
    for _, value in ipairs(NN.environment(st)) do x[#x + 1] = value end
    return x
end

-- What a bot's figures are now, to judge its next turn by
local MADE = { oak_planks = true, barrel = true, oak_stairs = true, oak_fence = true, oak_pressure_plate = true, oak_door = true, bookshelf = true, white_carpet = true, lantern = true, torch = true, chest = true, iron_ingot = true, bread = true, pumpkin_pie = true, cake = true, bucket = true, leather = true,
    shears = true, cooked_cod = true, cooked_salmon = true, cooked_beef = true, cooked_porkchop = true, cooked_mutton = true, cooked_chicken = true }
local function snapshot(st, role)
    local s = { stats = {}, deaths = st.deaths or 0, stuck = st.times_stuck or 0, memory = 0, kinds = 0, made = 0, unmet = unmet(st, role), living = NN.condition(st) }
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
    r = r - grew(now.stats.construction_penalty or 0, before.stats.construction_penalty or 0)
        - 0.1 * grew(now.stats.idle_seconds or 0, before.stats.idle_seconds or 0)
    for what, w in pairs(NN.hazards or {}) do r = r + w * grew(now.stats[what] or 0, before.stats[what] or 0) end
    return r + NN.living_reward(before.living, now.living) - 40 * grew(now.deaths, before.deaths) - 2 * grew(now.stuck, before.stuck)
end

-- The state is 2 MB: written at most once a minute, not after each of the hundred turns a minute
local saved_at = 0
local function save_soon()
    if os.time() - saved_at < 60 then return end
    saved_at = os.time()
    save(state)
end

local function act(name, role, members, groups)
    local spec = ROLES[role]
    if not spec then error("no trade " .. tostring(role)) end
    local net = net_of(name, role)
    local st = call("status", { bot = name })
    if type(st) ~= "table" or not st.name then return { note = "no answer from " .. name } end
    if st.building then
        return { action = "continue construction", note = "keeping the active building order" }
    end
    local now = snapshot(st, role)
    -- The last turn's lesson, if it was in the same trade
    local prev, learnt = state.prev[name], nil
    if prev and prev.role == role and prev.x then
        local r = reward(role, prev.snap, now)
        state.fitness[role] = state.fitness[role] or {}
        local f = state.fitness[role][name] or { 0, 0 }
        state.fitness[role][name] = { f[1] + r, f[2] + 1 }
        state.scale[role] = 0.9 * (state.scale[role] or 5) + 0.1 * math.max(1, math.abs(r))
        NN.learn(net, prev.x, prev.action, r / state.scale[role])
        local name_of = spec.actions[prev.action].name
        state.rewards[role][name_of] = 0.8 * (state.rewards[role][name_of] or r) + 0.2 * r
        learnt = r
    end
    local ctx = { bot = name, st = st, members = members, groups = groups, role = role }
    -- Feeding a starving team must not depend on a random policy draw.
    if role == "bakers" then
        local who, low = nil, 9
        for _, other in ipairs(team(ctx)) do
            if other.name ~= name and (other.food or 20) < low then who, low = other.name, other.food end
        end
        if who then
            local note
            if carried(st, function(i) return FOOD[i] end) > 4 then
                note = give(ctx, who, "food", 4)
            elseif count(st, "wheat") >= 3 then
                note = make(ctx, "bread", count(st, "bread") + count(st, "wheat") // 3)
            else
                for _, raw in ipairs(sorted(RAW)) do
                    if count(st, raw) > 0 then note = make(ctx, RAW[raw], count(st, RAW[raw]) + count(st, raw)); break end
                end
                if not note then
                    for _, animal in ipairs({ "cow", "pig", "sheep", "chicken" }) do
                        local animals = call("entities", { bot = name, type = animal })
                        if type(animals) == "table" and #animals > 0 then
                            local _, err = call("kill", { bot = name, type = animal, count = 1 })
                            note = err and tostring(err) or "hunting food for the team"
                            break
                        end
                    end
                end
                note = note or exchange(ctx)
            end
            state.prev[name] = nil
            save_soon()
            return { action = "feed the hungry team", note = note }
        end
    end
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
    save_soon()
    return { action = action.name, note = note, want_percent = math.floor(p[pick] * 100 + 0.5), last_reward = learnt }
end

ROLES_ACT = act     -- the groups plugin runs the turns with it, each on its own thread (bot.spawn)

bot.tool{
    name = "roles",
    description = "Each bot has its own independently trained policy for each trade (bakers, farmers, butchers, miners, builders, soldiers, blacksmiths, explorers, fishers): within its trade, it picks what a bot does next from 104 numbers about the bot and environment (health, hunger, what it carries, its tools, night, hostiles and animals near, how much of what its trade needs it lacks), the trades handing each other what they make (wheat, eggs, milk, pumpkins, bread, leather, ore, tools, buckets, torches), and learns from what the bot achieved by its next turn (actor-critic). Every 5 game days the two best evaluated policies in each trade seed lightly mutated copies for the others; both parents are preserved. With no argument: each trade's actions, how often drawn and their average reward. act=bot with role: the bot's turn (the groups plugin). network=trade (bot=name optional): the network's weights and what it makes of that bot now.",
    params = { act = "string", role = "string", members = "array", groups = "object", network = "string", bot = "string" },
    run = function(args)
        if type(args.act) == "string" then return act(args.act, args.role, args.members, args.groups) end
        if type(args.network) == "string" then
            local role = args.network
            local spec = ROLES[role]
            if not spec then error("no trade " .. role) end
            local net = type(args.bot) == "string" and net_of(args.bot, role) or state.nets[role]
            local st = type(args.bot) == "string" and call("status", { bot = args.bot }) or nil
            local x = (type(st) == "table" and st.name) and features(st, { role = role }) or { 1 }
            local p, hidden, value, activations = NN.forward(net, x)
            local inputs, outputs = {}, {}
            for i = 1, #INPUTS do inputs[i] = { name = INPUTS[i], value = x[i] or 0 } end
            for o, a in ipairs(spec.actions) do
                outputs[o] = { name = a.name, percent = math.floor(p[o] * 1000 + 0.5) / 10, bias = net.b2[o],
                    drawn = state.counts[role][a.name] or 0, avg_reward = state.rewards[role][a.name] }
            end
            local prev = type(args.bot) == "string" and state.prev[args.bot] or nil
            return { role = role, bot = args.bot, generation = state.generation or 0,
                last_generation = (state.last_generation or {})[role], inputs = inputs, hidden = hidden, hidden_bias = net.b1, outputs = outputs, value = value,
                w1 = net.w1, w2 = net.w2, v = net.v, vb = net.vb, layers = net.layers, activations = activations,
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
