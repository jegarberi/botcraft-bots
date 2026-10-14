-- A berry picker: one bot, out of the team, whose whole job is a patch of sweet berry bushes. Every minute it
-- picks the ripe bushes (a right-click: the bush stays and grows again) and plants berries on the grass around
-- them, so the patch spreads. The berries are food the bots eat, and that the feeders hand out.
--   berries { bot = "Bot15" }            the patch nearest to it (seen by itself, or by any other bot)
--   berries { bot = "Bot15", x =, z = }  a given place
--   berries { stop = true }              back with the others
--   berries {}                           what it is doing
-- Kept in a file: after a restart of the program the picker goes back to its patch
local FILE = "../berries_job.lua"
local function save(p)
    if not p then os.remove(FILE) return end
    local f = io.open(FILE, "w")
    if f then
        f:write(string.format("return { bot = %q, x = %d, y = %d, z = %d, picked = %d, planted = %d, note = \"taken up again\" }", p.bot, p.x, p.y, p.z, p.picked, p.planted))
        f:close()
    end
end
local function restore()
    local f = io.open(FILE, "r")
    if not f then return nil end
    local chunk = load(f:read("a"), FILE, "t", {})
    f:close()
    local ok, p = pcall(chunk or function() end)
    return ok and type(p) == "table" and p or nil
end
local picker = restore()    -- { bot, x, y, z, picked, planted, note }
local busy = false
local PER_PASS = 8          -- bushes picked and berries planted per pass at most: the plugin waits on each call

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil
end

local function bushes(who, x, y, z)
    local list = call("find_blocks", { bot = who, block = "sweet_berry_bush", radius = 32, max = 64 }) or {}
    if x then
        local near = {}
        for _, b in ipairs(list) do
            if math.abs(b.x - x) <= 24 and math.abs(b.z - z) <= 24 then near[#near + 1] = b end
        end
        return near
    end
    return list
end

local function pass()
    local p = picker
    local st = call("status", { bot = p.bot })
    if not st or not st.position then return end
    local away = math.max(math.abs(st.position.x - p.x), math.abs(st.position.z - p.z))
    if away > 16 then
        p.note = "going to the patch at " .. p.x .. " " .. p.z
        call("travel", { bot = p.bot, x = p.x, z = p.z })
        call("goto", { bot = p.bot, x = p.x, y = p.y, z = p.z, tolerance = 3 })
        return
    end
    local list = bushes(p.bot, p.x, p.y, p.z)
    local picked, planted = 0, 0
    -- Ripe: age 2 or 3 (3 gives more)
    for _, b in ipairs(list) do
        if picked >= PER_PASS then break end
        local s = call("get_block", { bot = p.bot, x = b.x, y = b.y, z = b.z })
        if s and s.state and tonumber(s.state.age or "0") >= 2 then
            if call("interact_block", { bot = p.bot, x = b.x, y = b.y, z = b.z }) then picked = picked + 1 end
        end
    end
    -- Planting: on grass or dirt next to a bush, with room above
    local have = 0
    for _, slot in ipairs(call("inventory", { bot = p.bot }) or {}) do
        if slot.item == "minecraft:sweet_berries" then have = have + slot.count end
    end
    -- Keep a few to eat
    local spare = have - 4
    for _, b in ipairs(list) do
        if planted >= math.min(PER_PASS, spare) then break end
        for _, d in ipairs({ { 1, 0 }, { -1, 0 }, { 0, 1 }, { 0, -1 } }) do
            if planted >= math.min(PER_PASS, spare) then break end
            local x, z = b.x + d[1], b.z + d[2]
            local here = call("get_block", { bot = p.bot, x = x, y = b.y, z = z })
            local under = call("get_block", { bot = p.bot, x = x, y = b.y - 1, z = z })
            if here and under and here.block == "minecraft:air" and
                (under.block == "minecraft:grass_block" or under.block == "minecraft:dirt" or under.block == "minecraft:podzol" or under.block == "minecraft:coarse_dirt") then
                call("place_block", { bot = p.bot, item = "sweet_berries", x = x, y = b.y, z = z })
                local now = call("get_block", { bot = p.bot, x = x, y = b.y, z = z })
                if now and now.block == "minecraft:sweet_berry_bush" then planted = planted + 1 end
            end
        end
    end
    p.picked, p.planted = p.picked + picked, p.planted + planted
    save(p)
    p.note = #list .. " bushes; this pass picked " .. picked .. ", planted " .. planted .. " (" .. have .. " berries in the pack)"
end

bot.every(60, function()
    if not picker or busy then return end
    busy = true
    local ok, err = pcall(pass)
    busy = false
    if not ok then picker.note = "error: " .. tostring(err) end
end)

bot.tool{
    name = "berries",
    description = "Give one bot the job of a sweet berry patch, for good: every minute it picks the ripe bushes (they grow again) and plants berries on the grass around them. bot: which (default the last); x z: the patch, default the nearest one any bot can see; stop=true gives it back to the team; with no argument, what it is doing.",
    params = { bot = "string", x = "number", z = "number", stop = "boolean" },
    run = function(args)
        if args.stop then
            if picker then call("team_member", { bot = picker.bot, objective = "disband", members = {}, leader = "", count = 1, solo = false }) end
            picker = nil
            save(nil)
            return "the berry picker is back with the others"
        end
        if not args.bot then
            if not picker then return "no berry picker" end
            return { bot = picker.bot, patch = picker.x .. " " .. picker.y .. " " .. picker.z, picked = picker.picked, planted = picker.planted, doing = picker.note }
        end
        local who = args.bot
        local st = call("status", { bot = who })
        if not st or not st.position then error("no bot named " .. who) end
        local patch = nil
        if args.x and args.z then
            patch = { x = math.floor(args.x), y = math.floor(st.position.y), z = math.floor(args.z) }
        else
            -- The nearest bush any bot can see
            local best = nil
            for _, b in ipairs(call("bots", {}) or {}) do
                for _, bush in ipairs(bushes(b.name)) do
                    local d = (bush.x - st.position.x) ^ 2 + (bush.z - st.position.z) ^ 2
                    if not best or d < best.d then best = { x = bush.x, y = bush.y, z = bush.z, d = d } end
                end
            end
            if not best then error("no sweet berry bush in sight of any bot: give x and z") end
            patch = best
        end
        -- Out of the castle work: a team of one
        call("team_member", { bot = who, objective = "disband", members = { who }, leader = "", count = 1, solo = true })
        picker = { bot = who, x = patch.x, y = patch.y, z = patch.z, picked = 0, planted = 0, note = "starting" }
        save(picker)
        return { bot = who, patch = patch.x .. " " .. patch.y .. " " .. patch.z,
            note = "every minute it picks the ripe bushes there and plants berries around them (a few kept to eat)" }
    end,
}
