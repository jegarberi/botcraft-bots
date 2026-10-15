-- Offline check of the empire plugin: lua5.4 test_empire.lua plugins/empire.lua
-- 15 bots carrying more every minute: the split keeps the leader a builder and the berry picker a farmer, moves few
-- bots, and the team goes up the ages, researches, and its soldiers attack
assert(not io.open("../empire_job.lua"), "../empire_job.lua exists: run from a copy, or move it away")
local names, carry, assigned, chat, made, attacks = {}, {}, {}, {}, {}, 0
for i = 1, 15 do names[i] = { name = "Bot" .. i }; carry["Bot" .. i] = 0 end
local brain_on, tool, tick = true, nil, nil
bot = { call = function(name, a)
    if name == "bots" then return names end
    if name == "status" then
        local c = carry[a.bot]
        local inv = { bread = c, oak_log = c, cobblestone = c, raw_iron = c }
        if a.bot == "Bot10" then inv.iron_sword = 1; inv.iron_chestplate = 1 end
        return { inventory = inv, health = 20 }
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
    t = t + 6000
    for b in pairs(carry) do carry[b] = 300 end
    tick(); tick()
    local last = assigned[#assigned]
    local soldier = false
    for _, b in ipairs(last.soldiers) do soldier = soldier or b == "Bot10" end
    assert(#last.soldiers > 0 and soldier, "the leader picks the armed bot as a soldier")
    for _ = 1, 200 do
        t = t + 60
        for b in pairs(carry) do carry[b] = carry[b] + 1 end
        tick()
    end
    local s = tool.run({ stop = true })
    assert(s.age == "Imperial Age", "went up the ages, got " .. s.age)
    assert(brain_on, "the brain works again")
    assert(#chat >= 3 + 6 and #chat > 0, "ages and techs announced")
    assert(#made > 0 and attacks > 0, "techs made and soldiers sent")
    os.time = clock
    print("empire OK: " .. s.age .. ", " .. #chat .. " announcements, " .. #assigned .. " splits, " .. attacks .. " attack orders")
end)
os.remove("../empire_job.lua")
assert(ok, err)
