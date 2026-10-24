-- The bots in groups (two to a trade with 15 bots), each with a trade:
--   bakers     bread, pumpkin pies and cakes from the farmers' crops, eggs and milk; feed the hungriest
--   farmers    a crop farm each, eggs, pumpkins, milk (the berry picker is a farmer)
--   butchers   breed cows, sheep, pigs and chickens, slaughter some for meat and leather, shear sheep
--   miners     a team after ores, digging wide lit mines with stairs
--   builders   the castle team
--   soldiers   hunt hostile mobs, guard the leader, gear up
--   blacksmiths  smelt iron, make tools, buckets, torches and armor, and hand them out
--   fishers      fish at open water, cook the catch, feed the hungry
-- The trades hand each other what they make (the roles plugin's exchange).
--   explorers  go far for what the team has not found yet
-- What a bot does within its trade is drawn by that trade's own neural network (the roles plugin), each bot's
-- turn every minute.
--   groups {}                         groups everyone, once (and says who is what)
--   groups { show = true }            who is what
--   groups { stop = true }            everyone back to the castle
--   groups { assign = { bakers = {...}, ... } }   exactly these groups (the brain plugin decides them)
-- The groups are kept in a file and taken up again after a restart.
local FILE = "../groups_job.lua"
local ORES = "coal_ore,iron_ore,copper_ore,gold_ore,redstone_ore,lapis_ore,diamond_ore"
local TRADES = { "bakers", "farmers", "butchers", "miners", "builders", "soldiers", "blacksmiths", "explorers", "fishers", "lumberjacks", "carpenters" }

local groups = nil          -- { bakers = {...}, farmers = {...}, ... } (TRADES)
local notes = {}            -- name -> what it did last

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
    if not ok or type(g) ~= "table" then return nil end
    -- Saved under the trades' old names (hunters, then cooks; breeders; crafters)
    g.bakers, g.cooks, g.hunters = g.bakers or g.cooks or g.hunters, nil, nil
    g.butchers, g.breeders = g.butchers or g.breeders, nil
    g.blacksmiths, g.crafters = g.blacksmiths or g.crafters, nil
    for _, t in ipairs(TRADES) do g[t] = g[t] or {} end
    return g
end

-- Out of the castle team, for a trade of its own (a team of one)
local function solo(name)
    call("team_member", { bot = name, objective = "disband", members = { name }, leader = "", count = 1, solo = true })
end

-- Each bot's trade, for its status (the panels show it); "" for none
local function tell(g)
    for _, t in ipairs(TRADES) do for _, n in ipairs(g and g[t] or {}) do call("trade", { bot = n, trade = t }) end end
end

local function apply(g)
    notes = {}                  -- what they did in their old trades says nothing now
    tell(g)
    call("feeders", { stop = true })       -- the bakers do the feeding now, by their own network
    for _, t in ipairs(TRADES) do
        if t ~= "miners" and t ~= "builders" then for _, n in ipairs(g[t]) do solo(n) end end
    end
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

-- Every bot a turn a minute: its trade's network draws what it does. Each turn runs on its own thread
-- (bot.spawn), so a bot busy for minutes holds up neither the others nor the plugins; a bot still on its last
-- action has its next turn once that is done
local TURN = 60
local next_turn = {}       -- bot -> when its next turn is due
bot.every(5, function()
    if MIGRATING then return end      -- the bots are moving to a new place (migrate plugin)
    if not groups or not ROLES_ACT then return end
    local now = os.time()
    for _, trade in ipairs(TRADES) do
        for _, name in ipairs(groups[trade]) do
            -- The first turns spread over the minute
            next_turn[name] = next_turn[name] or now + math.random(0, TURN)
            if now >= next_turn[name] and not bot.busy(name) then
                next_turn[name] = now + TURN
                local members, all = groups[trade], groups
                bot.spawn(name, function()
                    -- Still covering its basics (shelter, wood, workbench, furnace, seeds, water, its field, bread): no turn
                    local okst, st = pcall(bot.call, "status", { bot = name })
                    if okst and type(st) == "table" and st.game_mode == "creative" and st.building then return end
                    if okst and type(st) == "table" and st.basics then
                        notes[name] = "basics first: " .. tostring(st.basics)
                        return
                    end
                    local ok, r = pcall(ROLES_ACT, name, trade, members, all)
                    notes[name] = ok and type(r) == "table" and (tostring(r.action or "skipped") .. ": " .. tostring(r.note)) or ("turn failed: " .. tostring(r))
                end)
            end
        end
    end
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
    description = "Put the bots in groups, each with a trade: bakers (bread, pies, cakes; feed the hungry), farmers (crop farms, eggs, pumpkins, milk), butchers (breed animals, slaughter for meat and leather, shear sheep), miners (a team after ores), builders (the castle), soldiers (hunt hostiles, guard the leader), blacksmiths (smelt iron, make tools, buckets, torches and armor and hand them out; the trades hand each other what they make), explorers (go far for new things). Within its trade each bot's next action is drawn by the trade's own neural network (see the roles tool). With no argument, makes the groups; show=true says who is what; stop=true sends everyone back to the castle team.",
    params = { show = "boolean", stop = "boolean", assign = "object" },
    run = function(args)
        local function empty() local g = {} for _, t in ipairs(TRADES) do g[t] = {} end return g end
        -- Given from outside (the brain plugin): { bakers = {...}, farmers = {...}, ... }
        if type(args.assign) == "table" then
            local g = empty()
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
                for _, k in ipairs(TRADES) do for _, n in ipairs(groups[k]) do call("trade", { bot = n, trade = "" }) end end
                for _, k in ipairs(TRADES) do
                    for _, n in ipairs(k ~= "builders" and groups[k] or {}) do
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
        -- Who goes where: the berry picker is a farmer, then in name order, two to a trade (a fisher only past 15 bots), the rest builders
        local names = {}
        for _, b in ipairs(call("bots", {}) or {}) do names[#names + 1] = b.name end
        local taken = {}
        local g = empty()
        local picker = call("berries", {})
        if type(picker) == "table" and picker.bot then g.farmers[1] = picker.bot; taken[picker.bot] = true end
        for _, n in ipairs(names) do
            if not taken[n] then
                for _, k in ipairs(TRADES) do
                    if k ~= "builders" and #g[k] < 2 and (k ~= "fishers" or (#names > 15 and #g[k] < 1)) then g[k][#g[k] + 1] = n; taken[n] = true; break end
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
