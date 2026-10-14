-- The bots' brain: a small neural network that picks each bot's trade every Minecraft day, trained by
-- reinforcement on what the bot and the whole team achieved, and a vote for the leader every 10 days.
--
-- The network (one for all the bots, so it learns from 15 examples a day instead of 1):
--   in   17 numbers about the bot and the team: health, hunger, which tools it has, food, wood and stone it
--        carries, how many bots are hungry, how the castle advanced, the trade it had (one-hot), a bias
--   ->   10 hidden units (tanh)
--   out  5 trades: hunter, farmer, breeder, miner, builder (softmax: how much it wants each)
-- Every day: each bot draws a trade from its wants (that is the exploring), groups stay of three (the
-- most decided bots get their choice first), and the groups plugin puts them to work.
-- At the end of the day, the reward of each bot is half its own work and half the team's:
--   own   built + collected + harvested + 3 x killed + 5 x bred + given / 2 + stored / 20
--         - 20 x deaths - 2 x times stuck                  (from the "stats" in its status)
--   team  everyone's work + 50 x castle stages finished + how well fed the team is
-- and the network is moved toward the trades that did better than usual (REINFORCE, with a running
-- average as baseline).
-- Every 10 days each bot votes for a leader: the candidates' own results over those days, how the team did
-- while each of them led, a bonus to the current leader if the team did better under it, and a little of
-- each voter's own judgement (noise). The winner leads the team (leader tool), is a builder, and is
-- announced in the chat. All of it is kept in a file: the learning goes on after a restart.
--   brain {}                  what it knows: day, leader, last vote, each bot's wants and rewards
--   brain { vote = true }     vote now
--   brain { stop = true } / brain { start = true }
local FILE = "../brain_state.lua"
local TRADES = { "hunters", "farmers", "breeders", "miners", "builders" }
local IN, HID, OUT = 17, 10, 5
local LEARNING_RATE = 0.1
local ENTROPY = 0.2            -- keeps some doubt: a bot sure of one trade would never try another
local VOTE_EVERY = 10

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil
end

-- ---------------------------------------------------------------- saving
local function serialize(v)
    if type(v) == "table" then
        local parts = {}
        for k, x in pairs(v) do parts[#parts + 1] = "[" .. serialize(k) .. "]=" .. serialize(x) end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    if type(v) == "string" then return string.format("%q", v) end
    if type(v) == "number" then return string.format("%.17g", v) end
    return tostring(v)
end

local function save(state)
    local f = io.open(FILE, "w")
    if f then f:write("return ", serialize(state)); f:close() end
end

local function restore()
    local f = io.open(FILE, "r")
    if not f then return nil end
    local chunk = load(f:read("a"), FILE, "t", { math = math })
    f:close()
    local ok, state = pcall(chunk or function() end)
    return ok and type(state) == "table" and state or nil
end

-- ---------------------------------------------------------------- the network
local function new_network()
    local rng = 12345
    local function rand()             -- the same start every time (no math.random state to depend on)
        rng = (rng * 1103515245 + 12345) % 2147483648
        return rng / 2147483648 - 0.5
    end
    local net = { w1 = {}, b1 = {}, w2 = {}, b2 = {} }
    for h = 1, HID do
        net.w1[h] = {}
        for i = 1, IN do net.w1[h][i] = rand() * 0.6 end
        net.b1[h] = 0
    end
    for o = 1, OUT do
        net.w2[o] = {}
        for h = 1, HID do net.w2[o][h] = rand() * 0.6 end
        net.b2[o] = 0
    end
    return net
end

local function forward(net, x)
    local hidden = {}
    for h = 1, HID do
        local s = net.b1[h]
        for i = 1, IN do s = s + net.w1[h][i] * x[i] end
        hidden[h] = math.tanh and math.tanh(s) or ((math.exp(s) - math.exp(-s)) / (math.exp(s) + math.exp(-s)))
    end
    local logits, top = {}, -math.huge
    for o = 1, OUT do
        local s = net.b2[o]
        for h = 1, HID do s = s + net.w2[o][h] * hidden[h] end
        logits[o] = s
        if s > top then top = s end
    end
    local p, sum = {}, 0
    for o = 1, OUT do p[o] = math.exp(logits[o] - top); sum = sum + p[o] end
    for o = 1, OUT do p[o] = p[o] / sum end
    return p, hidden
end

-- One step of REINFORCE: the log-probability of the chosen trade, up by the advantage
local function learn(net, x, action, advantage)
    local p, hidden = forward(net, x)
    local clip = math.max(-2, math.min(2, advantage))
    -- The policy gradient, plus the gradient of the entropy (how spread the wants are)
    local entropy = 0
    for o = 1, OUT do entropy = entropy - p[o] * math.log(p[o] + 1e-12) end
    local dlogit = {}
    for o = 1, OUT do
        dlogit[o] = clip * (((o == action) and 1 or 0) - p[o]) - ENTROPY * p[o] * (math.log(p[o] + 1e-12) + entropy)
    end
    clip = 1
    local dhidden = {}
    for h = 1, HID do
        local s = 0
        for o = 1, OUT do s = s + net.w2[o][h] * dlogit[o] end
        dhidden[h] = s * (1 - hidden[h] * hidden[h])
    end
    for o = 1, OUT do
        for h = 1, HID do net.w2[o][h] = net.w2[o][h] + LEARNING_RATE * clip * dlogit[o] * hidden[h] end
        net.b2[o] = net.b2[o] + LEARNING_RATE * clip * dlogit[o]
    end
    for h = 1, HID do
        for i = 1, IN do net.w1[h][i] = net.w1[h][i] + LEARNING_RATE * clip * dhidden[h] * x[i] end
        net.b1[h] = net.b1[h] + LEARNING_RATE * clip * dhidden[h]
    end
end

-- ---------------------------------------------------------------- what the bots see
local function inventory_count(st, test)
    local n = 0
    for item, count in pairs(type(st.inventory) == "table" and st.inventory or {}) do
        if test(item) then n = n + (tonumber(count) or 0) end
    end
    return n
end

local FOOD = { bread = true, cooked_beef = true, cooked_porkchop = true, cooked_mutton = true, cooked_chicken = true, baked_potato = true,
    carrot = true, apple = true, beef = true, porkchop = true, mutton = true, golden_carrot = true, sweet_berries = true, potato = true }

local function features(st, trade, team)
    local function has(suffix) return inventory_count(st, function(i) return i:sub(-#suffix) == suffix end) > 0 and 1 or 0 end
    local x = {
        1,
        (st.health or 20) / 20,
        (st.food or 20) / 20,
        has("_pickaxe"), has("_axe"), has("_sword"), has("_hoe"),
        math.min(1, inventory_count(st, function(i) return FOOD[i] end) / 16),
        math.min(1, inventory_count(st, function(i) return i:match("_log$") or i:match("_planks$") end) / 64),
        math.min(1, inventory_count(st, function(i) return i == "cobblestone" or i == "stone" end) / 64),
        team.hungry,
        team.castle,
    }
    for t = 1, OUT do x[#x + 1] = (t == trade) and 1 or 0 end
    return x
end

local WEIGHTS = { built = 1, collected = 1, harvested = 1, killed = 3, bred = 5, given = 0.5, stored = 0.05 }

-- The bot's own score for the day: what its counters grew by (a counter that went down was reset by a
-- restart of its process: then all of it is new)
local function own_score(now, before)
    local score = 0
    local stats, old = now.stats or {}, (before and before.stats) or {}
    for what, w in pairs(WEIGHTS) do
        local a, b = tonumber(stats[what]) or 0, tonumber(old[what]) or 0
        score = score + w * (a >= b and a - b or a)
    end
    local function delta(key)
        local a, b = tonumber(now[key]) or 0, tonumber(before and before[key]) or 0
        return a >= b and a - b or a
    end
    return score - 20 * delta("deaths") - 2 * delta("times_stuck")
end

-- ---------------------------------------------------------------- the state
local state = restore() or {
    enabled = true,
    day = 0,                 -- Minecraft days seen
    last_time = nil,         -- the time of day at the last look, to see a new day start
    last_day_at = os.time(),
    net = new_network(),
    baseline = 0,            -- running average of the rewards
    own_scale = 10,          -- running average size of the own and team scores, to keep the rewards near 1
    team_scale = 100,
    trades = {},             -- bot -> trade index it has today
    inputs = {},             -- bot -> the input it chose from today
    before = {},             -- bot -> its status figures at the start of the day
    castle_before = nil,
    history = {},            -- the last 10 days: { leader, team, own = { bot -> score } }
    leader = nil,
    last_vote = nil,
}

local function snapshot(st)
    local stats = {}
    for k, v in pairs(st.stats or {}) do stats[k] = v end
    return { stats = stats, deaths = st.deaths or 0, times_stuck = st.times_stuck or 0 }
end

local function statuses()
    local list = {}
    for _, b in ipairs(call("bots", {}) or {}) do
        local st = call("status", { bot = b.name })
        if type(st) == "table" and st.name then list[#list + 1] = st end
    end
    return list
end

local function castle_stage()
    local st = call("castle", { action = "status" })
    return type(st) == "table" and (st.stage_number or 0) or 0
end

-- Which bot does what today: each draws from its wants, the most decided first, three to a trade
-- (the leader is a builder, the berry picker a farmer)
local function assign(list, team)
    local capacity = math.max(1, math.ceil(#list / OUT))
    local picker = call("berries", {})
    picker = type(picker) == "table" and picker.bot or nil
    local wants = {}
    for _, st in ipairs(list) do
        local x = features(st, state.trades[st.name] or 5, team)
        local p = forward(state.net, x)
        -- Drawn at random by the wants: what is not tried is never learnt
        local r, pick = math.random(), OUT
        for o = 1, OUT do
            r = r - p[o]
            if r <= 0 then pick = o; break end
        end
        wants[#wants + 1] = { name = st.name, x = x, p = p, pick = pick, sure = p[pick] }
    end
    table.sort(wants, function(a, b) return a.sure > b.sure end)
    local groups, taken = { {}, {}, {}, {}, {} }, {}
    state.drawn = state.drawn or {}
    local function place(w, trade)
        groups[trade][#groups[trade] + 1] = w.name
        state.trades[w.name] = trade
        state.inputs[w.name] = w.x
        -- Learnt from only if it is the trade it drew: one it was given for lack of room says nothing of its wants
        state.drawn[w.name] = (trade == w.pick)
        taken[w.name] = true
    end
    for _, w in ipairs(wants) do
        if w.name == state.leader then place(w, 5)
        elseif w.name == picker then place(w, 2) end
    end
    for _, w in ipairs(wants) do
        if not taken[w.name] then
            local order = { w.pick }
            for o = 1, OUT do if o ~= w.pick then order[#order + 1] = o end end
            table.sort(order, function(a, b) if a == w.pick then return true elseif b == w.pick then return false end return w.p[a] > w.p[b] end)
            for _, o in ipairs(order) do
                if #groups[o] < capacity then place(w, o); break end
            end
            if not taken[w.name] then place(w, 5) end
        end
    end
    local out = {}
    for t = 1, OUT do out[TRADES[t]] = groups[t] end
    call("groups", { assign = out })
    return out, wants
end

-- ---------------------------------------------------------------- the vote
local function vote(list)
    local days = state.history
    local own_total, led, team_all = {}, {}, {}
    for _, d in ipairs(days) do
        team_all[#team_all + 1] = d.team
        for name, s in pairs(d.own or {}) do own_total[name] = (own_total[name] or 0) + s end
        if d.leader then
            led[d.leader] = led[d.leader] or {}
            led[d.leader][#led[d.leader] + 1] = d.team
        end
    end
    local function mean(t) local s = 0 for _, v in ipairs(t) do s = s + v end return #t > 0 and s / #t or 0 end
    local function spread(t) local m, s = mean(t), 0 for _, v in ipairs(t) do s = s + (v - m) ^ 2 end return math.sqrt(s / math.max(1, #t)) + 1e-6 end
    local names = {}
    for _, st in ipairs(list) do names[#names + 1] = st.name end
    local owns, leads = {}, {}
    for _, n in ipairs(names) do
        owns[#owns + 1] = own_total[n] or 0
        leads[#leads + 1] = led[n] and mean(led[n]) or mean(team_all)
    end
    local m_own, s_own, m_lead, s_lead = mean(owns), spread(owns), mean(leads), spread(leads)
    -- Did the team do better under the current leader than before it? A bonus to keep it
    local half = math.floor(#days / 2)
    local first, second = {}, {}
    for i, d in ipairs(days) do if i <= half then first[#first + 1] = d.team else second[#second + 1] = d.team end end
    local improving = #first > 0 and mean(second) >= mean(first)
    local tally = {}
    for _, voter in ipairs(names) do
        local best, best_score = nil, -math.huge
        for i, c in ipairs(names) do
            local score = 0.6 * (owns[i] - m_own) / s_own + 0.4 * (leads[i] - m_lead) / s_lead
            if c == state.leader and improving then score = score + 0.5 end
            -- Each one's own judgement: no two voters see it quite the same
            score = score + (math.random() - 0.5) * 0.6
            if score > best_score then best, best_score = c, score end
        end
        tally[best] = (tally[best] or 0) + 1
    end
    local winner, most = state.leader, -1
    for c, n in pairs(tally) do
        if n > most or (n == most and c == state.leader) then winner, most = c, n end
    end
    local result = { day = state.day, winner = winner, votes = most, voters = #names, tally = tally, kept = (winner == state.leader) }
    state.leader = winner
    state.last_vote = result
    call("leader", { name = winner })
    call("chat", { bot = winner, message = string.format("[vote] day %d: %s %s leader with %d of %d votes", state.day, winner,
        result.kept and "stays" or "is the new", most, #names) })
    return result
end

-- ---------------------------------------------------------------- the day
local function end_of_day()
    local list = statuses()
    if #list == 0 then return end
    local stage = castle_stage()
    local castle_delta = state.castle_before and math.max(0, stage - state.castle_before) or 0
    -- Scores: each bot's, and the team's
    local own, team_sum, fed = {}, 0, 0
    for _, st in ipairs(list) do
        own[st.name] = own_score(st, state.before[st.name])
        team_sum = team_sum + own[st.name]
        fed = fed + (st.food or 0) / 20
    end
    local team = team_sum + 50 * castle_delta + 20 * fed / #list
    state.history[#state.history + 1] = { leader = state.leader, team = team, own = own }
    while #state.history > VOTE_EVERY do table.remove(state.history, 1) end
    -- Learning: the trades of today, judged by half the bot's own score and half the team's
    state.own_scale = 0.9 * state.own_scale + 0.1 * math.max(1, team_sum / #list)
    state.team_scale = 0.9 * state.team_scale + 0.1 * math.max(1, math.abs(team))
    local rewards, sum, n = {}, 0, 0
    for _, st in ipairs(list) do
        if state.inputs[st.name] and state.trades[st.name] then
            rewards[st.name] = 0.5 * own[st.name] / state.own_scale + 0.5 * team / state.team_scale
            sum, n = sum + rewards[st.name], n + 1
        end
    end
    -- Each bot's choice is judged against the day's average (the team half is the same for everyone: it
    -- moves the baseline, kept for the record, and how the whole day compares with the usual)
    local mean = n > 0 and sum / n or 0
    for name, r in pairs(rewards) do
        if (state.drawn or {})[name] ~= false then
            learn(state.net, state.inputs[name], state.trades[name], (r - mean) + 0.25 * (mean - state.baseline))
        end
    end
    state.baseline = 0.9 * state.baseline + 0.1 * mean
    state.last_rewards = rewards
    state.last_team = team
    -- The new day: figures to compare with tomorrow, then the trades
    state.day = state.day + 1
    for _, st in ipairs(list) do state.before[st.name] = snapshot(st) end
    state.castle_before = stage
    if state.day % VOTE_EVERY == 0 then vote(list) end
    local hungry = 0
    for _, st in ipairs(list) do if (st.food or 20) <= 8 then hungry = hungry + 1 end end
    local groups = assign(list, { hungry = hungry / #list, castle = math.min(1, castle_delta / 5) })
    state.last_groups = groups
    save(state)
end

-- A new day when the time of day goes back to the morning (sleeping through the night does that too);
-- if the clock gives nothing for 25 minutes (20 is a day), a day anyway
local busy = false
local loaded_at = os.time()
bot.every(30, function()
    if not state.enabled or busy then return end
    busy = true
    -- The very first time: trades for everyone as soon as the bots are in (no waiting for a day to end)
    if not state.started and os.time() - loaded_at >= 150 then
        local list = statuses()
        if #list > 0 then
            for _, st in ipairs(list) do state.before[st.name] = snapshot(st) end
            state.castle_before = castle_stage()
            state.last_groups = assign(list, { hungry = 0, castle = 0 })
            state.started = true
            save(state)
        end
    end
    local st = call("status", {})
    local time = type(st) == "table" and tonumber(st.day_time) or nil
    local new_day = false
    if time and state.last_time and time < state.last_time then new_day = true end
    if os.time() - (state.last_day_at or 0) > 1500 then new_day = true end
    if time then state.last_time = time end
    if new_day then
        state.last_day_at = os.time()
        local ok, err = pcall(end_of_day)
        if not ok then io.stderr:write("[brain] ", tostring(err), "\n") end
    end
    busy = false
end)

bot.tool{
    name = "brain",
    description = "The bots' brain: a small neural network (17 inputs about the bot and the team, 10 hidden, 5 outputs) shared by all bots picks each bot's trade every Minecraft day (hunter, farmer, breeder, miner, builder; groups of three), and learns by reinforcement from what each bot and the whole team achieved that day (the stats in their status, the castle's progress, how fed they are). Every 10 days the bots vote for a leader from each one's results and how the team did under each leader. With no argument: the day, the leader, the last vote, and each bot's wants, trade and last reward. vote=true votes now; stop/start.",
    params = { vote = "boolean", stop = "boolean", start = "boolean" },
    run = function(args)
        if args.stop then state.enabled = false; save(state); return "the brain rests: the groups stay as they are" end
        if args.start then state.enabled = true; save(state); return "the brain is working" end
        if args.vote then
            local list = statuses()
            if #list == 0 then error("no bot") end
            local r = vote(list)
            save(state)
            return r
        end
        local bots = {}
        local hungry = 0
        local list = statuses()
        for _, st in ipairs(list) do if (st.food or 20) <= 8 then hungry = hungry + 1 end end
        for _, st in ipairs(list) do
            local p = forward(state.net, features(st, state.trades[st.name] or 5, { hungry = hungry / math.max(1, #list), castle = 0 }))
            local wants = {}
            for o = 1, OUT do wants[TRADES[o]] = math.floor(p[o] * 100 + 0.5) end
            bots[st.name] = { trade = TRADES[state.trades[st.name] or 5], wants_percent = wants,
                last_reward = state.last_rewards and state.last_rewards[st.name] }
        end
        return { enabled = state.enabled, day = state.day, next_vote_in_days = VOTE_EVERY - state.day % VOTE_EVERY,
            leader = state.leader, last_vote = state.last_vote, team_score_last_day = state.last_team, bots = bots }
    end,
}
