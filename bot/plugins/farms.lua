-- Automatic farms and the buildings that go with them, block by block (Java 26.x mechanics, designed from the
-- Minecraft Wiki tutorials: https://minecraft.wiki/w/Tutorial:Cactus_farming, Tutorial:Egg_farming, and the
-- usual hopper-furnace smelter). Each plan is drawn from its lowest corner at floor level (y 0 is where one
-- stands, y -1 the ground); "air" cells are cleared, cells left out are left as they are.
--   farms {}                                        the plans and what each one takes
--   farms { build = "cactus_farm", x =, y =, z = }  the team builds it there
--   farms { build = "egg_collector", bot = "Bot3" } that bot builds it alone, near where it stands
--   farms { built = true }                          the farms put up so far
-- The farmers build the cactus farm and the egg collector on their own, and lead chickens into it; the
-- blacksmiths build the smelter (roles plugin).
-- Kept in a file: who built what, where (one of each plan per bot)
local FILE = "../farms_job.lua"

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil, (not ok) and tostring(r) or nil
end

local function add(list, o, x, y, z, item) list[#list + 1] = { o.x + x, o.y + y, o.z + z, item } end

local PLANS = {
    cactus_farm = {
        size = { 5, 6, 5 },
        text = "Cactus on sand in the middle of a 5 x 5 basin. A fence beside the block the cactus grows into breaks each new"
            .. " block off as it grows; the drops fall into a ring of water that runs from one corner to the opposite one,"
            .. " where a hopper under the water passes them to a chest. No redstone. Some drops land on the cactus and burn"
            .. " (the wiki calls these farms not very efficient): build several for more.",
        blocks = function(o)
            local b = {}
            add(b, o, 3, -2, 3, "chest")
            for x = 0, 4 do for z = 0, 4 do
                local ring = x == 0 or x == 4 or z == 0 or z == 4
                add(b, o, x, -1, z, (x == 3 and z == 3) and "hopper[facing=down]" or "cobblestone")
                -- The basin: walls, the sand in the middle, water poured at one corner, the rest left for it to flow
                add(b, o, x, 0, z, ring and "cobblestone" or (x == 2 and z == 2) and "sand" or (x == 1 and z == 1) and "water" or "air")
                -- Glass round the cactus keeps the drops in; the fence post at the east goes up to the breaker
                add(b, o, x, 1, z, (x == 4 and z == 2) and "oak_fence" or ring and "glass" or (x == 2 and z == 2) and "cactus" or "air")
                add(b, o, x, 2, z, ((x == 4 or x == 3) and z == 2) and "oak_fence" or "air")
                add(b, o, x, 3, z, "air")
            end end
            return b
        end,
    },
    egg_collector = {
        size = { 3, 5, 3 },
        text = "A one-block pen of fences with a gate on the south side, its floor a hopper that drops into a chest below:"
            .. " every egg the chickens lay goes into the chest. Chickens are led in through the gate (lure), which is"
            .. " then shut. About 24 chickens fit before they crowd each other.",
        blocks = function(o)
            local b = {}
            add(b, o, 1, -2, 1, "chest")
            for x = 0, 2 do for z = 0, 2 do
                local middle = x == 1 and z == 1
                add(b, o, x, -1, z, middle and "hopper[facing=down]" or "cobblestone")
                add(b, o, x, 0, z, middle and "air" or (x == 1 and z == 2) and "oak_fence_gate[open=false]" or "oak_fence")
                add(b, o, x, 1, z, "air")
                add(b, o, x, 2, z, "air")
            end end
            return b
        end,
    },
    auto_smelter = {
        size = { 2, 5, 1 },
        text = "A furnace that runs by itself: what goes in the top chest drops through a hopper into the furnace, fuel in"
            .. " the side chest reaches it through a hopper from the side, and the smelted items go down a hopper into the"
            .. " bottom chest.",
        blocks = function(o)
            local b = {}
            add(b, o, 1, 0, 0, "chest")               -- what comes out
            add(b, o, 0, 0, 0, "cobblestone")
            add(b, o, 1, 1, 0, "hopper[facing=down]")
            add(b, o, 0, 1, 0, "cobblestone")
            add(b, o, 1, 2, 0, "furnace")
            add(b, o, 0, 2, 0, "hopper[facing=east]") -- fuel, into the side of the furnace
            add(b, o, 1, 3, 0, "hopper[facing=down]")
            add(b, o, 0, 3, 0, "chest")               -- fuel
            add(b, o, 1, 4, 0, "chest")               -- what goes in
            return b
        end,
    },
}

-- What a plan takes: blocks by item, water in buckets (not counting the cleared air)
local function materials(blocks)
    local m = {}
    for _, e in ipairs(blocks) do
        local item = e[4]:gsub("%[.*$", "")
        if item ~= "air" then m[item] = (m[item] or 0) + 1 end
    end
    return m
end

local function load_built()
    local f = io.open(FILE, "r")
    if not f then return {} end
    local chunk = load(f:read("a"), FILE, "t", {})
    f:close()
    local ok, t = pcall(chunk or function() end)
    return ok and type(t) == "table" and t or {}
end
local built = load_built()
local function save_built()
    local f = io.open(FILE, "w")
    if f then f:write("return ", NN.serialize(built)); f:close() end
end

-- A spot near (x, y, z) for a plan of this size, clear of every stored construction box by 2 blocks: rings of
-- candidates 6 to 30 blocks out. The builder levels the ground itself (the plans clear what is above y 0)
local function free_spot(who, x, y, z, size)
    local boxes = call("sites", {}) or {}
    -- Dry ground under each corner, no water or lava where it stands (the first one went up in a lake)
    local function block(bx, by, bz)
        local b = call("get_block", { bot = who, x = bx, y = by, z = bz })
        return type(b) == "table" and tostring(b.block) or nil
    end
    local function dry(cx, cz)
        for _, c in ipairs({ { 0, 0 }, { size[1] - 1, 0 }, { 0, size[3] - 1 }, { size[1] - 1, size[3] - 1 } }) do
            local ground, at = block(cx + c[1], y - 1, cz + c[2]), block(cx + c[1], y, cz + c[2])
            if not ground or not at or ground:match("water") or ground:match("lava") or ground:match(":air$") or
                at:match("water") or at:match("lava") then return false end
        end
        return true
    end
    local function clear(cx, cz)
        for _, s in ipairs(boxes) do
            if cx - 2 <= s.x2 and cx + size[1] + 1 >= s.x1 and cz - 2 <= s.z2 and cz + size[3] + 1 >= s.z1 and
                y - 3 <= s.y2 and y + size[2] >= s.y1 then return false end
        end
        return dry(cx, cz)
    end
    for r = 6, 30, 4 do
        for _, d in ipairs({ { r, 0 }, { -r, 0 }, { 0, r }, { 0, -r }, { r, r }, { -r, -r }, { r, -r }, { -r, r } }) do
            if clear(x + d[1], z + d[2]) then return { x = x + d[1], y = y, z = z + d[2] } end
        end
    end
    return nil
end

local function build(args)
    local p = PLANS[args.build]
    if not p then
        local names = {}
        for name in pairs(PLANS) do names[#names + 1] = name end
        table.sort(names)
        error("no plan named " .. tostring(args.build) .. " (there are: " .. table.concat(names, ", ") .. ")")
    end
    local o
    if args.x and args.y and args.z then
        o = { x = math.floor(args.x), y = math.floor(args.y), z = math.floor(args.z) }
    elseif args.bot then
        local st = call("status", { bot = args.bot })
        if not st or not st.position then error("no bot named " .. args.bot) end
        o = free_spot(args.bot, math.floor(st.position.x), math.floor(st.position.y), math.floor(st.position.z), p.size)
        if not o then error("no free spot within 30 blocks of " .. args.bot) end
    else
        error("give x y z, or the bot that builds it near itself")
    end
    local name = args.build:gsub("_", " ") .. (args.bot and (" of " .. args.bot) or "")
    local _, err
    if args.bot then
        _, err = call("team_member", { bot = args.bot, objective = "build", plan = "blocks", blocks = p.blocks(o),
            members = { args.bot }, leader = args.bot, solo = true, name = name })
    else
        _, err = call("team", { objective = "build", plan = "blocks", blocks = p.blocks(o), name = name })
    end
    if err then error(err) end
    built[#built + 1] = { plan = args.build, bot = args.bot, x = o.x, y = o.y, z = o.z }
    save_built()
    return { building = name, x = o.x, y = o.y, z = o.z, materials = materials(p.blocks(o)) }
end

bot.tool{
    name = "farms",
    description = "Automatic farms, block by block: cactus_farm (cactus broken off by a fence as it grows, washed by water into a hopper and a chest; no redstone), egg_collector (a one-block fence pen over a hopper and a chest: the eggs the chickens lay are collected; chickens are led in through its gate) and auto_smelter (chest, hopper, furnace, hopper, chest, with a fuel chest and hopper at the side). With no argument: each plan, its size and materials. build=<plan> with x y z (its lowest corner, at floor level): the team builds it there; build=<plan> with bot=<name>: that bot builds it alone at a free spot near itself. built=true: the farms put up so far. The farmers build the cactus farm and egg collector on their own and lead chickens into it, the blacksmiths the smelter.",
    params = { build = "string", bot = "string", x = "number", y = "number", z = "number", built = "boolean" },
    run = function(args)
        if args.built then return built end
        if args.build then return build(args) end
        local out = {}
        for name, p in pairs(PLANS) do
            out[name] = { size = table.concat(p.size, " x "), about = p.text, materials = materials(p.blocks({ x = 0, y = 0, z = 0 })) }
        end
        return out
    end,
}
