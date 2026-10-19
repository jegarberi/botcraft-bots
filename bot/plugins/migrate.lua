-- Migration: when the place the bots live in is poor in wood or food, the leader proposes moving away.
-- Every bot votes yes or no from what it sees around itself (trees, berry bushes, animals) and how fed it is.
-- With half + 1 yes votes they vote again for a direction: each bot points toward the wood and berries it
-- remembers farther away. Then they forget their beds (or the sleep rule walks them back every night), follow the
-- leader, and the leader walks that way a leg at a time until it stands where there is wood and food, at least
-- MIN_AWAY blocks from the old place. There the empire's town (and the castle, if one is being built) starts again,
-- the berry picker finds a patch near it, and every bot goes back to its trade.
-- While they travel, MIGRATING is set and the other plugins hand out no work.
--   migrate {}               what the last look saw, the last vote, the trip under way
--   migrate { vote = true }  look and vote now
--   migrate { stop = true }  end the trip and settle where the leader stands
local FILE = "../migrate_job.lua"
local CHECK = 1200          -- seconds between looks (one Minecraft day)
local REST = 3600           -- after settling, no new proposal for an hour
local LEG = 48              -- blocks the leader walks per leg, one leg every 30 seconds
local MAX_LEGS = 40         -- then they settle wherever they are
local MIN_AWAY = 150        -- the new place is at least this far from the old one
local WOOD, FOOD = 8, 3     -- what a good place has within 32 blocks of a bot: logs; berry bushes + animals
local DIRS = { "north", "north-east", "east", "south-east", "south", "south-west", "west", "north-west" }

MIGRATING = false           -- global on purpose: the other plugins' timers check it

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil, (not ok) and tostring(r) or nil
end

local state = { trip = nil, settled_at = nil, last_look = nil, last_vote = nil, log = {} }

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
        -- A trip under way goes on after a restart: the follow orders are given again first
        if state.trip then state.trip.ordered, MIGRATING = false, true end
    end
end

local function announce(text)
    table.insert(state.log, 1, os.date("%H:%M ") .. text)
    state.log[11] = nil
    local bots = call("bots", {}) or {}
    if bots[1] then call("chat", { bot = bots[1].name, message = "[migrate] " .. text }) end
end

local function names()
    local list = {}
    for _, b in ipairs(call("bots", {}) or {}) do list[#list + 1] = b.name end
    return list
end

local function leader_of(list)
    local l = call("leader", {})
    for _, n in ipairs(list) do if n == l then return l end end
    return list[1]
end

local function count(r) return type(r) == "table" and #r or 0 end

-- What one bot sees around itself
local function look(name)
    local st = call("status", { bot = name })
    if type(st) ~= "table" or type(st.position) ~= "table" then return nil end
    local logs = count(call("find_blocks", { bot = name, block = "_log", radius = 32, max = WOOD * 2 }))
    local berries = count(call("find_blocks", { bot = name, block = "sweet_berry_bush", radius = 32, max = 10 }))
    local animals = count(call("entities", { bot = name, type = "animal", radius = 32 }))
    return { logs = logs, food = berries + animals, hungry = (tonumber(st.food) or 20) <= 6, pos = st.position,
        team_leader = st.team_leader }
end

local function good(v) return v and v.logs >= WOOD and v.food >= FOOD end

-- The compass sector (1..8) from a step dx, dz (north is -z)
local function sector(dx, dz)
    local a = math.deg(math.atan(dx, -dz)) % 360
    return math.floor((a + 22.5) / 45) % 8 + 1
end

-- Where this bot would go: toward the wood and berries it remembers more than 64 blocks away; nil if none
local function points_to(name, pos)
    local sx, sz, n = 0, 0, 0
    for _, kind in ipairs({ "_log", "sweet_berry_bush" }) do
        for _, p in ipairs(call("recall", { bot = name, kind = kind, max = 30 }) or {}) do
            local dx, dz = p.x - pos.x, p.z - pos.z
            if dx * dx + dz * dz > 64 * 64 then sx, sz, n = sx + dx, sz + dz, n + 1 end
        end
    end
    if n > 0 then return sector(sx, sz) end
end

local function order_follow(list, leader)
    for _, n in ipairs(list) do
        call("team_member", { bot = n, objective = "follow", members = list, leader = leader, count = 1, solo = true })
    end
end

-- Look at every bot; if the place is poor, vote. force: vote whatever the place looks like
local function propose(force)
    local all = names()
    if #all < 3 then return "too few bots to vote" end
    local bots = {}
    for _, n in ipairs(all) do bots[n] = true end
    local views, logs, food, hungry = {}, 0, 0, 0
    for _, n in ipairs(all) do
        local v = look(n)
        if v then
            views[n] = v
            views[#views + 1] = n
            logs, food, hungry = logs + v.logs, food + v.food, hungry + (v.hungry and 1 or 0)
        end
    end
    local seen = #views
    if seen == 0 then return "no bot could look around" end
    state.last_look = { bots = seen, logs = logs / seen, food = food / seen, hungry = hungry }
    local poor = logs / seen < WOOD or food / seen < FOOD or hungry * 2 >= seen
    if not poor and not force then save() return "this place is good enough" end
    -- Each bot votes from its own surroundings: yes if they lack wood or food, or it is starving
    local yes = 0
    for _, n in ipairs(views) do
        local v = views[n]
        if not good(v) or v.hungry then yes = yes + 1 end
    end
    local need = seen // 2 + 1
    local vote = { yes = yes, no = seen - yes, need = need, passed = yes >= need }
    state.last_vote = vote
    local why = string.format("this place is poor (per bot: %.0f logs, %.0f berry bushes and animals, %d starving)",
        logs / seen, food / seen, hungry)
    if not vote.passed then
        announce(string.format("%s: move? %d yes, %d no, %d needed: we stay", why, yes, seen - yes, need))
        save()
        return vote
    end
    -- The direction: each bot points where it remembers wood and berries; the most pointed-to wins
    local tally = { 0, 0, 0, 0, 0, 0, 0, 0 }
    for _, n in ipairs(views) do
        local s = points_to(n, views[n].pos)
        if s then tally[s] = tally[s] + 1 end
    end
    local dir, most = math.random(8), 0
    for i, c in ipairs(tally) do if c > most then dir, most = i, c end end
    vote.direction, vote.pointed = DIRS[dir], most
    -- Bots out with a player (following one) stay out of it
    local going = {}
    for _, n in ipairs(views) do
        local tl = views[n].team_leader
        if not (type(tl) == "string" and tl ~= "" and not bots[tl]) then going[#going + 1] = n end
    end
    local leader = leader_of(going)
    local from = views[leader] and views[leader].pos or views[views[1]].pos
    state.trip = { dir = dir, leader = leader, bots = going, from = { x = from.x, y = from.y, z = from.z }, legs = 0, fails = 0, ordered = false }
    MIGRATING = true
    announce(string.format("%s: move? %d yes, %d no: we go %s (%d bots point there), following %s", why, yes, seen - yes,
        DIRS[dir], most, leader))
    save()
    return vote
end

-- The new place: the town (east of a castle, as the empire lays it out), the castle, the berries, the trades
local function settle(why)
    local trip = state.trip
    state.trip, MIGRATING, state.settled_at = nil, false, os.time()
    local st = call("status", { bot = trip.leader })
    local p = type(st) == "table" and type(st.position) == "table" and st.position or trip.from
    local x, y, z = math.floor(p.x), math.floor(p.y), math.floor(p.z)
    for _, n in ipairs(trip.bots) do
        call("team_member", { bot = n, objective = "disband", members = {}, leader = "", count = 1, solo = false })
    end
    local castle = call("castle", { action = "status" })
    if type(castle) == "table" and castle.building then
        local f = io.open("../castle_job.lua", "r")
        local saved = f and load(f:read("a"), "castle_job", "t", {}) or nil
        if f then f:close() end
        local ok, job = pcall(saved or function() end)
        local args = ok and type(job) == "table" and type(job.args) == "table" and job.args or { action = "start" }
        args.x, args.y, args.z = x, y, z
        call("castle", { action = "stop" })
        call("castle", args)
    end
    call("empire", { town = { x = x + 144, y = y, z = z - 30 } })
    local picker = call("berries", {})
    if type(picker) == "table" and picker.bot then call("berries", { bot = picker.bot }) end
    -- Every bot back to its trade (the groups again, from who is what)
    local g = call("groups", { show = true })
    if type(g) == "table" then
        local assign = {}
        for trade, list in pairs(g) do
            assign[trade] = {}
            if type(list) == "table" then
                for _, n in ipairs(list) do assign[trade][#assign[trade] + 1] = tostring(n):match("^[^:]+") end
            end
        end
        call("groups", { assign = assign })
    end
    announce(string.format("we settle at %d %d after %d legs %s: %s", x, z, trip.legs, DIRS[trip.dir], why))
    save()
end

-- One leg of the trip
local function leg()
    local trip = state.trip
    if not trip.ordered then
        for _, n in ipairs(trip.bots) do call("home", { bot = n, forget = true }) end
        order_follow(trip.bots, trip.leader)
        trip.ordered = true
    end
    local st = call("status", { bot = trip.leader })
    if type(st) ~= "table" or type(st.position) ~= "table" then return end
    local p = st.position
    local a = math.rad((trip.dir - 1) * 45)
    local tx, tz = math.floor(p.x + LEG * math.sin(a)), math.floor(p.z - LEG * math.cos(a))
    -- ponytail: travel holds the plugins' shared lock for the leg (under a minute); one leg per tick keeps it short
    local _, err = call("travel", { bot = trip.leader, x = tx, z = tz })
    trip.legs = trip.legs + 1
    if err then
        trip.fails = trip.fails + 1
        -- Blocked three times (sea, cliffs): turn one sector clockwise
        if trip.fails >= 3 then trip.dir, trip.fails = trip.dir % 8 + 1, 0 end
    else
        trip.fails = 0
    end
    local now = call("status", { bot = trip.leader })
    local at = type(now) == "table" and type(now.position) == "table" and now.position or p
    local dx, dz = at.x - trip.from.x, at.z - trip.from.z
    local here = look(trip.leader)
    if dx * dx + dz * dz >= MIN_AWAY * MIN_AWAY and good(here) then
        settle(string.format("%d logs and %d berry bushes and animals around", here.logs, here.food))
    elseif trip.legs >= MAX_LEGS then
        settle("too far without a better place")
    else
        save()
    end
end

local loaded_at, last_check, busy = os.time(), os.time(), false
bot.every(30, function()
    if busy then return end
    busy = true
    local ok, err = pcall(function()
        local now = os.time()
        if now - loaded_at < 150 then return end      -- the bots are still joining
        if state.trip then return leg() end
        if now - last_check >= CHECK and not (state.settled_at and now - state.settled_at < REST) then
            last_check = now
            propose(false)
        end
    end)
    busy = false
    if not ok then bot.log("[migrate] " .. tostring(err)) end
end)

bot.tool{
    name = "migrate",
    description = "Moving the bots to a better place. Every 20 minutes each bot looks around (logs, berry bushes and animals within 32 blocks, how fed it is); if the place is poor in wood or food they vote, each bot yes if its own surroundings lack them or it is starving; with half + 1 yes votes they pick a direction (each points toward the wood and berries it remembers farther away), forget their beds, follow the leader that way a leg at a time and settle where there is wood and food, at least 150 blocks away: the empire's town (and a castle under way) starts again there and every bot goes back to its trade. No new proposal for an hour after settling. With no argument: the last look, the last vote, the trip. vote=true looks and votes now (whatever the place looks like); stop=true settles where the leader stands.",
    params = { vote = "boolean", stop = "boolean" },
    run = function(args)
        if args.stop and state.trip then settle("stopped by hand") end
        if args.vote and not state.trip then
            local r = propose(true)
            if type(r) == "string" then return r end
        end
        local t = state.trip
        return { migrating = t ~= nil, trip = t and { direction = DIRS[t.dir], leader = t.leader, legs = t.legs, from = t.from, bots = #t.bots } or nil,
            last_look = state.last_look, last_vote = state.last_vote, events = state.log,
            next_look_in = math.max(0, CHECK - (os.time() - last_check)) }
    end,
}
