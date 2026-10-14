-- Offline check of the brain: lua5.4 test_brain.lua plugins/brain.lua
-- 15 bots in a world where miners earn the most: the network must come to want mining, and on day 10
-- the bots elect a leader and say it in the chat
os.remove("../brain_state.lua")
local tool, ticks = nil, {}
local clock, now_time = 1000, 0
os.time = function() return clock end
local trade_of, stats = {}, {}
local chats = {}
for i = 1, 15 do stats["Bot" .. i] = { built = 0, collected = 0 } end
bot = { call = function(name, a)
    if name == "bots" then local l = {} for i = 1, 15 do l[i] = { name = "Bot" .. i } end return l end
    if name == "status" then
        local n = (a and a.bot) or "Bot1"
        return { name = n, health = 20, food = 18, day_time = now_time, inventory = { cobblestone = 10 }, stats = stats[n], deaths = 0, times_stuck = 0 }
    end
    if name == "groups" then
        for trade, list in pairs(a.assign) do for _, n in ipairs(list) do trade_of[n] = trade end end
        return a.assign
    end
    if name == "castle" then return { stage_number = 1 } end
    if name == "chat" then chats[#chats + 1] = a.message return "sent" end
    if name == "leader" then return a.name end
    return {}
end, tool = function(t) tool = t end, every = function(_, f) ticks[#ticks + 1] = f end, on_chat = function() end, log = print }
load(io.open(arg[1]):read("a"), "brain")()
local function tick() for _, f in ipairs(ticks) do f() end end
clock = clock + 200; tick()                                       -- first assignment
local function wanting(trade)
    local s, n = 0, 0
    for _, b in pairs(tool.run({}).bots) do s = s + b.wants_percent[trade]; n = n + 1 end
    return s / n
end
local before = wanting("miners")
for day = 1, 60 do
    -- One day's work: a miner's day is worth 40, anything else 10
    for n, s in pairs(stats) do s.collected = s.collected + ((trade_of[n] == "miners") and 40 or 10) end
    now_time = 20000; clock = clock + 600; tick()
    now_time = 100; clock = clock + 600; tick()                    -- the morning: a new day
end
local after = wanting("miners")
local st = tool.run({})
assert(st.day == 60, "60 days counted, got " .. tostring(st.day))
assert(after > before + 5, string.format("learnt to want mining: %.1f%% -> %.1f%%", before, after))
assert(st.leader and st.last_vote and st.last_vote.voters == 15, "a leader was voted")
local said = false
for _, m in ipairs(chats) do if m:match("^%[vote%]") then said = true end end
assert(said, "the vote is announced in the chat")
assert(io.open("../brain_state.lua"), "the brain is kept for a restart")
os.remove("../brain_state.lua")
print(string.format("brain plugin OK: wanting mining %.1f%% -> %.1f%%, leader %s (%d of %d votes)", before, after, st.leader, st.last_vote.votes, st.last_vote.voters))
