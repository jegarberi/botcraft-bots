-- Offline check of the empire plugin: lua5.4 test_empire.lua plugins/empire.lua
-- 15 bots carrying more every minute: the split keeps the leader a builder and the berry picker a farmer, moves few
-- bots, and the team goes up the ages, researches, and its soldiers attack
assert(not io.open("../empire_job.lua"), "../empire_job.lua exists: run from a copy, or move it away")
local names, carry, assigned, chat, made, attacks = {}, {}, {}, {}, {}, 0
for i = 1, 15 do names[i] = { name = "Bot" .. i }; carry["Bot" .. i] = 0 end
local brain_on, tool, tick = true, nil, nil
local building, builds = {}, {}
bot = { call = function(name, a)
    if name == "bots" then return names end
    if name == "status" then
        local c = carry[a.bot]
        local inv = { bread = c, oak_log = c, cobblestone = c, raw_iron = c }
        if a.bot == "Bot10" then inv.iron_sword = 1; inv.iron_chestplate = 1 end
        local b = (building[a.bot] or 0) > 0
        if b then building[a.bot] = building[a.bot] - 1 end
        return { inventory = inv, health = 20, building = b, build_left = b and 10 or 0, position = { x = 0, y = 64, z = 0 } }
    end
    if name == "leader" then return "Bot4" end
    if name == "berries" then return { bot = "Bot15" } end
    if name == "brain" then if a.stop then brain_on = false elseif a.start then brain_on = true end return { enabled = brain_on } end
    if name == "groups" then
        if a.assign then assigned[#assigned + 1] = a.assign return a.assign end
        return { builders = { "Bot4: building", "Bot1" }, farmers = { "Bot15", "Bot2" }, cooks = { "Bot12", "Bot6: cook meat: none" },
            breeders = { "Bot9", "Bot11" }, miners = { "Bot8" }, soldiers = { "Bot5", "Bot3" }, crafters = { "Bot13", "Bot7" },
            explorers = { "Bot10", "Bot14" } }
    end
    if name == "chat" then chat[#chat + 1] = a.message end
    if name == "make" then made[#made + 1] = a.item end
    if name == "team_member" and a.objective == "attack" then attacks = attacks + 1 end
    if name == "team_member" and a.objective == "build" then building[a.bot] = 2; builds[#builds + 1] = a end
    if name == "get_block" then return { block = a.y > 70 and "minecraft:air" or "minecraft:grass_block" } end
    return {}
end, tool = function(t) tool = t end, every = function(_, f) tick = f end, on_chat = function() end, log = print }
local ok, err = pcall(function()
    load(io.open(arg[1]):read("a"), "empire")()
    assert(tool.run({}).playing == false, "off until started")
    tick()
    assert(#assigned == 0, "nothing done while off")
    tool.run({ start = true })
    assert(not brain_on, "the brain rests while it plays")
    local clock = os.time
    local t = clock()
    os.time = function() return t end
    tick()
    local g = assigned[1]
    local n, where = 0, {}
    for k, list in pairs(g) do for _, b in ipairs(list) do n = n + 1; where[b] = k end end
    assert(n == 15, "every bot in one trade, got " .. n)
    assert(where.Bot4 == "builders" and where.Bot15 == "farmers", "leader a builder, picker a farmer")
    assert(#g.farmers == 4 and #g.soldiers == 0, "Dark Age, food short first: more farmers, no soldiers, got " .. #g.farmers .. " " .. #g.soldiers)
    assert(where.Bot12 == "cooks" and where.Bot8 == "miners", "few bots moved")
    assert(chat[1]:match("^%[empire%] Bot") and chat[1]:match("->"), "the leader tells the moves")
    for _ = 1, 400 do
        t = t + 60
        for b in pairs(carry) do carry[b] = carry[b] + 1 end
        tick()
    end
    -- The village: a house for every bot first, no trade meanwhile, then every bot in a trade again
    local before, splits = #builds, #assigned
    tool.run({ village = true })
    t = t + 60; tick()
    local homes = {}
    for i = before + 1, #builds do if builds[i].name:match("'s house$") then homes[builds[i].bot] = true end end
    local h = 0
    for _ in pairs(homes) do h = h + 1 end
    assert(h >= 14, "a house order for every bot (the empire's builder may finish first), got " .. h)
    local n_ = 0
    for _, list in pairs(assigned[#assigned]) do n_ = n_ + #list end
    assert(#assigned == splits or n_ < 15, "no trade while building its house")
    for _ = 1, 10 do t = t + 60; tick() end
    local v = tool.run({}).village
    for b in pairs(carry) do assert(v[b] and v[b]:match("^house at"), b .. " has its house: " .. tostring(v[b])) end
    n_ = 0
    for _, list in pairs(assigned[#assigned]) do n_ = n_ + #list end
    assert(n_ == 15, "every bot in a trade again, got " .. n_)
    local s = tool.run({ stop = true })
    assert(s.age == "Imperial Age", "went up the ages, got " .. s.age)
    assert(brain_on, "the brain works again")
    assert(#chat >= 3 + 6 and #chat > 0, "ages and techs announced")
    assert(#made > 0 and attacks > 0, "techs made and soldiers sent")
    assert(s.buildings.House == 3 and s.buildings.Wonder == 1, "3 houses for 15 bots and a wonder")
    local names_ = {}
    for _, b in ipairs(builds) do
        names_[b.name] = true
        assert((b.bot ~= "Bot4" or b.name:match("'s house$")) and #b.blocks > 0 and #b.blocks <= 4096, "a builder, not the leader, within the order's size")
        for _, e in ipairs(b.blocks) do if e[4] ~= "air" and e[4] ~= "cobblestone" then assert(e[2] >= 71, "built on the ground") end end
    end
    local soldier = false
    for _, b in ipairs(s.groups.soldiers) do soldier = soldier or b == "Bot10" end
    assert(soldier, "the leader picks the armed bot as a soldier")
    assert(names_.Mill and names_.Barracks and names_.Monastery, "buildings of every age")
    os.time = clock
    print("empire OK: " .. s.age .. ", " .. #chat .. " announcements, " .. #assigned .. " splits, " .. attacks .. " attack orders")
end)
os.remove("../empire_job.lua")
assert(ok, err)
