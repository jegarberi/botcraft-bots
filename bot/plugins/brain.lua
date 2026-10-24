-- The bots' brain: a small neural network that assigns each bot a lasting trade, trained by
-- reinforcement on what the bot and the whole team achieved, and a vote for the leader every 10 days.
--
-- Every bot has its own network; every 5 days the best two seed mutated copies for the others.
--   in   116 numbers about the bot and the team: health, hunger, which tools it has, food, wood and stone it
--        carries, how many bots are hungry, how the castle advanced, the trade it had (one-hot), a bias
--   ->   five hidden layers of 10 units (tanh with residual connections)
--   out  11 trades: baker, farmer, butcher, miner, builder, soldier, blacksmith, explorer (softmax: how much it
--        wants each); within its trade, the trade's own network picks what it does (the roles plugin)
-- New bots draw a trade from their wants; assigned bots keep their trades across days and leader votes.
-- At the end of the day, the reward of each bot is half its own work and half the team's:
--   own   built + collected + harvested + 3 x killed + 5 x bred + given / 2 + stored / 20
--         - 20 x deaths - 2 x times stuck                  (from the "stats" in its status)
--   team  everyone's work + 50 x castle stages finished + how well fed the team is
-- and the network is moved toward the trades that did better than usual (REINFORCE, with a running
-- average as baseline).
-- Every 10 days each bot votes for a leader: the candidates' own results over those days, how the team did
-- while each of them led, a bonus to the current leader if the team did better under it, and a little of
-- each voter's own judgement (noise). The winner leads the team (leader tool), keeps its trade, and is
-- announced in the chat. All of it is kept in a file: the learning goes on after a restart.
--   brain {}                  what it knows: day, leader, last vote, each bot's wants and rewards
--   brain { vote = true }     vote now
--   brain { stop = true } / brain { start = true }
local FILE = "../brain_state.lua"
-- The trades (in the places of the cooks, breeders and crafters before them: the networks' outputs keep their
-- meaning; each trade has its own network for its actions, see the roles plugin)
local TRADES = { "bakers", "farmers", "butchers", "miners", "builders", "soldiers", "blacksmiths", "explorers", "fishers", "lumberjacks", "carpenters" }
local IN, HID, OUT, EXTRA_LAYERS = 116, 10, 11, 4
local LEARNING_RATE = 0.1
local ENTROPY = 0.2            -- keeps some doubt: a bot sure of one trade would never try another
local VOTE_EVERY = 10
local VALUE_RATE = 0.2        -- how fast the critic follows the rewards
-- What a network is made of, sent with it: one of another shape is never merged into this one
local ARCH = "brain:116x10x10x10x10x10x11+critic:environment:v7"
local GENERATION_DAYS = 5      -- every 5 days the best two seed the others
local MUTATION = 0.05          -- with this much noise (a normal spread) on every weight of the copies

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
-- Any size: the sizes are read from the network itself (the roles plugin uses the same code, global NN)
-- Zero-initialized residual layers preserve the policy when upgrading saved networks.
local function deepen(net)
    net.layers = net.layers or {}
    for l = #net.layers + 1, EXTRA_LAYERS do
        local layer = { w = {}, b = {} }
        for h = 1, #net.w1 do
            layer.w[h], layer.b[h] = {}, 0
            for i = 1, #net.w1 do layer.w[h][i] = 0 end
        end
        net.layers[l] = layer
    end
    return net
end

local function new_network(n_in, n_hid, n_out, seed)
    n_in, n_hid, n_out = n_in or IN, n_hid or HID, n_out or OUT
    local rng = seed or 12345
    local function rand()             -- the same start every time (no math.random state to depend on)
        rng = (rng * 1103515245 + 12345) % 2147483648
        return rng / 2147483648 - 0.5
    end
    local net = { w1 = {}, b1 = {}, w2 = {}, b2 = {} }
    for h = 1, n_hid do
        net.w1[h] = {}
        for i = 1, n_in do net.w1[h][i] = rand() * 0.6 end
        net.b1[h] = 0
    end
    for o = 1, n_out do
        net.w2[o] = {}
        for h = 1, n_hid do net.w2[o][h] = rand() * 0.6 end
        net.b2[o] = 0
    end
    -- The critic: what a day usually brings from such a situation (a linear head on the same hidden layer)
    net.v, net.vb = {}, 0
    for h = 1, n_hid do net.v[h] = 0 end
    return deepen(net)
end

local function copy_net(net)
    local out = { w1 = {}, b1 = {}, w2 = {}, b2 = {}, v = {}, vb = net.vb or 0 }
    for h = 1, #net.w1 do
        out.w1[h] = {}
        for i = 1, #net.w1[h] do out.w1[h][i] = net.w1[h][i] end
        out.b1[h] = net.b1[h]
        out.v[h] = net.v and net.v[h] or 0
    end
    for o = 1, #net.w2 do
        out.w2[o] = {}
        for h = 1, #net.w2[o] do out.w2[o][h] = net.w2[o][h] end
        out.b2[o] = net.b2[o]
    end
    out.layers = {}
    for l, layer in ipairs(deepen(net).layers) do
        out.layers[l] = { w = {}, b = {} }
        for h, row in ipairs(layer.w) do
            out.layers[l].w[h] = { table.unpack(row) }
            out.layers[l].b[h] = layer.b[h]
        end
    end
    return out
end

-- A copy with a little noise on every weight (Box-Muller normal numbers)
local function mutated(net, sigma)
    local out = copy_net(net)
    sigma = sigma or MUTATION
    local function n() return sigma * math.sqrt(-2 * math.log(1 - math.random())) * math.cos(2 * math.pi * math.random()) end
    for h = 1, #out.w1 do
        for i = 1, #out.w1[h] do out.w1[h][i] = out.w1[h][i] + n() end
        out.b1[h] = out.b1[h] + n()
        out.v[h] = out.v[h] + n()
    end
    for o = 1, #out.w2 do
        for h = 1, #out.w2[o] do out.w2[o][h] = out.w2[o][h] + n() end
        out.b2[o] = out.b2[o] + n()
    end
    for _, layer in ipairs(out.layers) do
        for h, row in ipairs(layer.w) do
            for i = 1, #row do row[i] = row[i] + n() end
            layer.b[h] = layer.b[h] + n()
        end
    end
    return out
end

-- Keep the two best evaluated policies; alternate their mutated offspring among the rest.
local function evolve_population(nets, fitness)
    local ranking, names = {}, {}
    for name in pairs(nets) do
        names[#names + 1] = name
        local f = fitness[name]
        if f and f[2] > 0 then ranking[#ranking + 1] = { bot = name, fitness = f[1] / f[2] } end
    end
    table.sort(names)
    table.sort(ranking, function(a, b)
        if a.fitness ~= b.fitness then return a.fitness > b.fitness end
        return a.bot < b.bot
    end)
    local parents, elite = {}, {}
    for i = 1, math.min(2, #ranking) do
        parents[i] = ranking[i].bot
        elite[parents[i]] = copy_net(nets[parents[i]])
    end
    local child = 0
    for _, name in ipairs(names) do
        if #parents > 0 and not elite[name] then
            child = child + 1
            nets[name] = mutated(elite[parents[(child - 1) % #parents + 1]])
        end
    end
    return ranking, parents
end

local function forward(net, x)
    local hidden = {}
    for h = 1, #net.w1 do
        local s = net.b1[h]
        for i = 1, #net.w1[h] do s = s + net.w1[h][i] * (x[i] or 0) end
        hidden[h] = 2 / (1 + math.exp(-2 * s)) - 1
    end
    local activations = { hidden }
    for _, layer in ipairs(deepen(net).layers) do
        local next_hidden = {}
        for h, row in ipairs(layer.w) do
            local z = layer.b[h]
            for i, w in ipairs(row) do z = z + w * hidden[i] end
            -- Bounded residual branch; the identity path keeps old learning intact.
            next_hidden[h] = hidden[h] + (2 / (1 + math.exp(-2 * z)) - 1)
        end
        hidden = next_hidden
        activations[#activations + 1] = hidden
    end
    local logits, top = {}, -math.huge
    for o = 1, #net.w2 do
        local s = net.b2[o]
        for h = 1, #hidden do s = s + net.w2[o][h] * hidden[h] end
        logits[o] = s
        if s > top then top = s end
    end
    local p, sum = {}, 0
    for o = 1, #logits do p[o] = math.exp(logits[o] - top); sum = sum + p[o] end
    for o = 1, #logits do p[o] = p[o] / sum end
    local value = net.vb or 0
    for h = 1, #hidden do value = value + (net.v and net.v[h] or 0) * hidden[h] end
    return p, hidden, value, activations
end

-- One step of actor-critic: the critic says what such a day usually brings; the chosen action's
-- log-probability goes up by how much better the day was (the advantage), and the critic moves toward the
-- reward. (REINFORCE with a learnt, situation-dependent baseline)
local function learn(net, x, action, reward)
    local p, hidden, value, activations = forward(net, x)
    local advantage = reward - value
    local clip = math.max(-2, math.min(2, advantage))
    for h = 1, #hidden do net.v[h] = net.v[h] + VALUE_RATE * clip * hidden[h] end
    net.vb = net.vb + VALUE_RATE * clip
    -- The policy gradient, plus the gradient of the entropy (how spread the wants are)
    local entropy = 0
    for o = 1, #p do entropy = entropy - p[o] * math.log(p[o] + 1e-12) end
    local dlogit = {}
    for o = 1, #p do
        dlogit[o] = clip * (((o == action) and 1 or 0) - p[o]) - ENTROPY * p[o] * (math.log(p[o] + 1e-12) + entropy)
    end
    local dhidden = {}
    for h = 1, #hidden do
        local z = 0
        for o = 1, #p do z = z + net.w2[o][h] * dlogit[o] end
        dhidden[h] = z
    end
    for l = #net.layers, 1, -1 do
        local layer, input, output = net.layers[l], activations[l], activations[l + 1]
        local previous, delta = { table.unpack(dhidden) }, {}
        for h, row in ipairs(layer.w) do
            local branch = output[h] - input[h]
            delta[h] = dhidden[h] * (1 - branch * branch)
            for i, w in ipairs(row) do previous[i] = previous[i] + w * delta[h] end
        end
        for h, row in ipairs(layer.w) do
            for i = 1, #row do row[i] = row[i] + LEARNING_RATE * delta[h] * input[i] end
            layer.b[h] = layer.b[h] + LEARNING_RATE * delta[h]
        end
        dhidden = previous
    end
    local first = activations[1]
    for h = 1, #first do dhidden[h] = dhidden[h] * (1 - first[h] * first[h]) end
    for o = 1, #p do
        for h = 1, #hidden do net.w2[o][h] = net.w2[o][h] + LEARNING_RATE * dlogit[o] * hidden[h] end
        net.b2[o] = net.b2[o] + LEARNING_RATE * dlogit[o]
    end
    for h = 1, #hidden do
        for i = 1, #net.w1[h] do net.w1[h][i] = net.w1[h][i] + LEARNING_RATE * dhidden[h] * (x[i] or 0) end
        net.b1[h] = net.b1[h] + LEARNING_RATE * dhidden[h]
    end
end

-- A network saved with fewer inputs or outputs (5 trades before 8): what it learnt is kept, the new
-- inputs start at zero weight and the new outputs small
local function grown(net, n_in, n_out)
    local fresh = new_network(n_in, #net.w1, n_out, 777)
    for h = 1, #net.w1 do
        for i = 1, n_in do fresh.w1[h][i] = net.w1[h][i] or 0 end
        fresh.b1[h] = net.b1[h]
        fresh.v[h] = net.v and net.v[h] or 0
    end
    fresh.vb = net.vb or 0
    for o = 1, math.min(n_out, #net.w2) do fresh.w2[o], fresh.b2[o] = { table.unpack(net.w2[o]) }, net.b2[o] end
    fresh.layers = copy_net(net).layers
    return fresh
end

NN = { evolve = evolve_population, new = new_network, copy = copy_net, mutated = mutated, forward = forward, learn = learn, serialize = serialize, grow = grown, deepen = deepen }

local EDIBLE = {}
for item in ("bread pumpkin_pie apple golden_apple carrot golden_carrot potato baked_potato beetroot beetroot_soup mushroom_stew rabbit_stew sweet_berries glow_berries melon_slice dried_kelp cooked_beef cooked_porkchop cooked_mutton cooked_chicken cooked_rabbit cooked_cod cooked_salmon beef porkchop mutton chicken rabbit cod salmon cookie honey_bottle"):gmatch("%S+") do EDIBLE[item] = true end
local function condition(st)
    local foods, best = 0, {}
    local function gear(item)
        local material, kind = item:match("^(%a+)_(%a+)$")
        local tier = ({ wooden = 1, leather = 1, golden = 1, stone = 2, chainmail = 2, iron = 3, diamond = 4, netherite = 5 })[material]
        if tier and ({ pickaxe = true, axe = true, sword = true, shovel = true, hoe = true, helmet = true, chestplate = true, leggings = true, boots = true })[kind] then
            best[kind] = math.max(best[kind] or 0, tier)
        end
    end
    for item, count in pairs(type(st.inventory) == "table" and st.inventory or {}) do
        if count > 0 then if EDIBLE[item] then foods = foods + 1 end; gear(item) end
    end
    for _, item in pairs(type(st.equipment) == "table" and st.equipment or {}) do if type(item) == "string" then gear(item) end end
    local quality = 0
    for _, tier in pairs(best) do quality = quality + tier end
    -- The four essentials, equally important: food, a tool to mine or dig, a weapon, armor
    local essentials = (foods > 0 and 1 or 0) + ((best.pickaxe or best.shovel) and 1 or 0) + ((best.sword or best.axe) and 1 or 0)
        + ((best.helmet or best.chestplate or best.leggings or best.boots) and 1 or 0)
    return { variety = foods, quality = quality, health = st.health or 20, food = st.food or 20,
        has_food = foods > 0 and 1 or 0, missing = 4 - essentials, at = os.time(), deaths = st.deaths or 0 }
end
local function living_reward(before, now)
    if not before then return 0 end
    local survived = now.deaths == before.deaths
    local duration = math.max(0, math.min(300, now.at - before.at)) / 300
    -- Getting or running out of food weighs like a stone tool (3 per tier, stone is 2); every essential
    -- lacking costs 0.5 per 5 minutes, as long as it lacks
    return (survived and duration * math.min(now.health, now.food) / 20 or 0)
        + 2 * (now.variety - before.variety) + 3 * (now.quality - before.quality)
        + 6 * (now.has_food - (before.has_food or now.has_food)) - 0.5 * duration * now.missing
        -- Low health and hunger cost as long as they last: each up to 2 per 5 minutes near empty
        - 2 * duration * (1 - math.max(0, math.min(20, now.health)) / 20)
        - 2 * duration * (1 - math.max(0, math.min(20, now.food)) / 20)
        + (now.health - before.health) / 4 + (now.food - before.food) / 4
end
-- Health lost, per point (the bot's counters): any damage, and more for drowning, falls and fire or lava;
-- dying of one of these costs 40 more than the 40 any death costs
local HAZARDS = { damage_taken = -0.5, drown_damage = -1, fall_damage = -1, burn_damage = -1,
    drown_deaths = -40, fall_deaths = -40, burn_deaths = -40 }
NN.condition, NN.living_reward, NN.edible, NN.hazards = condition, living_reward, EDIBLE, HAZARDS

-- ---------------------------------------------------------------- what the bots see
local function inventory_count(st, test)
    local n = 0
    for item, count in pairs(type(st.inventory) == "table" and st.inventory or {}) do
        if test(item) then n = n + (tonumber(count) or 0) end
    end
    return n
end

local function armor_worn(st)
    local eq = type(st.equipment) == "table" and st.equipment or {}
    local n = 0
    for _, slot in ipairs({ "head", "chest", "legs", "feet" }) do
        if type(eq[slot]) == "string" and eq[slot] ~= "" then n = n + 1 end
    end
    return n / 4
end

local function stored_resources(st)
    local n = 0
    for _, c in pairs(type(st.stored) == "table" and st.stored or {}) do n = n + (tonumber(c) or 0) end
    return math.min(1, n / 256)
end

local function stations_known(st)
    local m = type(st.memory) == "table" and st.memory or {}
    local n = (tonumber(m.chest) or 0) + (tonumber(m.barrel) or 0) + (tonumber(m.furnace) or 0)
        + (tonumber(m.blast_furnace) or 0) + (tonumber(m.smoker) or 0) + (tonumber(m.crafting_table) or 0)
    return math.min(1, n / 10)
end

local function world_resources(st)
    local m = type(st.memory) == "table" and st.memory or {}
    local n = 0
    for k, v in pairs(m) do
        if k ~= "chest" and k ~= "barrel" and k ~= "furnace" and k ~= "blast_furnace" and k ~= "smoker" and k ~= "crafting_table" then
            n = n + (tonumber(v) or 0)
        end
    end
    return math.min(1, n / 50)
end

local function has_objective(st)
    return (st.building or (tonumber(st.gather_left) or 0) > 0 or (tonumber(st.making_until) or 0) > 0
        or (type(st.team_order) == "string" and st.team_order ~= "")) and 1 or 0
end

NN.armor_worn, NN.stored_resources, NN.stations_known, NN.world_resources, NN.has_objective =
    armor_worn, stored_resources, stations_known, world_resources, has_objective

local FOOD = { bread = true, cooked_beef = true, cooked_porkchop = true, cooked_mutton = true, cooked_chicken = true, baked_potato = true,
    carrot = true, apple = true, beef = true, porkchop = true, mutton = true, golden_carrot = true, sweet_berries = true, potato = true }

-- Append-only order from the bundled Java 26.3 biome data; unknown covers unloaded/custom biomes.
local BIOMES = {
    "badlands", "bamboo_jungle", "basalt_deltas", "beach", "birch_forest",
    "cherry_grove", "cold_ocean", "crimson_forest", "dappled_forest", "dark_forest",
    "deep_cold_ocean", "deep_dark", "deep_frozen_ocean", "deep_lukewarm_ocean", "deep_ocean",
    "desert", "dripstone_caves", "end_barrens", "end_highlands", "end_midlands",
    "eroded_badlands", "flower_forest", "forest", "frozen_ocean", "frozen_peaks",
    "frozen_river", "grove", "ice_spikes", "jagged_peaks", "jungle",
    "lukewarm_ocean", "lush_caves", "mangrove_swamp", "meadow", "mushroom_fields",
    "nether_wastes", "ocean", "old_growth_birch_forest", "old_growth_pine_taiga", "old_growth_spruce_taiga",
    "pale_garden", "plains", "river", "savanna", "savanna_plateau",
    "small_end_islands", "snowy_beach", "snowy_plains", "snowy_slopes", "snowy_taiga",
    "soul_sand_valley", "sparse_jungle", "stony_peaks", "stony_shore", "sulfur_caves",
    "sunflower_plains", "swamp", "taiga", "the_end", "the_void",
    "warm_ocean", "warped_forest", "windswept_forest", "windswept_gravelly_hills", "windswept_hills",
    "windswept_savanna", "wooded_badlands",
}
NN.environment_inputs = { "time of day sine", "time of day cosine", "rain strength", "thunder strength", "biome unknown" }
for _, biome in ipairs(BIOMES) do NN.environment_inputs[#NN.environment_inputs + 1] = "biome " .. biome end
function NN.environment(st)
    local phase = ((tonumber(st.day_time) or 0) % 24000) * 2 * math.pi / 24000
    local function level(v) return math.max(0, math.min(1, tonumber(v) or 0)) end
    local x = { math.sin(phase), math.cos(phase), level(st.rain_level), level(st.thunder_level), 1 }
    local biome = tostring(st.biome or ""):gsub("^minecraft:", "")
    for _, name in ipairs(BIOMES) do
        local match = biome == name
        x[#x + 1] = match and 1 or 0
        if match then x[5] = 0 end
    end
    return x
end

local INPUT_NAMES = {
    "bias", "health", "food level", "air", "pos x", "pos y", "pos z", "night",
    "needs unmet", "team hungry", "castle progress",
    "building", "build remaining", "build idle", "has objective",
    "armor worn", "gear quality", "has pickaxe", "has axe", "has sword", "has hoe",
    "food carried", "food variety", "wood carried", "stone carried", "iron carried", "coal carried",
    "hostiles near", "animals near", "players near",
    "chest resources", "furnaces known", "world resources",
    "was baker", "was farmer", "was butcher", "was miner", "was builder", "was soldier",
    "was blacksmith", "was explorer", "was fisher", "was lumberjack", "was carpenter",
}

for _, name in ipairs(NN.environment_inputs) do INPUT_NAMES[#INPUT_NAMES + 1] = name end

local function features(st, trade, team)
    local function has(suffix) return inventory_count(st, function(i) return i:sub(-#suffix) == suffix end) > 0 and 1 or 0 end
    local function near(kind, max_n)
        local list = st.name and call("entities", { bot = st.name, type = kind }) or {}
        local k = 0
        for _, e in ipairs(type(list) == "table" and list or {}) do if (e.distance or 99) <= 16 then k = k + 1 end end
        if kind == "player" and k == 0 and type(st.players_online) == "table" then
            k = math.max(0, #st.players_online - 1)
        end
        return math.min(1, k / max_n)
    end
    local pos = type(st.position) == "table" and st.position or {}
    local cond = condition(st)
    local time = tonumber(st.day_time) or 0
    local food_carry = math.min(1, inventory_count(st, function(i) return FOOD[i] or EDIBLE[i] end) / 16)
    local wood_carry = math.min(1, inventory_count(st, function(i) return i:match("_log$") or i:match("_planks$") end) / 64)
    local stone_carry = math.min(1, inventory_count(st, function(i) return i == "cobblestone" or i == "stone" end) / 64)
    local iron_carry = math.min(1, inventory_count(st, function(i) return i == "iron_ingot" or i == "raw_iron" end) / 16)
    local coal_carry = math.min(1, inventory_count(st, function(i) return i == "coal" or i == "charcoal" end) / 16)
    local unmet = math.max(0, 1 - (food_carry + wood_carry + stone_carry + has("_pickaxe") + has("_axe")) / 5)
    local x = {
        1,
        (st.health or 20) / 20,
        (st.food or 20) / 20,
        math.max(0, math.min(1, (tonumber(st.air) or 300) / 300)),
        math.tanh((tonumber(pos.x) or 0) / 500),
        math.max(0, math.min(1, ((tonumber(pos.y) or 64) + 64) / 384)),
        math.tanh((tonumber(pos.z) or 0) / 500),
        (time % 24000) >= 13000 and 1 or 0,
        unmet,
        team.hungry,
        team.castle,
        st.building and 1 or 0,
        math.min(1, (tonumber(st.build_left) or 0) / 1000),
        math.min(1, (tonumber(st.build_idle) or 0) / 600),
        has_objective(st),
        armor_worn(st),
        cond.quality / 45,
        has("_pickaxe"), has("_axe"), has("_sword"), has("_hoe"),
        food_carry,
        math.min(1, cond.variety / 8),
        wood_carry,
        stone_carry,
        iron_carry,
        coal_carry,
        near("hostile", 4),
        near("animal", 8),
        near("player", 4),
        stored_resources(st),
        stations_known(st),
        world_resources(st),
    }
    for t = 1, OUT do x[#x + 1] = (t == trade) and 1 or 0 end
    for _, value in ipairs(NN.environment(st)) do x[#x + 1] = value end
    return x
end

-- What a bot does for itself, and what it does for the others (cooperation: building the team's project,
-- handing over food and items, coming when called to a fight and killing there, picking up a fallen
-- teammate's things). Each its own part of the reward
local WEIGHTS = { idle_seconds = -0.1, construction_penalty = -1, collected = 1, harvested = 1, killed = 3, bred = 5, stored = 0.05 }
for what, w in pairs(HAZARDS) do WEIGHTS[what] = w end
local COOP_WEIGHTS = { built = 1, given = 1, answered = 3, assisted = 5, salvaged = 2 }

local function counted(now, before, weights)
    local score = 0
    local stats, old = now.stats or {}, (before and before.stats) or {}
    for what, w in pairs(weights) do
        local a, b = tonumber(stats[what]) or 0, tonumber(old[what]) or 0
        score = score + w * (a >= b and a - b or a)
    end
    return score
end

local function coop_score(now, before) return counted(now, before, COOP_WEIGHTS) end

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
    return score + living_reward(before and before.living, condition(now)) - 40 * delta("deaths") - 2 * delta("times_stuck")
end

-- ---------------------------------------------------------------- the state
local state = restore() or {
    enabled = true,
    day = 0,                 -- Minecraft days seen
    last_time = nil,         -- the time of day at the last look, to see a new day start
    last_day_at = os.time(),
    net = new_network(),     -- the champion: the best network of the last generation (shared with netlink)
    nets = {},               -- bot -> its own network
    fitness = {},            -- bot -> { sum of its rewards, days } in this generation
    generation = 0,
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
    return { stats = stats, deaths = st.deaths or 0, times_stuck = st.times_stuck or 0, living = condition(st) }
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

-- Each bot's own network: a mutated copy of the champion the first time
local function net_of(name)
    state.nets = state.nets or {}
    if not state.nets[name] then state.nets[name] = mutated(state.net) end
    return state.nets[name]
end

-- The best two bots seed the next generation, preserving both parents unchanged.
local function evolve()
    local ranking, parents = evolve_population(state.nets, state.fitness or {})
    state.fitness = {}
    if #parents == 0 then return end
    state.net = copy_net(state.nets[parents[1]])
    state.generation = (state.generation or 0) + 1
    state.last_generation = { generation = state.generation, day = state.day, winner = parents[1], winners = parents,
        fitness = ranking[1].fitness, ranking = ranking }
end

-- Which bot does what today: each draws from its wants, the most decided first, two to a trade (15 bots, 8 trades)
-- (the leader is a builder, the berry picker a farmer)
local function assign(list, team)
    local capacity = math.max(1, math.ceil(#list / OUT))
    local picker = call("berries", {})
    picker = type(picker) == "table" and picker.bot or nil
    local wants = {}
    for _, st in ipairs(list) do
        local x = features(st, state.trades[st.name] or 5, team)
        local p = forward(net_of(st.name), x)
        -- Drawn at random by the wants: what is not tried is never learnt
        local r, pick = math.random(), OUT
        for o = 1, OUT do
            r = r - p[o]
            if r <= 0 then pick = o; break end
        end
        wants[#wants + 1] = { name = st.name, x = x, p = p, pick = pick, sure = p[pick] }
    end
    table.sort(wants, function(a, b) return a.sure > b.sure end)
    local groups, taken = {}, {}
    for t = 1, OUT do groups[t] = {} end
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
        if state.trades[w.name] then place(w, state.trades[w.name])
        elseif w.name == state.leader then place(w, 5)
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
    local own_total, coop_total, led, team_all = {}, {}, {}, {}
    for _, d in ipairs(days) do
        team_all[#team_all + 1] = d.team
        for name, s in pairs(d.own or {}) do own_total[name] = (own_total[name] or 0) + s end
        for name, s in pairs(d.coop or {}) do coop_total[name] = (coop_total[name] or 0) + s end
        if d.leader then
            led[d.leader] = led[d.leader] or {}
            led[d.leader][#led[d.leader] + 1] = d.team
        end
    end
    local function mean(t) local s = 0 for _, v in ipairs(t) do s = s + v end return #t > 0 and s / #t or 0 end
    local function spread(t) local m, s = mean(t), 0 for _, v in ipairs(t) do s = s + (v - m) ^ 2 end return math.sqrt(s / math.max(1, #t)) + 1e-6 end
    local names = {}
    for _, st in ipairs(list) do names[#names + 1] = st.name end
    local owns, coops, leads = {}, {}, {}
    for _, n in ipairs(names) do
        owns[#owns + 1] = own_total[n] or 0
        coops[#coops + 1] = coop_total[n] or 0
        leads[#leads + 1] = led[n] and mean(led[n]) or mean(team_all)
    end
    local m_own, s_own, m_lead, s_lead = mean(owns), spread(owns), mean(leads), spread(leads)
    local m_coop, s_coop = mean(coops), spread(coops)
    -- Did the team do better under the current leader than before it? A bonus to keep it
    local half = math.floor(#days / 2)
    local first, second = {}, {}
    for i, d in ipairs(days) do if i <= half then first[#first + 1] = d.team else second[#second + 1] = d.team end end
    local improving = #first > 0 and mean(second) >= mean(first)
    local tally = {}
    for _, voter in ipairs(names) do
        local best, best_score = nil, -math.huge
        for i, c in ipairs(names) do
            -- Its own work, how much it helped the others, and how the team did when it led
            local score = 0.35 * (owns[i] - m_own) / s_own + 0.35 * (coops[i] - m_coop) / s_coop + 0.3 * (leads[i] - m_lead) / s_lead
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
    local own, coop, team_sum, coop_sum, fed = {}, {}, 0, 0, 0
    for _, st in ipairs(list) do
        own[st.name] = own_score(st, state.before[st.name])
        coop[st.name] = coop_score(st, state.before[st.name])
        team_sum = team_sum + own[st.name]
        coop_sum = coop_sum + coop[st.name]
        fed = fed + (st.food or 0) / 20
    end
    local team = team_sum + coop_sum + 50 * castle_delta + 20 * fed / #list
    state.history[#state.history + 1] = { leader = state.leader, team = team, own = own, coop = coop }
    while #state.history > VOTE_EVERY do table.remove(state.history, 1) end
    -- Learning: the trades of today, judged by half the bot's own score and half the team's
    state.own_scale = 0.9 * state.own_scale + 0.1 * math.max(1, team_sum / #list)
    state.coop_scale = 0.9 * (state.coop_scale or 5) + 0.1 * math.max(1, coop_sum / #list)
    state.team_scale = 0.9 * state.team_scale + 0.1 * math.max(1, math.abs(team))
    local rewards, sum, n = {}, 0, 0
    for _, st in ipairs(list) do
        if state.inputs[st.name] and state.trades[st.name] then
            -- 40% its own work, 30% what it did for the others, 30% how the whole team did
            rewards[st.name] = 0.4 * own[st.name] / state.own_scale + 0.3 * coop[st.name] / state.coop_scale + 0.3 * team / state.team_scale
            sum, n = sum + rewards[st.name], n + 1
        end
    end
    -- Each bot's choice is judged against the day's average (the team half is the same for everyone: it
    -- moves the baseline, kept for the record, and how the whole day compares with the usual)
    local mean = n > 0 and sum / n or 0
    -- The network as it was this morning: kept (the last 3 days) to roll back to
    state.checkpoints = state.checkpoints or {}
    table.insert(state.checkpoints, 1, { day = state.day, net = copy_net(state.net) })
    while #state.checkpoints > 3 do table.remove(state.checkpoints) end
    for name, r in pairs(rewards) do
        if (state.drawn or {})[name] ~= false then
            learn(net_of(name), state.inputs[name], state.trades[name], r)
            state.samples = (state.samples or 0) + 1
        end
        state.fitness = state.fitness or {}
        local f = state.fitness[name] or { 0, 0 }
        state.fitness[name] = { f[1] + r, f[2] + 1 }
    end
    state.baseline = 0.9 * state.baseline + 0.1 * mean
    state.last_rewards = rewards
    state.last_team = team
    state.last_coop = coop
    -- The new day: figures to compare with tomorrow, then the trades
    state.day = state.day + 1
    if state.day % GENERATION_DAYS == 0 then evolve() end
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
-- A network saved smaller (before the critic, before the 8 trades) grows to the current shape
local function fit(net) return (#net.w2 == OUT and #net.w1[1] == IN and net.v) and deepen(net) or grown(net, IN, OUT) end
state.net = fit(state.net)
for name, net in pairs(state.nets or {}) do state.nets[name] = fit(net) end
for _, cp in ipairs(state.checkpoints or {}) do cp.net = fit(cp.net) end
state.merge_trial = nil
bot.every(30, function()
    if CENTRAL_ACTIVE then return end
    if MIGRATING then return end      -- the bots are moving to a new place (migrate plugin)
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

-- ---------------------------------------------------------------- sharing (see the netlink plugin)
-- The network as a flat list of numbers, in a fixed order, and back
local function flatten(net)
    local w = {}
    for h = 1, HID do for i = 1, IN do w[#w + 1] = net.w1[h][i] end; w[#w + 1] = net.b1[h] end
    for o = 1, OUT do for h = 1, HID do w[#w + 1] = net.w2[o][h] end; w[#w + 1] = net.b2[o] end
    for h = 1, HID do w[#w + 1] = net.v[h] end
    w[#w + 1] = net.vb
    for _, layer in ipairs(net.layers) do
        for h, row in ipairs(layer.w) do
            for _, value in ipairs(row) do w[#w + 1] = value end
            w[#w + 1] = layer.b[h]
        end
    end
    return w
end

-- What another program's bots learnt: a step toward their network, bigger the more lessons it comes from
-- (a peer with as many as ours moves us a quarter of the way; it moves toward us the same way)
local function merge(w, samples, origin, arch)
    local mine = state.samples or 0
    if arch ~= nil and arch ~= ARCH then return nil, "other_architecture" end
    if type(w) ~= "table" or #w ~= #flatten(state.net) then return nil, "other_shape" end
    if (samples or 0) <= 0 then return nil, "no_lessons" end
    -- Every number a real one: a NaN or an infinity would spread through the whole network
    for _, v in ipairs(w) do
        if type(v) ~= "number" or v ~= v or v == math.huge or v == -math.huge or math.abs(v) > 1e6 then return nil, "not_finite" end
    end
    -- The same lessons twice from the same program are merged once
    state.merged_from = state.merged_from or {}
    if origin and (state.merged_from[origin] or -1) >= samples then return nil, "already_merged" end
    if origin then state.merged_from[origin] = samples end
    -- The champion moved toward the peer's network becomes a challenger: the network of the bot doing worst in
    -- this generation. If it does well it wins a generation and spreads; if not, it is replaced like any other
    local mix = 0.5 * samples / (mine + samples)
    local net, k = copy_net(state.net), 0
    local function step(old) k = k + 1; return old + mix * ((tonumber(w[k]) or old) - old) end
    for h = 1, HID do for i = 1, IN do net.w1[h][i] = step(net.w1[h][i]) end; net.b1[h] = step(net.b1[h]) end
    for o = 1, OUT do for h = 1, HID do net.w2[o][h] = step(net.w2[o][h]) end; net.b2[o] = step(net.b2[o]) end
    for h = 1, HID do net.v[h] = step(net.v[h]) end
    net.vb = step(net.vb)
    for _, layer in ipairs(net.layers) do
        for h, row in ipairs(layer.w) do
            for i = 1, #row do row[i] = step(row[i]) end
            layer.b[h] = step(layer.b[h])
        end
    end
    local worst, worst_fit = nil, math.huge
    for name in pairs(state.nets or {}) do
        local f = (state.fitness or {})[name]
        local avg = f and f[2] > 0 and f[1] / f[2] or 0
        if avg < worst_fit then worst, worst_fit = name, avg end
    end
    if worst then
        state.nets[worst] = net
        state.fitness[worst] = nil
        state.challenger = { bot = worst, origin = origin, day = state.day }
    else
        state.net = net
    end
    state.merged = (state.merged or 0) + 1
    save(state)
    return mix
end

bot.tool{
    name = "brain",
    description = "The bots' brain: a small neural network (116 inputs about the bot, environment and team, five hidden layers of 10 units, 11 outputs) of its own (every 5 days the two best-scoring bots seed slightly mutated copies for the others) assigns each bot a lasting trade (baker, farmer, butcher, miner, builder, soldier, blacksmith, explorer, fisher; groups of two; each bot's own policy for its trade then picks its actions, see the roles tool), and learns by reinforcement from what each bot and the whole team achieved that day (the stats in their status, the castle's progress, how fed they are). Every 10 days the bots vote for a leader from each one's results and how the team did under each leader. With no argument: the day, the leader, the last vote, and each bot's wants, trade and last reward. vote=true votes now; stop/start.",
    params = { vote = "boolean", stop = "boolean", start = "boolean", export = "boolean", merge = "object", rollback = "number", network = "string" },
    run = function(args)
        -- For the netlink plugin: the network out, a peer's network in
        if args.export then return { samples = state.samples or 0, w = flatten(state.net), arch = ARCH } end
        if type(args.merge) == "table" then
            local mix, why = merge(args.merge.w, args.merge.samples, args.merge.origin, args.merge.arch)
            if not mix then error("rejected:" .. tostring(why)) end
            return { mixed = mix, merged_so_far = state.merged }
        end
        -- Back to the network as it was n mornings ago (1 to 3)
        if args.rollback then
            local cp = (state.checkpoints or {})[math.floor(args.rollback)]
            if not cp then error("no checkpoint that far back (" .. #(state.checkpoints or {}) .. " kept)") end
            state.net = copy_net(cp.net)
            for name in pairs(state.nets or {}) do state.nets[name] = copy_net(cp.net) end
            state.fitness = {}
            save(state)
            return "back to the network of day " .. cp.day
        end
        if args.stop then state.enabled = false; save(state); return "the brain rests: the groups stay as they are" end
        if args.start then state.enabled = true; save(state); return "the brain is working" end
        if args.vote then
            local list = statuses()
            if #list == 0 then error("no bot") end
            local r = vote(list)
            save(state)
            return r
        end
        -- One bot's network as it is now, and what it makes of the bot right now (for the panels)
        if type(args.network) == "string" and args.network ~= "" then
            local st = call("status", { bot = args.network })
            if type(st) ~= "table" or not st.name then error("no bot " .. args.network) end
            local net = net_of(st.name)
            local list, hungry = statuses(), 0
            for _, o in ipairs(list) do if (o.food or 20) <= 8 then hungry = hungry + 1 end end
            local x = features(st, state.trades[st.name] or 5, { hungry = hungry / math.max(1, #list), castle = 0 })
            local p, hidden, value, activations = forward(net, x)
            local inputs, outputs = {}, {}
            for i = 1, IN do inputs[i] = { name = INPUT_NAMES[i], value = x[i] } end
            for o = 1, OUT do outputs[o] = { name = TRADES[o], percent = math.floor(p[o] * 1000 + 0.5) / 10, bias = net.b2[o] } end
            local f = (state.fitness or {})[st.name]
            return { bot = st.name, trade = TRADES[state.trades[st.name] or 5], generation = state.generation or 0,
                fitness = f and f[2] > 0 and f[1] / f[2] or nil, days_in_generation = f and f[2] or 0,
                champion = state.last_generation and state.last_generation.winner or nil,
                inputs = inputs, hidden = hidden, hidden_bias = net.b1, outputs = outputs, value = value,
                w1 = net.w1, w2 = net.w2, v = net.v, vb = net.vb, layers = net.layers, activations = activations,
                -- Its trade's own network (the roles plugin), with what it makes of the bot now
                role_network = call("roles", { network = TRADES[state.trades[st.name] or 5], bot = st.name }) }
        end
        local bots = {}
        local hungry = 0
        local list = statuses()
        for _, st in ipairs(list) do if (st.food or 20) <= 8 then hungry = hungry + 1 end end
        for _, st in ipairs(list) do
            local p = forward(net_of(st.name), features(st, state.trades[st.name] or 5, { hungry = hungry / math.max(1, #list), castle = 0 }))
            local wants = {}
            for o = 1, OUT do wants[TRADES[o]] = math.floor(p[o] * 100 + 0.5) end
            bots[st.name] = { trade = TRADES[state.trades[st.name] or 5], wants_percent = wants,
                last_reward = state.last_rewards and state.last_rewards[st.name],
                last_cooperation = state.last_coop and state.last_coop[st.name],
                fitness = (function(f) return f and f[2] > 0 and f[1] / f[2] or nil end)((state.fitness or {})[st.name]) }
        end
        return { enabled = state.enabled, day = state.day, lessons = state.samples or 0, networks_merged = state.merged or 0,
            generation = state.generation or 0, next_generation_in_days = GENERATION_DAYS - state.day % GENERATION_DAYS,
            last_generation = state.last_generation, challenger = state.challenger, next_vote_in_days = VOTE_EVERY - state.day % VOTE_EVERY,
            leader = state.leader, last_vote = state.last_vote, team_score_last_day = state.last_team, bots = bots }
    end,
}
