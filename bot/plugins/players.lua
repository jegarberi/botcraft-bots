-- Orders from the human players, in the game chat: "<bot name> <command>", or "all <command>" for every bot.
--   follow           follow the one who says it (out of its other work until "stay")
--   stay / stop      back to its own work
--   come             walk to the one who says it
--   attack [mob]     kill the nearest hostile mob (or that kind) within 24 blocks
--   farm             harvest and replant the crops around it
--   mine [ore]       go mining (default iron), in a wide lit mine with stairs
--   gather <block> [n], make <item> [n]
--   drop             throw everything but armor and tools toward the player, then stand still 10 s
--   store            put what it collected and its valuables in the chests (valuables go to the vault)
--   status           health, food, where it is and what it is doing
--   help             this list
-- A bot that is too far to see the player asks the server to teleport it there (/tpa, the player accepts).
local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil, (not ok) and tostring(r) or nil
end

local function say(name, text) call("chat", { bot = name, message = text }) end

local function bot_names()
    local names = {}
    for _, b in ipairs(call("bots", {}) or {}) do names[#names + 1] = b.name end
    return names
end

-- Where the player is, as this bot sees it (nil: out of sight)
local function player_at(name, player)
    for _, e in ipairs(call("entities", { bot = name, type = "player", radius = 160 }) or {}) do
        if e.name == player or e.player == player or e.username == player then return e.position end
    end
    return nil
end

-- Out of sight: ask for a teleport to the player (the player has to accept it)
local function reach(name, player)
    local at = player_at(name, player)
    if at then return at end
    call("chat", { bot = name, message = "/tpa " .. player })
    say(name, player .. ", I'm too far away to see you: I asked to teleport to you, accept it with /tpaccept")
    return nil
end

local commands = {}

commands.help = function(name, player)
    say(name, "orders: <bot or all> follow | stay | come | attack [mob] | farm | mine [ore] | gather <block> [n] | make <item> [n] | drop | store | status")
end

commands.follow = function(name, player)
    reach(name, player)
    call("team_member", { bot = name, objective = "follow", members = { name }, leader = player, count = 1, solo = true })
    say(name, "following you, " .. player .. " (say '" .. name .. " stay' to let me go back to work)")
end

commands.stay = function(name, player)
    call("team_member", { bot = name, objective = "disband", members = {}, leader = "", count = 1, solo = false })
    say(name, "ok, back to my work")
end
commands.stop = commands.stay

commands.come = function(name, player)
    local at = reach(name, player)
    if not at then return end
    local r, err = call("goto", { bot = name, x = at.x, y = at.y, z = at.z, tolerance = 2 })
    say(name, r and "here I am" or ("I can't get there: " .. tostring(err)))
end

commands.attack = function(name, player, words)
    local kind = words[1] or "hostile"
    local r, err = call("kill", { bot = name, type = kind, count = 1, radius = 24 })
    say(name, r and ("done: killed a " .. kind) or ("no " .. kind .. " I can reach within 24 blocks"))
end

commands.farm = function(name, player)
    local r, err = call("farm", { bot = name, radius = 16 })
    say(name, r and ("harvested " .. (r.harvested or 0) .. ", replanted " .. (r.replanted or 0)) or ("can't farm here: " .. tostring(err)))
end

commands.mine = function(name, player, words)
    local ore = words[1] or "iron_ore"
    if not ore:match("_ore$") then ore = ore .. "_ore" end
    local r, err = call("autopilot", { bot = name, enabled = true, gather = ore .. ",deepslate_" .. ore, count = tonumber(words[2]) or 8 })
    say(name, r and ("going mining for " .. ore) or ("can't: " .. tostring(err)))
end

commands.gather = function(name, player, words)
    if not words[1] then say(name, "gather what? e.g. '" .. name .. " gather oak_log 16'") return end
    local r, err = call("autopilot", { bot = name, enabled = true, gather = words[1], count = tonumber(words[2]) or 16 })
    say(name, r and ("gathering " .. words[1]) or ("can't: " .. tostring(err)))
end

commands.make = function(name, player, words)
    if not words[1] then say(name, "make what? e.g. '" .. name .. " make bread 4'") return end
    local r, err = call("make", { bot = name, item = words[1], count = tonumber(words[2]) or 1 })
    say(name, r and ("making " .. words[1]) or ("can't: " .. tostring(err)))
end

commands.drop = function(name, player)
    local r, err = call("drop", { bot = name, to = player })
    say(name, r and ("dropped " .. (r.dropped or 0) .. " stacks, waiting 10 s") or ("can't drop: " .. tostring(err)))
end

commands.store = function(name, player)
    local r, err = call("store", { bot = name })
    say(name, r and ("stored " .. (r.stored or 0) .. " stacks in the chests") or ("can't store: " .. tostring(err)))
end

commands.status = function(name, player)
    local st = call("status", { bot = name })
    if not st or not st.position then return end
    local p = st.position
    say(name, string.format("health %d, food %d, at %d %d %d, %s: %s", math.floor(st.health or 0), st.food or 0,
        math.floor(p.x), math.floor(p.y), math.floor(p.z), st.state or "?", (st.last_note or ""):sub(1, 80)))
end

bot.on_chat(function(line)
    local player, text = line:match("^<([^>]+)> (.+)$")
    if not player then return end
    local names = bot_names()
    for _, n in ipairs(names) do
        if n == player then return end          -- the bots' own talk
    end
    local words = {}
    for w in text:gmatch("%S+") do words[#words + 1] = w end
    if #words < 2 then return end
    local who, command = words[1]:lower(), words[2]:lower()
    local handler = commands[command]
    if not handler then return end
    local args = {}
    for i = 3, #words do args[#args + 1] = words[i] end
    for _, n in ipairs(names) do
        if who == "all" or who == "bots" or who == n:lower() then
            local ok, err = pcall(handler, n, player, args)
            if not ok then say(n, "something went wrong: " .. tostring(err)) end
        end
    end
end)
