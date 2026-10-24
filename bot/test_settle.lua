-- Offline check of plugins/settle.lua's vote (lua5.4 test_settle.lua): stand-in bots, what each sees, the outcome
local tools, world = {}, {}
-- world.bots: name -> { trade, x, y, z, biome, sees = { [block] = n, animal = n, hostile = n } }
local function n_list(n) local l = {} for i = 1, n do l[i] = {} end return l end
bot = {
    tool = function(t) tools[t.name] = t end,
    call = function(tool, a)
        if tool == "bots" then
            local l = {}
            for name in pairs(world.bots) do l[#l + 1] = { name = name } end
            table.sort(l, function(x, y) return x.name < y.name end)
            return l
        end
        local b = a.bot and world.bots[a.bot]
        if tool == "status" then
            return { position = { x = b.x, y = b.y, z = b.z }, trade = b.trade, biome = b.biome, dimension = "minecraft:overworld" }
        elseif tool == "find_blocks" then return n_list(b.sees[a.block] or 0)
        elseif tool == "entities" then return n_list(b.sees[a.type] or 0)
        elseif tool == "chat" or tool == "empire" then return "ok" end
        error("unexpected call " .. tool)
    end,
}
dofile((arg[0]:match("^(.*)/") or ".") .. "/plugins/settle.lua")
local settle = tools.settle

local ORE_HILL = { _ore = 40, iron_ore = 10, _log = 30, water = 20, animal = 4 }
local FOREST = { _log = 40, water = 20, animal = 6, sweet_berry_bush = 5 }

-- Two places 300 blocks apart. Three miners stand on the ore hill, two lumberjacks and a farmer in the forest
world.bots = {
    M1 = { trade = "miners", x = 0, y = 70, z = 0, biome = "meadow", sees = ORE_HILL },
    M2 = { trade = "miners", x = 5, y = 70, z = 5, biome = "meadow", sees = ORE_HILL },
    M3 = { trade = "miners", x = 10, y = 70, z = 0, biome = "meadow", sees = ORE_HILL },
    L1 = { trade = "lumberjacks", x = 300, y = 64, z = 0, biome = "forest", sees = FOREST },
    L2 = { trade = "lumberjacks", x = 305, y = 64, z = 0, biome = "forest", sees = FOREST },
    F1 = { trade = "farmers", x = 300, y = 64, z = 5, biome = "forest", sees = FOREST },
}
local r = settle.run({ vote = true })
assert(r.places == 2, "the two groups' spots each merge into one place: " .. r.places)
assert(r.voters == 6)
-- 3 against 3 is no majority: the second round decides
assert(r.rounds[2], "a tie goes to a second round")
print(string.format("tie: round 1 %s=%d %s=%d, round 2 winner %d %d", r.rounds[1][1].at, r.rounds[1][1].votes, r.rounds[1][2].at,
    r.rounds[1][2].votes, r.winner.x, r.winner.z))

-- Fishers by the water outnumber a miner: the water side wins in one round, with a majority
world.bots = {
    A = { trade = "fishers", x = 0, y = 64, z = 0, biome = "river", sees = { water = 20, _log = 5 } },
    B = { trade = "fishers", x = 2, y = 64, z = 0, biome = "river", sees = { water = 20, _log = 5 } },
    C = { trade = "fishers", x = 4, y = 64, z = 0, biome = "river", sees = { water = 20, _log = 5 } },
    D = { trade = "fishers", x = 0, y = 64, z = 4, biome = "river", sees = { water = 20, _log = 5 } },
    E = { trade = "miners", x = 400, y = 80, z = 0, biome = "stony_peaks", sees = { _ore = 40, lava = 3 } },
    U = { trade = "miners", x = 100, y = 20, z = 0, biome = "dripstone_caves", sees = { _ore = 60 } },   -- in a cave: votes, puts nothing forward
}
r = settle.run({ vote = true })
assert(r.voters == 6 and r.places == 2, "the bot in the cave puts no place forward")
assert(r.winner.x == 0 and r.winner.z == 0, "the water place wins")
assert(not r.rounds[2], "a majority in the first round")
assert(r.rounds[1][1].votes >= 4)

-- Lava and monsters put voters off: two equal places but for those, everyone votes for the safe one
world.bots = {
    P = { trade = "builders", x = 0, y = 64, z = 0, biome = "plains", sees = { _log = 20, water = 10, lava = 2, hostile = 8 } },
    Q = { trade = "builders", x = 200, y = 64, z = 0, biome = "plains", sees = { _log = 20, water = 10 } },
}
r = settle.run({ vote = true })
assert(r.winner.x == 200, "the safe place wins")
print("settle: ok")
