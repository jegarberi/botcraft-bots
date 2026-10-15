-- The bots in groups of three, each with a trade:
--   hunters   the feeders: hunt, cook, and take food to hungry bots (the feeders tool)
--   farmers   a crop farm each: harvest, replant, bone meal; wheat and carrots go to the breeders
--             (the berry picker, if there is one, is a farmer)
--   breeders  breed cows, sheep, pigs and chickens with the food they have
--   miners    a team of three after ores, digging wide lit mines with stairs
--   builders  the rest: the castle team
--   groups {}                         groups everyone, in threes, once (and says who is what)
--   groups { show = true }            who is what
--   groups { stop = true }            everyone back to the castle
--   groups { assign = { hunters = {...}, ... } }   exactly these groups (the brain plugin decides them)
-- The groups are kept in a file and taken up again after a restart.
local FILE = "../groups_job.lua"
local ORES = "coal_ore,iron_ore,copper_ore,gold_ore,redstone_ore,lapis_ore,diamond_ore"
local BREEDS = { { "cow", "minecraft:wheat" }, { "sheep", "minecraft:wheat" }, { "pig", "minecraft:carrot" }, { "chicken", "minecraft:wheat_seeds" } }

local groups = nil          -- { hunters = {...}, farmers = {...}, breeders = {...}, miners = {...}, builders = {...} }
local notes = {}            -- name -> what it did last
local busy = false

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil, (not ok) and r or nil
end

local function save(g)
    if not g then os.remove(FILE) return end
    local f = io.open(FILE, "w")
    if not f then return end
    local parts = {}
    for k, list in pairs(g) do
        local names = {}
        for _, n in ipairs(list) do names[#names + 1] = string.format("%q", n) end
        parts[#parts + 1] = k .. " = {" .. table.concat(names, ",") .. "}"
    end
    f:write("return {" .. table.concat(parts, ",") .. "}")
    f:close()
end

local function restore()
    local f = io.open(FILE, "r")
    if not f then return nil end
    local chunk = load(f:read("a"), FILE, "t", {})
    f:close()
    local ok, g = pcall(chunk or function() end)
    return ok and type(g) == "table" and g or nil
end

local function count(name, item)
    local n = 0
    for _, slot in ipairs(call("inventory", { bot = name }) or {}) do
        if slot.item == item then n = n + slot.count end
    end
    return n
end

-- Out of the castle team, for a trade of its own (a team of one)
local function solo(name)
    call("team_member", { bot = name, objective = "disband", members = { name }, leader = "", count = 1, solo = true })
end

local function apply(g)
    notes = {}                  -- what they did in their old trades says nothing now
    call("feeders", { bots = g.hunters })
    for _, n in ipairs(g.farmers) do solo(n) end
    for _, n in ipairs(g.breeders) do solo(n) end
    -- The miners: one team of their own, after ore, the first one leading
    for _, n in ipairs(g.miners) do
        call("team_member", { bot = n, objective = "gather", block = ORES, members = g.miners, leader = g.miners[1], count = 64, solo = true })
    end
    for _, n in ipairs(g.builders) do
        call("team_member", { bot = n, objective = "disband", members = {}, leader = "", count = 1, solo = false })
    end
    -- That cleared the builders' orders: the castle hands its current stage out again, to them too
    call("castle", { action = "rehand" })
end

-- Farmers: their farm (made once, where they stand, if there is no farmland about), harvested and replanted;
-- what the breeders lack, handed over
local function has_hoe(name)
    for _, slot in ipairs(call("inventory", { bot = name }) or {}) do
        if slot.item:match("_hoe$") then return true end
    end
    return false
end

local function farmer(name)
    local farmland = call("find_blocks", { bot = name, block = "farmland", radius = 16, max = 1 }) or {}
    if #farmland == 0 then
        -- A hoe first (stone, else wood): the make tool sets it as a goal the bot works toward
        if not has_hoe(name) then
            local r = call("make", { bot = name, item = "stone_hoe", count = 1 }) or call("make", { bot = name, item = "wooden_hoe", count = 1 })
            notes[name] = r and "making a hoe for its farm" or "can't make a hoe yet"
            return
        end
        local r, err = call("make_farm", { bot = name })
        if r then
            notes[name] = "made a farm: " .. (r.tilled or 0) .. " tilled"
        elseif tostring(err):match("no water") then
            -- No water near: a short walk in some direction (the plugins wait while it walks), to look again from there
            local st = call("status", { bot = name })
            if st and st.position then
                local a = math.random() * 2 * math.pi
                call("travel", { bot = name, x = math.floor(st.position.x + 24 * math.cos(a)), z = math.floor(st.position.z + 24 * math.sin(a)) })
            end
            notes[name] = "no water near for a farm: went looking further"
        else
            notes[name] = "could not make a farm: " .. tostring(err)
        end
        return
    end
    local r = call("farm", { bot = name, radius = 16 })
    notes[name] = r and ("farm: " .. (r.harvested or 0) .. " harvested, " .. (r.replanted or 0) .. " replanted") or "farm: nothing ripe"
    for _, b in ipairs(groups.breeders) do
        for _, food in ipairs({ "minecraft:wheat", "minecraft:carrot", "minecraft:wheat_seeds" }) do
            if count(name, food) >= 12 and count(b, food) < 4 then
                local given = call("give", { bot = name, to = b, item = food, count = 8 })
                if given then notes[name] = "gave 8 " .. food:gsub("minecraft:", "") .. " to " .. b end
                return
            end
        end
    end
end

-- Breeders: two of a kind for each animal they have the food for (a pair is ready again 5 minutes later)
local function breeder(name)
    local done = {}
    for _, kind in ipairs(BREEDS) do
        if count(name, kind[2]) >= 2 then
            local r = call("breed", { bot = name, type = kind[1], food = kind[2], radius = 24 })
            if r then done[#done + 1] = kind[1] end
        end
    end
    notes[name] = #done > 0 and ("bred " .. table.concat(done, ", ")) or "waiting for food from the farmers (wheat, carrots, seeds)"
end

-- One farmer or breeder a turn, every 15 seconds: the plugins share one lock, and all of them in a row (walks,
-- breeding) held it for minutes, the castle's stages, the brain's days and netlink's sending waiting meanwhile
local turn = 0
bot.every(15, function()
    if not groups or busy then return end
    local workers = {}
    for _, n in ipairs(groups.farmers) do workers[#workers + 1] = { n, farmer } end
    for _, n in ipairs(groups.breeders) do workers[#workers + 1] = { n, breeder } end
    if #workers == 0 then return end
    busy = true
    turn = turn % #workers + 1
    pcall(workers[turn][2], workers[turn][1])
    busy = false
end)

-- After a restart: once the bots are back, the groups again (their memories keep the rest)
local saved = restore()
local loaded_at = os.time()
bot.every(30, function()
    if not saved or os.time() - loaded_at < 150 then return end
    groups, saved = saved, nil
    apply(groups)
end)

bot.tool{
    name = "groups",
    description = "Put the bots in groups of three, each with a trade: hunters (the feeders: hunt, cook, feed the hungry), farmers (a crop farm each: harvest, replant, bone meal; they give wheat and carrots to the breeders), breeders (breed cows, sheep, pigs, chickens), miners (a team after ores, digging wide lit mines with stairs), builders (the rest: the castle). With no argument, makes the groups; show=true says who is what; stop=true sends everyone back to the castle team.",
    params = { show = "boolean", stop = "boolean", assign = "object" },
    run = function(args)
        -- Given from outside (the brain plugin): { hunters = {...}, farmers = {...}, ... }
        if type(args.assign) == "table" then
            local g = { hunters = {}, farmers = {}, breeders = {}, miners = {}, builders = {} }
            for k in pairs(g) do
                for _, n in ipairs(args.assign[k] or {}) do g[k][#g[k] + 1] = n end
            end
            groups = g
            save(g)
            apply(g)
            return g
        end
        if args.stop then
            if groups then
                call("feeders", { stop = true })
                for _, k in ipairs({ "farmers", "breeders", "miners" }) do
                    for _, n in ipairs(groups[k]) do
                        call("team_member", { bot = n, objective = "disband", members = {}, leader = "", count = 1, solo = false })
                    end
                end
            end
            groups = nil
            save(nil)
            return "everyone is back with the castle team"
        end
        if args.show or groups then
            if not groups then return "no groups yet" end
            local out = {}
            for k, list in pairs(groups) do
                local each = {}
                for _, n in ipairs(list) do each[#each + 1] = n .. (notes[n] and (": " .. notes[n]) or "") end
                out[k] = each
            end
            return out
        end
        -- Who goes where: the feeders are the hunters, the berry picker is a farmer, then in name order
        local names = {}
        for _, b in ipairs(call("bots", {}) or {}) do names[#names + 1] = b.name end
        local taken = {}
        local g = { hunters = {}, farmers = {}, breeders = {}, miners = {}, builders = {} }
        for _, f in ipairs(call("feeders", {}) or {}) do
            if #g.hunters < 3 then g.hunters[#g.hunters + 1] = f.bot; taken[f.bot] = true end
        end
        local picker = call("berries", {})
        if type(picker) == "table" and picker.bot then g.farmers[1] = picker.bot; taken[picker.bot] = true end
        for _, n in ipairs(names) do
            if not taken[n] then
                for _, k in ipairs({ "hunters", "farmers", "breeders", "miners" }) do
                    if #g[k] < 3 then g[k][#g[k] + 1] = n; taken[n] = true; break end
                end
                if not taken[n] then g.builders[#g.builders + 1] = n end
            end
        end
        groups = g
        save(g)
        apply(g)
        return g
    end,
}
