-- Writes docs/buildings.md: the plan of every building the bots put up, drawn from the plugins' own block lists
-- (castle.lua, empire.lua, roles.lua), so the drawings are what the bots build.
--   cd bot && lua5.4 gen_plans.lua > ../docs/buildings.md
-- The plugins are loaded with a stand-in `bot` (no game); their local building functions are found through
-- the upvalues of the tools they register.
local here = arg[0]:match("^(.*)/") or "."
local tools, timers = {}, {}
bot = { call = function() return nil end, tool = function(t) tools[t.name] = t end, every = function(_, f) timers[#timers + 1] = f end,
    on_chat = function() end, log = function() end, spawn = function() return false end, busy = function() return false end }
NN = NN or {}
local function load_plugin(name)
    local f = assert(io.open(here .. "/plugins/" .. name .. ".lua"))
    local chunk = assert(load(f:read("a"), name, "t"))
    f:close()
    chunk()
end

-- Every function and value reachable through upvalues, by name (a name can be several functions)
local function crawl(roots)
    local found, seen = {}, {}
    local function walk(f)
        if seen[f] then return end
        seen[f] = true
        for i = 1, math.huge do
            local name, v = debug.getupvalue(f, i)
            if not name then break end
            found[name] = found[name] or {}
            local dup = false
            for _, x in ipairs(found[name]) do dup = dup or x == v end
            if not dup then table.insert(found[name], v) end
            if type(v) == "function" then walk(v) end
        end
    end
    for _, f in ipairs(roots) do walk(f) end
    return function(name, nparams)
        for _, v in ipairs(found[name] or {}) do
            if nparams == nil or (type(v) == "function" and debug.getinfo(v, "u").nparams == nparams) then return v end
        end
        error("not found: " .. name)
    end
end

-- ------------------------------------------------------------------ drawing
local SYMBOLS = {
    air = ".", stone = "#", cobblestone = "c", stone_bricks = "B", dirt = "d", farmland = "f", water = "~", glass_pane = "g",
    oak_planks = "p", spruce_planks = "P", oak_log = "L", spruce_log = "l", stripped_oak_log = "|", oak_door = "D", spruce_door = "D",
    oak_stairs = "^", spruce_stairs = "^", cobblestone_stairs = "^", oak_fence = "+", spruce_fence = "+", oak_fence_gate = "=",
    torch = "*", lantern = "*", chest = "C", barrel = "O", furnace = "F", crafting_table = "T", white_bed = "b", oak_sign = "s",
    campfire = "&", composter = "K", cobblestone_wall = "!", hay_block = "H", oak_pressure_plate = "_", carved_pumpkin = "U",
    smoker = "S", blast_furnace = "F", anvil = "A", smithing_table = "T", grindstone = "G", cauldron = "u", white_wool = "w",
    raw_iron_block = "I", coal_block = "k", white_carpet = "-", bookshelf = "%", stone_brick_stairs = "^", stone_slab = "_",
    coarse_dirt = ",", spruce_fence_gate = "=", spruce_sign = "s",
}
local function base(item) return (tostring(item):gsub("^minecraft:", ""):gsub("[%[|].*$", "")) end

-- The blocks as a map, the last entry for a spot winning (plans clear a space with air, then build in it)
local function index(blocks)
    local at, lo, hi = {}, { math.huge, math.huge, math.huge }, { -math.huge, -math.huge, -math.huge }
    for _, b in ipairs(blocks) do
        at[b[1] .. "," .. b[2] .. "," .. b[3]] = base(b[4])
        for i = 1, 3 do lo[i] = math.min(lo[i], b[i]); hi[i] = math.max(hi[i], b[i]) end
    end
    return at, lo, hi
end

local function legend_of(used)
    local names = {}
    for name in pairs(used) do names[#names + 1] = name end
    table.sort(names)
    local parts = {}
    for _, name in ipairs(names) do parts[#parts + 1] = "`" .. (SYMBOLS[name] or "?") .. "` " .. name:gsub("_", " ") end
    return table.concat(parts, ", ")
end

local FLOOR = 64     -- every plan is drawn with its floor (where one stands) at this height: labelled y 0
-- Top-down layers, bottom first, north up, west left; identical layers in a row drawn once
local function layers(blocks)
    local at, lo, hi = index(blocks)
    local out, used, previous, from = {}, {}, nil, nil
    local function flush(to)
        if previous then
            local a, b = from - FLOOR, to - FLOOR
            out[#out + 1] = (a == b and ("y " .. a) or ("y " .. a .. " to " .. b)) .. "\n" .. previous
        end
    end
    for y = lo[2], hi[2] do
        local rows = {}
        for z = lo[3], hi[3] do
            local row = {}
            for x = lo[1], hi[1] do
                local name = at[x .. "," .. y .. "," .. z]
                if name then used[name] = true end
                row[#row + 1] = name and (SYMBOLS[name] or "?") or " "
            end
            rows[#rows + 1] = (table.concat(row):gsub("%s+$", ""))
        end
        local text = table.concat(rows, "\n")
        if text ~= previous then flush(y - 1); previous, from = text, y end
    end
    flush(hi[2])
    -- Heights relative to the building's floor (y 0: where one stands inside)
    return out, used, lo, hi
end

-- A side view: the blocks at x, from z_from to z_to (default all), top row first
local function section(blocks, x, z_from, z_to)
    local at, lo, hi = index(blocks)
    lo[3], hi[3] = z_from or lo[3], z_to or hi[3]
    local rows, used = {}, {}
    for y = hi[2], lo[2], -1 do
        local row = {}
        for z = lo[3], hi[3] do
            local name = at[x .. "," .. y .. "," .. z]
            if name then used[name] = true end
            row[#row + 1] = name and (SYMBOLS[name] or "?") or " "
        end
        local text = (table.concat(row):gsub("%s+$", ""))
        if text ~= "" or #rows > 0 then rows[#rows + 1] = text end
    end
    return table.concat(rows, "\n"), used
end

local doc = {}
local function say(text) doc[#doc + 1] = text end
local function plan(title, text, blocks, opts)
    opts = opts or {}
    say("### " .. title .. "\n\n" .. text .. "\n")
    local out, used, lo, hi = layers(blocks)
    say(string.format("%d blocks, %d x %d x %d (west-east x north-south x height). Layers bottom first; north is up.\n",
        #blocks, hi[1] - lo[1] + 1, hi[3] - lo[3] + 1, hi[2] - lo[2] + 1))
    local shown = opts.max_layers and math.min(#out, opts.max_layers) or #out
    say("```\n" .. table.concat(out, "\n\n", 1, shown) .. (shown < #out and ("\n\n(" .. (#out - shown) .. " more layers)") or "") .. "\n```\n")
    say(legend_of(used) .. "\n")
end

-- ------------------------------------------------------------------ the castle
load_plugin("castle")
local roots = {}
for _, t in pairs(tools) do roots[#roots + 1] = t.run end
local C = crawl(roots)
local function list(f, ...) local l = {} f(l, ...) return l end

say([[# Building plans

Every building the bots put up, drawn from the plans in the plugins (generated by `bot/gen_plans.lua`:
`cd bot && lua5.4 gen_plans.lua > ../docs/buildings.md`). Each plan is a list of blocks; a builder takes the next
block it can place (lowest first), fetching or making what it lacks (see "How a plan is built" at the end).

Drawings are top-down layers, bottom first, north up and west on the left, one character a block. `.` is air the
plan clears, a blank is left as it is. y 0 is the floor, where one stands; y -1 the ground or foundation under it.
Identical layers in a row are drawn once ("y 1 to 3").

## Contents

- [The castle](#the-castle): site, walls, towers, keep, vault, houses, plots, pens, gatehouse, moat, well, bridge,
  fountain, roads
- [The empire town and the village](#the-empire-town-and-the-village): huts, homes, gardens, shops, pens, roads,
  village walls
- [Built by the program itself](#built-by-the-program-itself): crop farms with their water, shelter, mines, tunnels
- [How a plan is built](#how-a-plan-is-built)
]])

say("## The castle\n")
local R1, R2, R3 = C("R1"), C("R2"), C("R3")
local THICK = C("THICK")
local structures = C("structures", 5)
local stages = structures(0, 64, 0, "stone", "stone_bricks")
-- The whole site, 4 by 4 blocks a character: the most telling block of each square
local RANK = { "water", "oak_door", "spruce_door", "glass_pane", "spruce_stairs", "oak_fence", "spruce_fence", "chest",
    "stone", "cobblestone", "stone_bricks", "dirt", "spruce_planks", "spruce_log" }
local rank = {}
for i, name in ipairs(RANK) do rank[name] = i end
local all, lo, hi = {}, { math.huge, math.huge }, { -math.huge, -math.huge }
local order = {}
for _, s in ipairs(stages) do
    order[#order + 1] = s.name .. " (" .. #s.blocks .. ")"
    for _, b in ipairs(s.blocks) do
        all[#all + 1] = b
        lo[1], lo[2] = math.min(lo[1], b[1]), math.min(lo[2], b[3])
        hi[1], hi[2] = math.max(hi[1], b[1]), math.max(hi[2], b[3])
    end
end
local cell = {}
for _, b in ipairs(all) do
    local name = base(b[4])
    if name ~= "air" and b[2] >= 58 then
        local key = ((b[1] - lo[1]) // 4) .. "," .. ((b[3] - lo[2]) // 4)
        if not cell[key] or (rank[name] or 99) < (rank[cell[key]] or 99) then cell[key] = name end
    end
end
local rows, used = {}, {}
for zc = 0, (hi[2] - lo[2]) // 4 do
    local row = {}
    for xc = 0, (hi[1] - lo[1]) // 4 do
        local name = cell[xc .. "," .. zc]
        if name then used[name] = true end
        row[#row + 1] = name and (SYMBOLS[name] or "?") or " "
    end
    rows[#rows + 1] = (table.concat(row):gsub("%s+$", ""))
end
say(string.format([[### The whole site

`castle { action = "start", x =, y =, z = }` levels the ground, then builds in %d stages, one after the other, the
whole team on each (blocks in each stage):

%s

%d x %d blocks in all. Three square walls around the keep, at half-sizes %d, %d and %d from the centre, each %d blocks
thick; the village of houses between the first and second walls, the farm plots and pens between the second and
third; the moat outside the third. One character is 4 x 4 blocks (the most telling block in it):

```
%s
```
%s
]], #stages, "- " .. table.concat(order, "\n- "), hi[1] - lo[1] + 1, hi[2] - lo[2] + 1, R1, R2, R3, THICK,
    table.concat(rows, "\n"), legend_of(used)))

local wall = C("wall", 7)
local w = list(wall, 0, 0, R1, 64, 67, "stone")
local cut, used_cut = section(w, 0, -R1 - 2, -R1 + THICK + 1)
say("### The walls\n\nEach wall is a square ring " .. THICK .. " blocks thick: the three middle blocks stop at the walkway, the\n" ..
    "outer and inner blocks rise one more on every other block (crenels). A gate 3 wide and 3 high goes through on the\n" ..
    "south side, and buttresses stand on the outer face every 9 blocks. The first and second walls are 4 high to the\n" ..
    "walkway, the third 3. Cross-section through the north side of the first wall (south to the right):\n\n```\n" ..
    cut .. "\n```\n" .. legend_of(used_cut) .. "\n")
plan("Corner tower", "Round, hollow, on each corner of each wall, with a floor at the top, a cone roof narrowing a block a level and a\nspire.", list(C("tower", 6), 0, 0, 64, 70, "stone"))
plan("Keep", "In the centre: 16 high, floors at 6 and 16, glass-pane windows on the axes, a doorway to the south, a gap in the\nmiddle floor to climb, then a stepped roof to a point with a spire.", list(C("keep", 5), 0, 64, 0, "stone"), { max_layers = 12 })
plan("Vault", "Dug under the keep: a 9 x 9 room 3 high, two blocks of rock over it, reached by a stair through the keep's floor,\nwith a double chest per category of item along the walls (food, weapons, armor, equipment, valuables,\nmaterials, building blocks). When a category fills up, another floor is dug 5 blocks lower.", list(C("vault", 6), 0, 64, 0, "stone", 0))
plan("House (castle village)", "Seven of them between the first and second walls: walls 3 high of the wall block with spruce log corners,\na doorway toward the street, a glass-pane window in the middle of each other wall, a pitched roof of spruce\nstairs with a plank ridge. A paved floor under each.", list(C("house", 6), 0, 64, 0, "stone"))
plan("Farm plot", "Seven 9 x 9 plots between the second and third walls: dirt, and water in the middle (the builders bring it in\nbuckets), which keeps the whole plot hydrated. `castle { action = \"farms\" }` tills and sows them.", list(C("plot", 4), 0, 64, 0))
plan("Animal pen", "Four fenced pens (cow, sheep, chicken, pig), stocked by luring animals in (`castle { action = \"ranch\" }`).", list(C("pen", 4), C("pens", 2)(0, 0)[1], 64, "spruce_fence"))
plan("Gatehouse", "Around the south gate of the outer wall.", list(C("gatehouse", 5), 0, 64, 0, "stone"))
plan("Well", "A 2 x 2 pool in the strip between the towers and the moat, filled at two opposite corners: the other two fill\nthemselves, so buckets taken from it never empty it. The moat is filled from it.", list(C("well", 4), 0, 64, 0))
local m = list(C("moat", 4), 0, 64, 0)
local poured_x = 0
for _, b in ipairs(list(C("moat_water", 4), 0, 64, 0)) do
    m[#m + 1] = b
    if b[3] < -R3 then poured_x = b[1] end
end
local mcut, mused = section(m, poured_x, nil, -R3 + 2)
say("### Moat\n\nAround the third wall: 5 wide and 3 deep. The bots pour a bucket every few blocks along it (carried from the\nwell) and the water spreads between. Cross-section on the north side where a bucket is poured:\n\n```\n" .. mcut .. "\n```\n" .. legend_of(mused) .. "\n")
plan("Bridge", "A fixed wooden bridge over the moat from the gatehouse.", list(C("bridge", 4), 0, 64, 0))
plan("Fountain", "In the courtyard, where the paths meet.", list(C("fountain", 5), 0, 64, 0, "stone"))
say([[### Roads to the mines and to places found

`castle { action = "roads" }` and the road bot: a road 3 wide of cobblestone (by default) following the ground, with a
torch every few blocks, from the outer gate to each of the team's mines and to the places the explorer finds, a
bridge (a deck with a parapet on each side) over water and ravines, a signpost at each end saying where it leads, what
is there and how far, and a waypoint sign every 128 blocks on long roads.
]])

-- ------------------------------------------------------------------ the empire
tools, timers = {}, {}
load_plugin("empire")
roots = {}
for _, t in pairs(tools) do roots[#roots + 1] = t.run end
for _, f in ipairs(timers) do roots[#roots + 1] = f end
local E = crawl(roots)
say("## The empire town and the village\n")
say([[With `empire { start = true }` the team plays like an Age of Empires computer player and puts up these buildings in a
town east of the castle, one builder at a time, on a grid of lots 18 blocks apart (8 to a row). Each is a hut: a
cobblestone foundation, the space cleared, corner posts, walls with a doorway in the middle of the south side (none
when open), a flat roof and a second layer inset by one. In village mode every bot first builds its own home on a
lot, with a garden and a campfire, then a road to the others; once every house stands, each trade builds its shop.
]])
local o = { x = 0, y = 64, z = 0 }
for _, b in ipairs(E("BUILDINGS")) do
    plan(b.name .. " (" .. ({ "Dark", "Feudal", "Castle", "Imperial" })[b.age] .. " Age)", "Costs " ..
        (function() local t = {} for r, n in pairs(b.cost) do t[#t + 1] = n .. " " .. r end table.sort(t) return table.concat(t, ", ") end)() ..
        (b.house and ". One for every 5 bots." or "."), (b.make(o)))
end
plan("Home (village mode)", "Each bot's own house: 7 x 7, walls 4 high of planks on a cobblestone row with log corners, a pitched roof of oak\nstairs, a bed, a furnace and a double chest, a table (a fence under a pressure plate) between two chairs, a sign\nwith its owner's name by the door, and in front the lot's campfire with log seats and its walled garden plot\n(dirt, a composter, corner torches). Trees on the lot are cleared first, and dirt fills under the floor where the\nground is lower.", E("home", 2)(o, "Bot1"))
for _, trade in ipairs({ "bakers", "farmers", "butchers", "miners", "fishers", "blacksmiths", "lumberjacks", "carpenters" }) do
    local blocks, name = E("shop", 2)(o, trade)
    plan(name .. " (" .. trade .. "' shop)", "A 7 x 5 hut of planks with stripped log corners, a sign and what the trade works with" ..
        (trade == "farmers" and ", and a garden plot." or "."), blocks)
end
say([[### Village roads and walls

Each home gets a road (3 wide, stone stairs where it climbs, its columns shared so every builder uses the same heights)
joining it to the others. Once the village stands, a perimeter wall with arched gates and torches goes up along each
side of the village border (North, South, West and East Wall).
]])

-- ------------------------------------------------------------------ roles: the farmers' animal pen
say([[### Farmer's animal pen

On its own lot, south of the garden plot (lot x 9 to 14, z 8 to 13, when the road leaves room): 6 x 6, a fence
all round with a gate in the middle of the south side, the inside cleared. The farmer opens the gate, lures cows,
sheep, pigs or chickens in with their food, closes it and breeds them.

```
++++++
+....+
+....+
+....+
+....+
++=+++
```
`+` oak fence, `=` oak fence gate, `.` air
]])

-- ------------------------------------------------------------------ the program's own
say([[## Built by the program itself

### Crop farm (`make_farm`, the farmers' "make a farm")

A 9 x 9 field around water. With water beside soil within 32 blocks, the field is the soil around it. Otherwise
the farm makes its own water: on a village garden bed (a composter on its rim) or the soil nearest the bot, a trench
3 long and 1 deep with a bucket poured at each end. The middle block, between two sources, becomes one too: a source
that never runs dry, and the nearest water for the next buckets (the bots fill buckets at water that refills first:
two still neighbours or more, a lake, a river, a trench like this one). The bucket is filled at the nearest still
water, walked to, up to 96 blocks away; a bot with no bucket makes one first (3 iron ingots).

Then the field is tilled with the hoe and sown: wheat, carrots, potatoes and beetroot in rows (rows of different crops
grow faster), and pumpkin and melon seeds every other block on the outer ring, so the fruit grows on the grass outside
the field. With no seeds, the bot breaks grass for wheat seeds first; pumpkins and melon slices it carries are crafted
into seeds. A torch lights the middle.

```
. . . . . . . . . . .      f  farmland with a crop
. s f s f s f s f s .      s  pumpkin or melon stem (outer ring, every other block)
. f f f f f f f f f .      ~  water: the middle one never runs dry
. s f f f f f f f s .      .  grass, where pumpkins and melons grow
. f f f f f f f f f .
. s f f ~ ~ ~ f f s .
. f f f f f f f f f .
. s f f f f f f f s .
. f f f f f f f f f .
. s f s f s f s f s .
. . . . . . . . . . .
```

The farm tool (and the autopilot when anything is ripe within 16 blocks) harvests and replants: ripe wheat, carrots,
potatoes, beetroot and nether wart, replanted from their own drops; sugar cane cut from its second block up (the base
grows again); pumpkins and melons grown from a stem (the stem gives more); sweet berry bushes with berries (picked
with a right-click, the bush stays). Then the farm grows, 16 new spots a round at most: crops on any bare farmland,
more soil tilled within 4 blocks of the field's water while seeds last, and sugar cane planted on soil or sand beside
water that has no field. Bone meal goes on what is still growing. The berry picker (`berries`) tends a patch of sweet
berry bushes and plants berries on the grass around it so the patch spreads.

### Shelter

`team { objective = "build", plan = "shelter", x1 =, y1 =, z1 =, size = 5 }` (the web panel's "Build shelter"): a
first-night shelter, a square of outer width `size` (5 by default) from its lowest corner, walls 3 high, a flat
roof and a doorway in the middle of the north side, of whatever blocks the bots carry.

### Wide mine

`mine` and the miners: a staircase 3 wide, going down a block a step (4 high, with a cobblestone stair across each
step so it can be walked back up) to the level where the ore is most common, then galleries 3 wide and 3 high along
it, a torch on the wall every 6 blocks. It stops before water, lava and bedrock. The entrance is recorded as a mine:
the castle's roads lead to it.

### Tunnels

To reach buried ore, follow the leader underground or get out of a hole: a tunnel 3 high and, with a pickaxe, 3 wide,
with a cobblestone stair on every step up or down (crafted from the cobblestone dug), a torch every 6 blocks. The side
lanes are left where digging them is unsafe. Narrower or lower slices, and steps without a stair, cost the bot reward
(see the neural network document).

## How a plan is built

A plan is a list of blocks. The builder (a bot with a build order, alone or as a team) takes the next block that can
be placed, lowest first, and for each one: digs out what is in the way, places it from the inventory, or else gets
it: crafts it, smelts it, makes what its recipe lacks, mines or cuts the raw materials, takes it from a chest known
to hold it. `water` is not a block to place: the bot makes a bucket, fills it at the nearest still water (one that
refills first) and empties it there. Blocks it cannot place are skipped and tried again later.
]])

io.write(table.concat(doc, "\n"))
