-- Castle plugin: levels the ground, then has the whole team build, from the centre outwards:
--   torches over the whole site and 12 blocks around it, a paved floor, a vault under the keep, the keep, the first wall, a village, the second wall, farm plots,
--   four pens (cows, sheep, chickens, pigs) that the bots then stock by leading animals in,
--   the third wall, and a moat around it all (a ditch 5 wide and 3 deep, crossed by a strip of ground in front
--   of the south gate).
-- The vault grows by itself: when a category's chests are full, another floor is dug under the last one.
-- The vault holds a double chest per category of item (food, weapons, armor, equipment, valuables, materials,
-- building blocks); once it is built the bots store there, sorted, and look there first for what they need.
-- The south gate of the third wall has a gatehouse, and a wooden bridge leads from it over the moat.
-- Each wall is 5 blocks wide, a 3-wide walkway with a raised block on each side, and has a tower on each of
-- its four corners.
--
--   castle { action = "plan",  x, y, z }   what would be built, and how many blocks
--   castle { action = "start", x, y, z }   level the site and build everything, stage by stage
--   castle { action = "status" }           which stage is under way, blocks left per bot
--   castle { action = "farms" }            once the plots are built: water, till and sow them
--   castle { action = "ranch" }            once the pens are built: lead animals into them and breed them
--   castle { action = "water" }            for a moat already dug: the team makes buckets, a well, and fills it
--   castle { action = "roads" }            once the castle stands: roads from its outer gate to the team's mines
--   castle { action = "explore", bot }     an explorer: finds structures itself (/locate) and lays a road to each,
--                                          with signposts at both ends and a waypoint sign every 128 blocks
--   castle { action = "stop" }             forget the job (bots finish the stage they are on)
--
-- x z is the centre, y the level of the floor to stand on (the walls start at y, the ground is y - 1).
-- Building is done by the team's "build" objective with plan = "blocks": this file only decides which
-- block goes where and hands the stages over one at a time, a timer starting the next when all bots are done.

local R1, R2, R3 = 36, 78, 126     -- half-sizes of the three square walls, to their outer face
local THICK = 5                    -- walls are 5 blocks wide: a 3-wide walkway in the middle,
local PARAPET = 1                  --   and 1 block on each side standing 1 block higher
local KEEP = 12                    -- half-size of the keep (25x25: it towers over the rest, as in the reference picture)
local TOWER = 3                    -- half-size of the corner towers (7x7, they wrap the wall's corner)
local MOAT_GAP = 2                 -- the moat: a ditch around the third wall, this far out from its towers,
local MOAT_WIDE = 5                --   this wide
local MOAT_DEEP = 3                --   and this deep
local OUTER = R3 + 1 + MOAT_GAP + MOAT_WIDE   -- half-size of the whole site, moat included
local ROOF = "spruce"               -- wood of the houses' roofs (stairs and ridge)
local CHUNK = 3500                 -- blocks per hand-over (the build objective takes 4096 at most)
local AIR = "air"

local job = nil                    -- { stages = { { name, blocks } }, index }

-- What the plugin is doing is kept in files, so a restart of the program picks it up again: the castle job
-- (how it was started and which stage it was at) and the road builder
local JOB_FILE, ROADS_FILE = "../castle_job.lua", "../roadbot_job.lua"
local roadman                      -- the road builder (see roadbot): not one of the castle's builders
local function serialize(v)
    if type(v) == "table" then
        local parts = {}
        for k, x in pairs(v) do parts[#parts + 1] = "[" .. serialize(k) .. "]=" .. serialize(x) end
        return "{" .. table.concat(parts, ",") .. "}"
    end
    return type(v) == "string" and string.format("%q", v) or tostring(v)
end
local function save(path, value)
    if value == nil then os.remove(path) return end
    local f = io.open(path, "w")
    if f then f:write("return ", serialize(value)); f:close() end
end
local function restore(path)
    local f = io.open(path, "r")
    if not f then return nil end
    local text = f:read("a")
    f:close()
    local chunk = load(text, path, "t", {})
    local ok, value = pcall(chunk or function() end)
    return ok and value or nil
end
local castle_saved = restore(JOB_FILE)   -- { args, index }: the job to take up again once the bots are back
local loaded_at = os.time()
local job_site = nil               -- { x, y, z } of the last castle started, for the farms
local ranch_due = nil              -- timer ticks until the next try at stocking the pens, nil when not trying
local ranch_tries = 0

local function add(list, x, y, z, item) list[#list + 1] = { x, y, z, item } end

-- Texture: a wall all of one block looks flat. About one block in five of the stone is cobblestone instead,
-- scattered by position (the same spots every time the plan is made, so a job taken up again matches)
local function textured(item, x, y, z)
    if item ~= "stone" and item ~= "minecraft:stone" then return item end
    local h = (x * 73856093 ~ y * 19349663 ~ z * 83492791) % 5
    return h == 0 and "cobblestone" or item
end
local function add_stone(list, x, y, z, item) add(list, x, y, z, textured(item, x, y, z)) end

-- Square wall with its outer face at half-size r, solid and 5 blocks wide from y1 up. The 3 middle blocks
-- stop at y2: their top is the walkway. The block on each side goes 1 higher, every other block:
-- crenellated, as in the reference picture. A gate 3 wide and 3 high goes through it to the south
local function wall(list, cx, cz, r, y1, y2, item)
    for x = cx - r, cx + r do
        for z = cz - r, cz + r do
            local d = math.max(math.abs(x - cx), math.abs(z - cz))
            if d > r - THICK then
                local side = d > r - PARAPET or d <= r - THICK + PARAPET
                local top = (side and (x + z) % 2 == 0) and y2 + 1 or y2
                local gate = z > cz and math.abs(x - cx) <= 1
                for y = y1, top do
                    if not (gate and y <= y1 + 2) then add_stone(list, x, y, z, item) end
                end
            end
        end
    end
    -- Buttresses on the outer face every 9 blocks, up to a block below the walkway (not by the gate)
    for t = -r + 4, r - 4, 9 do
        for _, b in ipairs({ { cx + t, cz - r - 1 }, { cx + t, cz + r + 1 }, { cx - r - 1, cz + t }, { cx + r + 1, cz + t } }) do
            if not (b[2] == cz + r + 1 and math.abs(b[1] - cx) <= 3) then
                for y = y1, y2 - 1 do add_stone(list, b[1], y, b[2], item) end
            end
        end
    end
end

-- Corner tower, as in the reference picture: round (radius TOWER + 1, it wraps the wall's corner), hollow,
-- up to `top` with a floor there, then a cone roof narrowing one block a level and a spire on its point
local function tower(list, tx, tz, y1, top, item)
    local r = TOWER + 1
    for dx = -r, r do
        for dz = -r, r do
            local d = math.sqrt(dx * dx + dz * dz)
            if d <= r + 0.5 then
                if d > r - 0.5 then
                    -- Arrow slits: a gap 1 wide and 2 high in the middle of each side, well above the ground
                    local slit = (dx == 0 or dz == 0)
                    for y = y1, top - 1 do
                        if slit and (y == y1 + 3 or y == y1 + 4) then add(list, tx + dx, y, tz + dz, AIR)
                        else add_stone(list, tx + dx, y, tz + dz, item) end
                    end
                end
                add_stone(list, tx + dx, top, tz + dz, item)
            end
        end
    end
    -- The cone: a ring a level, each one smaller, from one wider than the tower (the eaves) to a point
    for level = 0, r + 1 do
        local ring = r + 1 - level
        for dx = -ring, ring do
            for dz = -ring, ring do
                local d = math.sqrt(dx * dx + dz * dz)
                if d <= ring + 0.5 and d > ring - 0.7 then add_stone(list, tx + dx, top + 1 + level, tz + dz, item) end
            end
        end
    end
    local tip = top + r + 3
    for y = tip, tip + 2 do add(list, tx, y, tz, "cobblestone_wall") end
end

-- A wall with a tower on each of its four corners, 3 blocks higher than the walkway and centred on the
-- middle of the wall's width
local function walled(list, cx, cz, r, y1, y2, item)
    wall(list, cx, cz, r, y1, y2, item)
    local c = r - THICK // 2
    for _, sx in ipairs({ -1, 1 }) do
        for _, sz in ipairs({ -1, 1 }) do
            tower(list, cx + sx * c, cz + sz * c, y1, y2 + 3, item)
        end
    end
end

-- Middle of the free band between one wall's outer face and the next wall's inner face
local function between(inner, outer)
    local first, last = inner + 1, outer - THICK
    return first + (last - first) // 2
end

-- Small house: walls 3 high, doorway to the north, a window of glass pane at eye level in the middle of each
-- of the three other walls, and a pitched roof of wooden stairs: two rows on each side rising to a ridge
-- of planks, the gable ends closed with the wall block
-- door: the side of the doorway, as a step from the middle, { 0, -1 } north by default (the street side)
local function house(list, x0, y, z0, item, door_side, creative)
    if creative then item = "stone_bricks" end
    local stairs = creative and "stone_brick_stairs" or (ROOF .. "_stairs")
    local ridge = creative and "stone_bricks" or (ROOF .. "_planks")
    local ddx, ddz = table.unpack(door_side or { 0, -1 })
    for dy = 0, 2 do
        for dx = -2, 2 do
            for dz = -2, 2 do
                local edge = math.abs(dx) == 2 or math.abs(dz) == 2
                local mid = (dx == 0 or dz == 0) and edge
                local door = mid and dx == 2 * ddx and dz == 2 * ddz and dy <= 1
                local window = dy == 1 and mid and not (dx == 2 * ddx and dz == 2 * ddz)
                -- Wooden posts at the corners, the walls between of stone, as in the reference picture
                local corner = math.abs(dx) == 2 and math.abs(dz) == 2
                if edge and not door then
                    add(list, x0 + dx, y + dy, z0 + dz, window and "glass_pane" or (corner and not creative and (ROOF .. "_log") or item))
                end
            end
        end
    end
    local door_facing = ddz < 0 and "south" or ddz > 0 and "north" or ddx < 0 and "east" or "west"
    add(list, x0 + 2 * ddx, y, z0 + 2 * ddz, ROOF .. "_door[facing=" .. door_facing .. ",half=lower]")
    add(list, x0 + 2 * ddx, y + 1, z0 + 2 * ddz, ROOF .. "_door[facing=" .. door_facing .. ",half=upper]")
    for dx = -2, 2 do
        -- A stair's high side is the way it faces: toward the ridge, so north on the south slope
        add(list, x0 + dx, y + 3, z0 - 2, stairs .. "[facing=south,half=bottom]")
        add(list, x0 + dx, y + 3, z0 + 2, stairs .. "[facing=north,half=bottom]")
        add(list, x0 + dx, y + 4, z0 - 1, stairs .. "[facing=south,half=bottom]")
        add(list, x0 + dx, y + 4, z0 + 1, stairs .. "[facing=north,half=bottom]")
        add(list, x0 + dx, y + 5, z0, ridge)
    end
    for _, dx in ipairs({ -2, 2 }) do
        for dz = -1, 1 do add(list, x0 + dx, y + 3, z0 + dz, item) end
        add(list, x0 + dx, y + 4, z0, item)
    end
end

-- The keep, the tallest thing as in the reference picture: 25x25, two floors (the ground floor 6 high, the upper
-- one 5), stained-glass windows every 4 blocks, a gate 3 wide to the south, then a roof stepping in a block a
-- level to a point with a spire. Creative: furnished (keep_rooms), with the lower level beside the vault
local KEEP_HIGH = 12
-- Paintings and armor stands are entities, not blocks: summoned once the rooms stand (see the keep stage)
local KEEP_ENTITIES = {}

-- Each spot once, the last word wins (the rooms are carved out of the shell, a door out of a wall)
local function grid()
    local cells, order = {}, {}
    local g = {}
    function g.set(x, y, z, item)
        local k = x .. "," .. y .. "," .. z
        if not cells[k] then order[#order + 1] = k end
        cells[k] = { x, y, z, item }
    end
    function g.box(x1, y1, z1, x2, y2, z2, item)
        for x = math.min(x1, x2), math.max(x1, x2) do
            for yy = math.min(y1, y2), math.max(y1, y2) do
                for z = math.min(z1, z2), math.max(z1, z2) do g.set(x, yy, z, item) end
            end
        end
    end
    function g.emit(list) for _, k in ipairs(order) do list[#list + 1] = cells[k] end end
    return g
end

local function keep_shell(g, cx, y, cz, item, creative)
    for x = cx - KEEP, cx + KEEP do
        for z = cz - KEEP, cz + KEEP do
            local edge = math.abs(x - cx) == KEEP or math.abs(z - cz) == KEEP
            local corner = math.abs(x - cx) == KEEP and math.abs(z - cz) == KEEP
            local t = math.abs(z - cz) == KEEP and x - cx or z - cz
            local window = edge and not corner and math.abs(t) <= KEEP - 2 and t % 4 == 2
            for dy = 0, KEEP_HIGH - 1 do
                if edge then
                    local pane = window and (dy == 2 or dy == 3 or dy == 8 or dy == 9)
                    local glass = creative and (dy < 6 and "red_stained_glass_pane" or "light_blue_stained_glass_pane") or "glass_pane"
                    g.set(x, y + dy, z, pane and glass or textured(item, x, y + dy, z))
                end
            end
            g.set(x, y + 6, z, item)
            g.set(x, y + KEEP_HIGH, z, item)
        end
    end
    -- The gate: 3 wide, 3 high
    g.box(cx - 1, y, cz + KEEP, cx + 1, y + 2, cz + KEEP, AIR)
    for level = 1, KEEP + 1 do
        local h = KEEP + 1 - level
        for x = cx - h, cx + h do
            for z = cz - h, cz + h do
                if math.max(math.abs(x - cx), math.abs(z - cz)) == h then g.set(x, y + KEEP_HIGH + level, z, textured(item, x, y, z)) end
            end
        end
    end
    local tip = y + KEEP_HIGH + KEEP + 2
    for dy = 0, 3 do g.set(cx, tip + dy, cz, "cobblestone_wall") end
end

-- The rooms. Coordinates from the middle (dx dz) and from the ground floor's floor (dy 0 is the first block above
-- it). Ground floor: the main hall (ballroom, throne at the north wall) to the north; an entrance corridor from
-- the gate, the kitchen to its west and the staff's bedroom to its east. Upper floor: the main bedroom to the north,
-- a corridor, three guest bedrooms to the south. A secret door from the main bedroom (a stone button low on its west
-- wall) to a hidden passage and a ladder shaft down to the armory. Lower level, beside the vault: the armory to the
-- west, the dungeon to the east (six cells, an iron door each opened by a button in the corridor)
local function keep_rooms(g, cx, y, cz, entities)
    local function S(dx, dy, dz, item) g.set(cx + dx, y + dy, cz + dz, item) end
    local function B(dx1, dy1, dz1, dx2, dy2, dz2, item) g.box(cx + dx1, y + dy1, cz + dz1, cx + dx2, y + dy2, cz + dz2, item) end
    local wall, trim = "stone_bricks", "polished_andesite"
    local I = KEEP - 1
    -- Everything inside cleared, both floors
    B(-I, 0, -I, I, 5, I, AIR)
    B(-I, 7, -I, I, KEEP_HIGH - 1, I, AIR)
    local function painting(dx, dy, dz, facing, variant)
        entities[#entities + 1] = { kind = "painting", x = cx + dx, y = y + dy, z = cz + dz, facing = facing, variant = variant }
    end
    local function chandelier(dx, ceiling, dz)
        S(dx, ceiling - 1, dz, "chain[axis=y]")
        S(dx, ceiling - 2, dz, "lantern[hanging=true]")
    end
    -- Main hall: red wool, a white border, quartz pillars with gold capitals, chandeliers, the throne
    B(-I, -1, -I, I, -1, 3, "white_wool")
    B(-I + 1, -1, -I + 1, I - 1, -1, 2, "red_wool")
    for _, px in ipairs({ -7, 7 }) do
        for _, pz in ipairs({ -7, -1 }) do
            B(px, 0, pz, px, 4, pz, "quartz_pillar[axis=y]")
            S(px, 5, pz, "gold_block")
        end
    end
    for _, c in ipairs({ { -4, -4 }, { 4, -4 }, { 0, -8 }, { 0, 0 } }) do chandelier(c[1], 6, c[2]) end
    B(-2, 0, -I, 2, 0, -I + 1, trim)                    -- the dais
    S(0, 1, -I + 1, "quartz_stairs[facing=north,half=bottom]")
    S(-1, 1, -I + 1, "gold_block"); S(1, 1, -I + 1, "gold_block")
    S(0, 1, -I, "gold_block"); S(0, 2, -I, "gold_block")
    for _, px in ipairs({ -8, -4, 4, 8 }) do
        painting(px, 2, -I, "south", "minecraft:kebab")
        painting(px, 2, 3, "north", "minecraft:aztec")
    end
    -- The vault's stair down (dug through the floor by the vault stage): kept open, a fence around it
    B(-2, -1, 0, 1, -1, 0, AIR)
    B(-2, 0, -1, 1, 0, -1, "spruce_fence")
    B(-2, 0, 1, 1, 0, 1, "spruce_fence")
    S(-3, 0, 0, "spruce_fence")
    -- The stair up, along the hall's south side: 6 steps east, through a gap in the floor above
    for k = 0, 5 do
        S(-10 + k, k, 1, "stone_brick_stairs[facing=east,half=bottom]")
        if k > 0 then B(-10 + k, 0, 1, -10 + k, k - 1, 1, wall) end
        S(-10 + k, 6, 1, AIR)
    end
    -- The wall between the hall and the south rooms, an arch through the middle
    B(-I, 0, 4, I, 5, 4, wall)
    B(-1, 0, 4, 1, 2, 4, AIR)
    -- Entrance corridor, from the gate
    B(-1, -1, 5, 1, -1, I, trim)
    B(-2, 0, 5, -2, 5, I, wall)
    B(2, 0, 5, 2, 5, I, wall)
    S(0, 5, 8, "lantern[hanging=true]")
    -- Kitchen (west of the corridor)
    B(-I, -1, 5, -3, -1, I, trim)
    S(-2, 0, 8, "spruce_door[facing=west,half=lower]"); S(-2, 1, 8, "spruce_door[facing=west,half=upper]")
    S(-I, 0, 6, "smoker[facing=east]"); S(-I, 0, 7, "furnace[facing=east]"); S(-I, 0, 8, "smoker[facing=east]")
    S(-I, 0, 9, "barrel[facing=east]"); S(-I, 0, 10, "barrel[facing=east]"); S(-I, 1, 9, "barrel[facing=east]")
    S(-I, 0, I, "water_cauldron[level=3]"); S(-I + 1, 0, I, "crafting_table")
    for dx = -8, -5 do
        S(dx, 0, 8, "spruce_slab[type=top]")
        S(dx, 0, 7, "spruce_stairs[facing=north,half=bottom]")
        S(dx, 0, 9, "spruce_stairs[facing=south,half=bottom]")
    end
    S(-7, 5, 6, "lantern[hanging=true]"); S(-7, 5, 10, "lantern[hanging=true]")
    -- Staff bedroom (east of the corridor): three beds, a barrel at the foot of each
    B(3, -1, 5, I, -1, I, "spruce_planks")
    S(2, 0, 8, "spruce_door[facing=east,half=lower]"); S(2, 1, 8, "spruce_door[facing=east,half=upper]")
    for _, bz in ipairs({ 6, 8, 10 }) do
        S(I - 1, 0, bz, "white_bed[facing=east,part=foot]")
        S(I, 0, bz, "white_bed[facing=east,part=head]")
        S(I - 2, 0, bz, "barrel[facing=up]")
    end
    S(6, 5, 8, "lantern[hanging=true]")

    -- Upper floor (its floor is dy 6). The corridor, red wool, from the stair
    B(-I, 6, 1, I, 6, 3, "red_wool")
    for k = 0, 5 do S(-10 + k, 6, 1, AIR) end
    for _, lx in ipairs({ -6, 0, 6 }) do S(lx, KEEP_HIGH - 1, 2, "lantern[hanging=true]") end
    B(-I, 7, 0, I, KEEP_HIGH - 1, 0, wall)
    B(-I, 7, 4, I, KEEP_HIGH - 1, 4, wall)
    -- Main bedroom: purple wool, a king's bed of two, gold nightstands with lanterns, bookshelves, a chandelier
    B(-I, 6, -I, I, 6, -1, "purple_wool")
    S(3, 7, 0, "spruce_door[facing=north,half=lower]"); S(3, 8, 0, "spruce_door[facing=north,half=upper]")
    for _, bx in ipairs({ 0, 1 }) do
        S(bx, 7, -I, "red_bed[facing=north,part=head]")
        S(bx, 7, -I + 1, "red_bed[facing=north,part=foot]")
    end
    for _, nx in ipairs({ -1, 2 }) do S(nx, 7, -I, "gold_block"); S(nx, 8, -I, "lantern[hanging=false]") end
    B(I, 7, -I + 1, I, 9, -2, "bookshelf")
    S(-4, 7, -2, "chest"); S(-3, 7, -2, "chest")
    chandelier(0, KEEP_HIGH, -6)
    painting(-4, 9, -I, "south", "minecraft:wasteland")
    painting(5, 9, -I, "south", "minecraft:plant")
    painting(I, 10, -6, "west", "minecraft:bomb")
    -- The secret door, in the wall x -10 between the bedroom and a hidden passage at x -11 (z -11..-6). The
    -- opening at z -11; two sticky pistons at z -9 hold the two door blocks in it, pushed by a redstone torch at
    -- z -8 (beside the lower one; the block over the torch passes the power to the upper one). A button on either
    -- side of the wall block at z -7 under the torch puts the torch out: the pistons pull the door blocks aside
    -- while it is held, and the door shuts behind
    B(-10, 7, -I, -10, KEEP_HIGH - 1, -6, wall)
    B(-I, 9, -I, -I, KEEP_HIGH - 1, -5, wall)
    B(-I, 7, -10, -I, 8, -6, AIR)
    B(-I, 7, -5, -I, 8, -5, wall)
    B(-10, 7, -I, -10, 8, -I, AIR)                           -- the opening (shut once the torch pushes the pistons)
    S(-10, 7, -10, wall); S(-10, 8, -10, wall)               -- the door blocks, beside it
    S(-10, 7, -9, "sticky_piston[facing=north]"); S(-10, 8, -9, "sticky_piston[facing=north]")
    S(-10, 7, -8, "redstone_wall_torch[facing=north]")
    S(-10, 8, -8, wall)
    S(-10, 7, -7, wall)
    S(-9, 7, -7, "stone_button[face=wall,facing=east]")
    S(-I, 7, -7, "stone_button[face=wall,facing=west]")
    -- The shaft: x -11 z -11, from the lower level up to the passage, a ladder on the keep's north wall
    B(-10, 0, -I, -10, 5, -10, wall)
    B(-I, 0, -10, -I, 5, -10, wall)
    -- Guest bedrooms: three, light blue wool, a bed, a barrel, a lantern, a painting
    B(-4, 7, 5, -4, KEEP_HIGH - 1, I, wall)
    B(4, 7, 5, 4, KEEP_HIGH - 1, I, wall)
    for i, gx in ipairs({ -8, 0, 8 }) do
        B(gx - 3, 6, 5, gx + 3, 6, I, "light_blue_wool")
        S(gx, 7, 4, "spruce_door[facing=south,half=lower]"); S(gx, 8, 4, "spruce_door[facing=south,half=upper]")
        S(gx, 7, I, "blue_bed[facing=south,part=head]"); S(gx, 7, I - 1, "blue_bed[facing=south,part=foot]")
        S(gx + 1, 7, I, "barrel[facing=up]"); S(gx - 1, 7, I, "gold_block"); S(gx - 1, 8, I, "lantern[hanging=false]")
        S(gx, KEEP_HIGH - 1, 8, "lantern[hanging=true]")
        painting(gx + 2, 10, I, "north", ({ "minecraft:alban", "minecraft:aztec2", "minecraft:kebab" })[i])
    end

    -- Lower level: the vault (its own stage) is x -4..4 z -4..4, dy -5..-3. Armory to the west, dungeon to the east,
    -- the same height, a doorway to each from the vault
    B(-KEEP, -6, -KEEP, -5, -2, KEEP, wall)
    B(-I, -5, -I, -6, -3, I, AIR)
    B(-I, -6, -I, -6, -6, I, trim)
    B(-5, -5, 0, -5, -4, 0, AIR)
    S(-I, -5, -8, "anvil[facing=north]"); S(-I, -5, -6, "grindstone[face=floor,facing=north]")
    S(-I, -5, -4, "smithing_table"); S(-I, -5, -2, "blast_furnace[facing=east]")
    for bz = 0, 8, 2 do S(-I, -5, bz, "barrel[facing=east]"); S(-I, -4, bz, "barrel[facing=east]") end
    for _, lz in ipairs({ -7, 0, 7 }) do S(-8, -3, lz, "lantern[hanging=true]") end
    for _, az in ipairs({ -9, -6, 3, 6, 9 }) do
        entities[#entities + 1] = { kind = "armor_stand", x = cx - 7, y = y - 5, z = cz + az, facing = "west" }
    end
    B(5, -6, -KEEP, KEEP, -2, KEEP, wall)
    B(6, -5, -I, I, -3, I, AIR)
    B(6, -6, -I, I, -6, I, trim)
    B(5, -5, 2, 5, -4, 2, AIR)
    for _, w in ipairs({ -12, -8, -4, 0, 4, 8 }) do
        if w > -12 then B(8, -5, w, I, -3, w, wall) end
        -- The front: iron bars, an iron door beside the wall, a button on that wall's end in the corridor
        B(8, -5, w + 1, 8, -3, w + 3, "iron_bars")
        S(8, -5, w + 1, "iron_door[facing=west,half=lower]"); S(8, -4, w + 1, "iron_door[facing=west,half=upper]")
        S(8, -3, w, wall)
        S(7, -4, w, "stone_button[face=wall,facing=west]")
        S(I, -3, w + 3, "cobweb")
    end
    for _, lz in ipairs({ -8, 0, 8 }) do S(7, -3, lz, "lantern[hanging=true]") end
    -- Last, through all the floors it crosses: the shaft's ladder, on the keep's north wall (rock below the ground)
    for dy = -5, 8 do
        S(-I, dy, -I, "ladder[facing=south]")
        if dy < -1 then S(-I, dy, -KEEP, wall) end
    end
end

-- The keep's paintings and armor stands (entities: not in a build plan), summoned by a creative bot; one already
-- there (within a block) is left. Painting NBT as of 1.21 (facing 0 south 1 west 2 north 3 east, variant, block_pos;
-- TileX/Y/Z for older servers); armor stands with the equipment component of 1.21.5+
local function summon_keep_entities()
    local who
    for _, b in ipairs(bot.call("bots")) do
        local ok, st = pcall(bot.call, "status", { bot = b.name })
        if ok and st.game_mode == "creative" and st.dimension == "minecraft:overworld" and not (roadman and roadman.bot == b.name) then who = b.name; break end
    end
    if not who or #KEEP_ENTITIES == 0 then return 0 end
    local e = KEEP_ENTITIES[1]
    pcall(bot.call, "goto", { bot = who, x = e.x, y = e.y + 8, z = e.z, tolerance = 6 })
    local summoned = 0
    for _, kind in ipairs({ "painting", "armor_stand" }) do
        local ok, near = pcall(bot.call, "entities", { bot = who, type = kind })
        near = ok and type(near) == "table" and near or {}
        for _, t in ipairs(KEEP_ENTITIES) do
            if t.kind == kind then
                local there = false
                for _, n in ipairs(near) do
                    local p = n.position
                    if p and math.abs(p.x - (t.x + 0.5)) < 1.2 and math.abs(p.y - t.y) < 1.5 and math.abs(p.z - (t.z + 0.5)) < 1.2 then there = true end
                end
                if not there then
                    local cmd
                    if kind == "painting" then
                        local f = ({ south = 0, west = 1, north = 2, east = 3 })[t.facing]
                        cmd = string.format('/summon minecraft:painting %d %d %d {facing:%db,Facing:%db,variant:"%s",block_pos:[I;%d,%d,%d],TileX:%d,TileY:%d,TileZ:%d}',
                            t.x, t.y, t.z, f, f, t.variant, t.x, t.y, t.z, t.x, t.y, t.z)
                    else
                        local yaw = ({ south = 0, west = 90, north = 180, east = -90 })[t.facing]
                        cmd = string.format('/summon minecraft:armor_stand %.1f %d %.1f {Rotation:[%df,0f],ShowArms:1b,equipment:{head:{id:"minecraft:iron_helmet"},chest:{id:"minecraft:iron_chestplate"},legs:{id:"minecraft:iron_leggings"},feet:{id:"minecraft:iron_boots"},mainhand:{id:"minecraft:iron_sword"}}}',
                            t.x + 0.5, t.y, t.z + 0.5, yaw)
                    end
                    pcall(bot.call, "chat", { bot = who, message = cmd })
                    summoned = summoned + 1
                end
            end
        end
    end
    return summoned
end

local function keep(list, cx, y, cz, item, creative)
    local g = grid()
    keep_shell(g, cx, y, cz, item, creative)
    if creative then
        KEEP_ENTITIES = {}
        keep_rooms(g, cx, y, cz, KEEP_ENTITIES)
    else
        -- The way up: a gap in the upper floor, at the foot of the north wall
        g.set(cx, y + 6, cz - KEEP + 1, AIR)
    end
    g.emit(list)
end

-- Floor: the ground itself, paved. Inside the first wall (courtyard and keep) and under each house
local function floor(list, cx, y, cz, item)
    local r = R1 - THICK
    for x = cx - r, cx + r do
        for z = cz - r, cz + r do add(list, x, y - 1, z, item) end
    end
end

local function house_floor(list, x0, y, z0, item)
    for dx = -2, 2 do
        for dz = -2, 2 do add(list, x0 + dx, y - 1, z0 + dz, item) end
    end
end

-- The vault: a room 9x9 and 3 high dug under the keep, two blocks of rock left above it, reached by a
-- stair in the middle of the keep's floor. Along its walls, a double chest for each category of item.
local VAULT = 4                    -- half-size of the vault room
local DEPTH = 5                    -- its floor level, below the castle's

-- Floor k of the vault (0 is the first) is 5 blocks under the one above: the room and 2 blocks of rock.
-- Its chests are declared as "food", then "food#2" on the next floor down, and so on
local function vault_chests(cx, y, cz, k)
    k = k or 0
    local v = y - DEPTH * (k + 1)
    local n = k == 0 and "" or ("#" .. (k + 1))
    return {
        { "food" .. n,      cx - 4, v, cz - 4, cx - 3, v, cz - 4 },
        { "weapons" .. n,   cx - 1, v, cz - 4, cx,     v, cz - 4 },
        { "armor" .. n,     cx + 2, v, cz - 4, cx + 3, v, cz - 4 },
        { "equipment" .. n, cx - 4, v, cz + 4, cx - 3, v, cz + 4 },
        { "valuables" .. n, cx - 1, v, cz + 4, cx,     v, cz + 4 },
        { "materials" .. n, cx + 2, v, cz + 4, cx + 3, v, cz + 4 },
        { "building" .. n,  cx + 4, v, cz - 1, cx + 4, v, cz },
    }
end

local function vault(list, cx, y, cz, item, k)
    k = k or 0
    local top = y - DEPTH * k        -- where one stands on the level above: the keep's floor, or the floor above
    local v = top - DEPTH
    -- The room
    for x = cx - VAULT, cx + VAULT do
        for z = cz - VAULT, cz + VAULT do
            for yy = v, v + 2 do add(list, x, yy, z, AIR) end
        end
    end
    -- The stair: a slot through the floor above and the rock under it, and three steps of rising height
    -- below. One floor's stair is 2 blocks beside the next one's, so they don't cut into each other
    local sx, row = cx - 2, cz + (k % 2) * 2
    for i = 0, 3 do
        add(list, sx + i, top - 2, row, AIR)
        add(list, sx + i, top - 1, row, AIR)
    end
    for step = 0, 2 do
        for yy = v, v + step do add(list, sx + step, yy, row, item) end
    end
    -- The chests: two side by side join into a double chest. In front of each, a sign with what goes in it
    for _, c in ipairs(vault_chests(cx, y, cz, k)) do
        add(list, c[2], c[3], c[4], "chest")
        add(list, c[5], c[6], c[7], "chest")
        -- One step into the room from the wall the chest stands against
        local sgx = c[2] == cx + VAULT and c[2] - 1 or c[2]
        local sgz = c[4] == cz - VAULT and c[4] + 1 or (c[4] == cz + VAULT and c[4] - 1 or c[4])
        add(list, sgx, v, sgz, "spruce_sign|" .. c[1]:gsub("#.*", "") .. "|floor " .. (k + 1) .. "||")
    end
end

-- The moat: a ditch all around, outside the third wall, unbroken: a wooden bridge crosses it in front of
-- the south gate, and the roads start from the far end of that bridge
local function moat_ring(cx, cz)
    local first, last = R3 + 2 + MOAT_GAP, OUTER
    local cells = {}
    for x = cx - last, cx + last do
        for z = cz - last, cz + last do
            local d = math.max(math.abs(x - cx), math.abs(z - cz))
            if d >= first and d <= last then cells[#cells + 1] = { x, z } end
        end
    end
    return cells
end

-- The gatehouse: a block of masonry 9 wide around the south gate of the third wall, through the whole
-- thickness of the wall and one block out, 3 higher than the wall's walkway, with the gate's passage
-- (3 wide, 3 high) running through it, a platform on top and battlements around it
local function gatehouse(list, cx, y, cz, item)
    local z1, z2 = cz + R3 - THICK + 1, cz + R3 + 1
    local top = y + 5
    for x = cx - 4, cx + 4 do
        for z = z1, z2 do
            local passage = math.abs(x - cx) <= 1
            for yy = y, top do
                if passage and yy <= y + 2 then add(list, x, yy, z, AIR) else add(list, x, yy, z, item) end
            end
            local edge = math.abs(x - cx) == 4 or z == z1 or z == z2
            if edge and (x + z) % 2 == 0 then add(list, x, top + 1, z, item) end
        end
    end
end

-- The bridge over the moat, from the gatehouse to the far bank: a deck of planks 3 wide at ground level,
-- a fence along each side. Built out from the bank, plank by plank
local function bridge(list, cx, y, cz)
    for z = cz + R3 + 2, cz + OUTER do
        for dx = -1, 1 do add(list, cx + dx, y - 1, z, ROOF .. "_planks") end
        if z >= cz + R3 + 2 + MOAT_GAP then
            add(list, cx - 2, y - 1, z, ROOF .. "_planks")
            add(list, cx + 2, y - 1, z, ROOF .. "_planks")
            add(list, cx - 2, y, z, ROOF .. "_fence")
            add(list, cx + 2, y, z, ROOF .. "_fence")
        end
    end
end

-- A well: a pool 2x2 and 1 deep in the strip between the towers and the moat, beside the way in. Two
-- buckets emptied in opposite corners make it a spring that never runs dry: the bots refill there
local function well(list, cx, y, cz)
    local wx, wz = cx + 5, cz + R3 + 2
    for dx = 0, 1 do
        for dz = 0, 1 do add(list, wx + dx, y - 1, wz + dz, AIR) end
    end
    add(list, wx, y - 1, wz, "water")
    add(list, wx + 1, y - 1, wz + 1, "water")
end

-- Water for the moat: a bucket emptied on its bottom every 10 blocks along the middle of the ditch (water
-- runs 7 blocks each way); the spots the water has already reached need no bucket
local function moat_water(list, cx, y, cz)
    local mid = R3 + 2 + MOAT_GAP + MOAT_WIDE // 2
    local bottom = y - MOAT_DEEP
    for t = -mid, mid, 10 do
        add(list, cx + t, bottom, cz - mid, "water")
        add(list, cx + mid, bottom, cz + t, "water")
        add(list, cx - mid, bottom, cz + t, "water")
        add(list, cx + t, bottom, cz + mid, "water")
    end
end

local function moat(list, cx, y, cz)
    for _, c in ipairs(moat_ring(cx, cz)) do
        for depth = 1, MOAT_DEEP do add(list, c[1], y - depth, c[2], AIR) end
    end
end

-- Paths of coarse dirt in the ground, as in the reference picture: the road from the keep's door through the
-- three south gates, and a ring through the village on the inner side of the houses. And a fountain on the
-- road in the middle of the village: a basin 5x5 with water in it and a pillar in the middle
local function paths(list, cx, y, cz, item)
    local d = between(R1, R2)
    local ring = d - 4
    local seen = {}
    local function path(x, z)
        local key = x .. "," .. z
        if not seen[key] then seen[key] = true; add(list, x, y - 1, z, "coarse_dirt") end
    end
    for z = cz + KEEP + 1, cz + R3 + 1 do
        for x = cx - 1, cx + 1 do
            -- (not under the walls: their gates have the castle's own floor)
            local under_wall = false
            for _, r in ipairs({ R1, R2, R3 }) do
                if z - cz > r - THICK and z - cz <= r then under_wall = true end
            end
            if not under_wall and math.abs(z - (cz + d)) > 2 then path(x, z) end
        end
    end
    for t = -ring, ring do
        path(cx + t, cz - ring); path(cx + t, cz + ring); path(cx - ring, cz + t); path(cx + ring, cz + t)
    end
end

local function fountain(list, cx, y, cz, item)
    local fz = cz + between(R1, R2)
    for dx = -2, 2 do
        for dz = -2, 2 do
            local rim = math.abs(dx) == 2 or math.abs(dz) == 2
            add(list, cx + dx, y - 1, fz + dz, item)
            if rim then add(list, cx + dx, y, fz + dz, item)
            elseif dx == 0 and dz == 0 then
                for dy = 0, 2 do add(list, cx, y + dy, fz, item) end
                add(list, cx, y + 3, fz, "lantern")
            else add(list, cx + dx, y, fz + dz, "water") end
        end
    end
end

-- Where the houses and the farm plots go: around the ring, leaving the road to the south gates free
local function around(cx, cz, d)
    return {
        { cx - d, cz - d }, { cx, cz - d }, { cx + d, cz - d },
        { cx - d, cz },                     { cx + d, cz },
        { cx - d, cz + d },                 { cx + d, cz + d },
    }
end

-- The village: houses along the street ring every 12 blocks, as close together as in the reference picture,
-- each one's door on the street (the side toward the keep). Not on the road to the gates and the fountain
local function houses(cx, cz)
    local d = between(R1, R2)
    local list = {}
    -- Along each side, evenly either side of its middle, then one house on each corner
    local n = (d - 6) // 12
    for k = -n, n do
        local t = k * 12
        for _, side in ipairs({ { 0, -1 }, { 0, 1 }, { -1, 0 }, { 1, 0 } }) do
            local sx, sz = side[1], side[2]
            local x = cx + (sx ~= 0 and sx * d or t)
            local z = cz + (sz ~= 0 and sz * d or t)
            if not (sz == 1 and math.abs(x - cx) <= 6) then
                list[#list + 1] = { x, z, { -sx, -sz } }
            end
        end
    end
    for _, sx in ipairs({ -1, 1 }) do
        for _, sz in ipairs({ -1, 1 }) do list[#list + 1] = { cx + sx * d, cz + sz * d, { 0, -sz } } end
    end
    return list
end
local function plots(cx, cz) return around(cx, cz, between(R2, R3)) end

-- Pens: four fenced squares 9x9 in the farm ring, between the plots, one for each kind of animal. The gate
-- is in the middle of the side that faces the castle's north-south axis
local function pens(cx, cz)
    local d = between(R2, R3)
    local list = {}
    for i, kind in ipairs({ { "cow", "minecraft:wheat" }, { "sheep", "minecraft:wheat" }, { "chicken", "minecraft:wheat_seeds" }, { "pig", "minecraft:carrot" } }) do
        local sx = (i % 2 == 1) and -1 or 1          -- west, east, west, east
        local sz = (i <= 2) and 1 or -1              -- the first two to the south, the others to the north
        local px, pz = cx + sx * 12, cz + sz * d
        list[i] = { type = kind[1], food = kind[2], x = px, z = pz, gate_x = px - sx * 4, gate_z = pz, out_x = px - sx * 6 }
    end
    return list
end

local function pen(list, p, y, fence)
    for dx = -4, 4 do
        for dz = -4, 4 do
            if math.abs(dx) == 4 or math.abs(dz) == 4 then
                local x, z = p.x + dx, p.z + dz
                add(list, x, y, z, (x == p.gate_x and z == p.gate_z) and (fence .. "_gate") or fence)
            end
        end
    end
end

-- Farm plot: 9x9 of dirt at ground level with a hole in the middle for the water (it wets 4 blocks around)
local function plot(list, px, y, pz)
    for dx = -4, 4 do
        for dz = -4, 4 do
            -- The middle: water (the builders bring it in buckets) keeps the whole 9x9 plot hydrated
            if dx == 0 and dz == 0 then add(list, px, y - 1, pz, "water") else add(list, px + dx, y - 1, pz + dz, "dirt") end
        end
    end
end

local viewer = nil                 -- the bot whose eyes are used to look at the ground (nil: the first one)
local function block_at(x, y, z)
    local ok, b = pcall(bot.call, "get_block", { x = x, y = y, z = z, bot = viewer })
    return ok and b.block or nil
end

-- Levelling: everything above the floor goes (12 blocks up, trees included), holes in the floor are filled
local function level(cx, y, cz, fill, reach)
    local list, unseen = {}, 0
    reach = reach or OUTER         -- out to the far bank of the moat
    for x = cx - reach, cx + reach do
        for z = cz - reach, cz + reach do
            local ground = block_at(x, y - 1, z)
            if ground == nil then
                unseen = unseen + 1
            else
                local function hollow(b) return b == "minecraft:air" or b == "minecraft:water" or b == "minecraft:lava" end
                if hollow(ground) then
                    add(list, x, y - 1, z, fill)
                    -- No floor over a void: the hole under it is filled with dirt (the bots dig it outside the site)
                    -- down to solid ground. ponytail: 24 deep at most, a deeper ravine keeps its bottom
                    for dy = 2, 25 do
                        local b = block_at(x, y - dy, z)
                        if b == nil or not hollow(b) then break end
                        add(list, x, y - dy, z, "dirt")
                    end
                end
                for dy = 0, 11 do
                    local b = block_at(x, y + dy, z)
                    -- Not the snow: in a snowy place it settles again as fast as it is cleared
                    if b and b ~= "minecraft:air" and b ~= "minecraft:snow" then add(list, x, y + dy, z, AIR) end
                end
            end
        end
    end
    -- From the centre outward: the keep and the inner walls stand on ground levelled first, and the edges
    -- (a valley to fill, a hill to cut) come last. Row by row from the west, the first stages were all a
    -- valley 50 blocks deep the bots could not get into, every one of them given up after 5 minutes
    for i, b in ipairs(list) do b.order = i end
    table.sort(list, function(a, b)
        local ra = math.max(math.abs(a[1] - cx), math.abs(a[3] - cz))
        local rb = math.max(math.abs(b[1] - cx), math.abs(b[3] - cz))
        if ra ~= rb then return ra < rb end
        return a.order < b.order
    end)
    for _, b in ipairs(list) do b.order = nil end
    return list, unseen
end

-- Everything but the levelling, which depends on the terrain
local function structures(cx, y, cz, item, paving, creative)
    local stages = {}
    local function stage(name, fill)
        -- Towers and walls share their corner blocks: each spot once
        local all, list, seen = {}, {}, {}
        fill(all)
        for _, b in ipairs(all) do
            local key = b[1] .. "," .. b[2] .. "," .. b[3]
            if not seen[key] then seen[key] = true; list[#list + 1] = b end
        end
        stages[#stages + 1] = { name = name, blocks = list }
    end
    stage("floor", function(l)
        floor(l, cx, y, cz, paving)
        for _, p in ipairs(houses(cx, cz)) do house_floor(l, p[1], y, p[2], creative and "stone_bricks" or paving) end
    end)
    stage("vault", function(l) vault(l, cx, y, cz, item) end)
    -- Once the vault stands, its chests are declared: from then on the bots sort what they store by category
    stages[#stages].after = function()
        for _, c in ipairs(vault_chests(cx, y, cz)) do
            pcall(bot.call, "vault", { category = c[1], x = c[2], y = c[3], z = c[4] })
        end
    end
    stage("keep", function(l) keep(l, cx, y, cz, item, creative) end)
    if creative then
        -- (On a thread of its own: the bot walks over first)
        stages[#stages].after = function()
            if bot.spawn then bot.spawn("keep-entities", function() pcall(summon_keep_entities) end) else pcall(summon_keep_entities) end
        end
    end
    stage("first wall and towers", function(l) walled(l, cx, cz, R1, y, y + 3, item) end)
    stage("village", function(l) for _, p in ipairs(houses(cx, cz)) do house(l, p[1], y, p[2], item, p[3], creative) end end)
    stage("paths and fountain", function(l) paths(l, cx, y, cz, item); fountain(l, cx, y, cz, item) end)
    stage("second wall and towers", function(l) walled(l, cx, cz, R2, y, y + 3, item) end)
    stage("farm plots", function(l) for _, p in ipairs(plots(cx, cz)) do plot(l, p[1], y, p[2]) end end)
    if creative then
        stage("planted farms", function(l)
            for _, p in ipairs(plots(cx, cz)) do
                for dx = -4, 4 do for dz = -4, 4 do
                    if dx ~= 0 or dz ~= 0 then
                        add(l, p[1] + dx, y - 1, p[2] + dz, "farmland")
                        add(l, p[1] + dx, y, p[2] + dz, "wheat")
                    end
                end end
            end
        end)
    end
    stage("pens", function(l) for _, p in ipairs(pens(cx, cz)) do pen(l, p, y, "spruce_fence") end end)
    -- Once they stand, the bots go and fetch animals for them (see ranch below)
    stages[#stages].after = function()
        ranch_due = 0
        if creative and bot.spawn then
            bot.spawn("creative-pens", function() pcall(bot.call, "castle", {action = "ranch", creative = true}) end)
        end
    end
    stage("third wall and towers", function(l) walled(l, cx, cz, R3, y, y + 2, item) end)
    stage("gatehouse", function(l) gatehouse(l, cx, y, cz, item) end)
    -- Last: it is all digging, and the bots can still walk in and out everywhere while they build
    stage("moat", function(l) moat(l, cx, y, cz) end)
    stage("bridge", function(l) bridge(l, cx, y, cz) end)
    -- Then the water: each bot makes itself a bucket (3 iron ingots), the first two fill the well from
    -- whatever water is around, and from then on everyone refills at the well
    stage("moat well", function(l) well(l, cx, y, cz) end)
    stage("moat water", function(l) moat_water(l, cx, y, cz) end)
    return stages
end

-- Height of the ground at x z: the highest block that is not air, a plant or a tree. nil if out of sight
local function ground_height(x, z, from, to)
    for y = from, to, -1 do
        local b = block_at(x, y, z)
        if b == nil then return nil end
        if b ~= "minecraft:air" and not b:match("leaves$") and not b:match("_log$") and not b:match("snow$")
            and not b:match("grass$") and not b:match("fern$") and not b:match("flower") then
            return y, b
        end
    end
end

-- Look for flat ground around the bot: square sites of half-size `half`, the flattest first
local function survey(half, around_x, around_z, avoid_x, avoid_z)
    local sites = {}
    local me = bot.call("status").position
    local top = math.floor(me.y) + 40
    for cx = math.floor(around_x) - 96, math.floor(around_x) + 96, 16 do
        for cz = math.floor(around_z) - 96, math.floor(around_z) + 96, 16 do
            -- Not on top of the world spawn: only operators can build there
            if math.max(math.abs(cx - avoid_x), math.abs(cz - avoid_z)) > half + 24 then
                local heights, wet, unseen = {}, 0, 0
                local step = math.max(2, half // 4)
                for x = cx - half, cx + half, step do
                    for z = cz - half, cz + half, step do
                        local h, b = ground_height(x, z, top, top - 90)
                        if h == nil then unseen = unseen + 1
                        else
                            heights[#heights + 1] = h
                            -- A frozen lake is flat, and no ground to build on
                            if b == "minecraft:water" or b == "minecraft:lava" or b:match("ice$") then wet = wet + 1 end
                        end
                    end
                end
                if unseen == 0 and #heights > 0 then
                    table.sort(heights)
                    sites[#sites + 1] = { x = cx, z = cz, y = heights[(#heights + 1) // 2] + 1,
                        rough = heights[#heights] - heights[1], water = wet }
                end
            end
        end
    end
    table.sort(sites, function(a, b) return a.rough + 3 * a.water < b.rough + 3 * b.water end)
    local best = {}
    for i = 1, math.min(5, #sites) do best[i] = sites[i] end
    return best
end

-- Lighting: monsters spawn where no light reaches. A torch every 8 blocks on the ground, over the whole
-- site and a margin of 12 blocks around it, leaves no dark spot (a torch lights 13 blocks around)
local MARGIN = 12

local function lighting(cx, cz, reach, around_y)
    local list = {}
    for x = cx - reach - MARGIN, cx + reach + MARGIN, 8 do
        for z = cz - reach - MARGIN, cz + reach + MARGIN, 8 do
            if math.max(math.abs(x - cx), math.abs(z - cz)) <= KEEP then
                -- (The keep: lanterns in its rooms)
            elseif math.max(math.abs(x - cx), math.abs(z - cz)) <= reach then
                -- Inside the site: on the levelled floor
                add(list, x, around_y, z, "torch")
            else
                -- In the margin: on the ground as it is there, it is not levelled
                local h = ground_height(x, z, around_y + 30, around_y - 30)
                if h then add(list, x, h + 1, z, "torch") end
            end
        end
    end
    return list
end

-- A road from one point to another: 3 wide, laid in the ground itself (the top block replaced), following
-- the lie of the land, with 3 blocks cleared above it and a torch beside it every 8 blocks.
-- Over water, and over a ravine (a drop of 3 or more that comes back up within 32 blocks), it becomes a
-- bridge: a deck at the level of the bank it leaves, with a parapet 1 high on each side.
-- Returns the blocks, how many steps of the way were out of sight (the road is only planned where a bot
-- can see the ground), and how many steps are bridge
local function road(x1, z1, x2, z2, around_y, item, span)
    span = span or 40              -- how far above and below around_y the ground is looked for
    local list, unseen, bridged, seen = {}, 0, 0, {}
    local steps = math.max(math.abs(x2 - x1), math.abs(z2 - z1))
    local wide_x = math.abs(z2 - z1) >= math.abs(x2 - x1)   -- going mostly north-south: widen east-west
    -- The way, point by point: where, how high the ground is, and whether it is water
    local way = {}
    for i = 0, steps do
        local x = math.floor(x1 + (x2 - x1) * i / math.max(steps, 1) + 0.5)
        local z = math.floor(z1 + (z2 - z1) * i / math.max(steps, 1) + 0.5)
        local h, b = ground_height(x, z, around_y + span, around_y - span)
        way[#way + 1] = { x = x, z = z, h = h, wet = b == "minecraft:water" or b == "minecraft:lava" }
    end
    local function across(p, w) return wide_x and p.x + w or p.x, wide_x and p.z or p.z + w end
    local function once(x, y, z, what)
        local key = x .. "," .. y .. "," .. z
        if not seen[key] then seen[key] = true; add(list, x, y, z, what) end
    end
    -- Is the ground at point i the near edge of a ravine? It is if the far side is in sight within 32 steps
    local function far_side(i, level)
        for j = i + 1, math.min(#way, i + 32) do
            local p = way[j]
            if p.h and not p.wet and p.h >= level - 1 then return true end
        end
        return false
    end
    local deck, last = nil, nil      -- deck: level of the bridge under way; last: height of the last ground walked
    for i, p in ipairs(way) do
        if p.h == nil then
            unseen = unseen + 1
            deck = nil
        else
            if deck == nil and last and (p.wet or (p.h <= last - 3 and far_side(i, last))) then
                deck = last
            elseif deck and not p.wet and p.h >= deck - 1 then
                deck = nil
            end
            if deck then
                -- Bridge: the deck, a parapet on each side, headroom above
                bridged = bridged + 1
                for w = -2, 2 do
                    local bx, bz = across(p, w)
                    once(bx, deck, bz, item)
                    if math.abs(w) == 2 then
                        once(bx, deck + 1, bz, item)
                    else
                        for up = 1, 3 do once(bx, deck + up, bz, AIR) end
                    end
                end
                if i % 8 == 0 then
                    local tx, tz = across(p, 2)
                    once(tx, deck + 2, tz, "torch")
                end
            else
                -- Road on the ground. On water with no bank behind it yet (the very start), a causeway
                last = p.h
                for w = -1, 1 do
                    local rx, rz = across(p, w)
                    local rh = ground_height(rx, rz, around_y + span, around_y - span) or p.h
                    -- The sides at the level of the middle, give or take a step
                    if math.abs(rh - p.h) > 1 then rh = p.h end
                    once(rx, rh, rz, item)
                    for up = 1, 3 do once(rx, rh + up, rz, AIR) end
                end
                if i % 8 == 0 then
                    local tx, tz = across(p, 2)
                    local th = ground_height(tx, tz, around_y + span, around_y - span)
                    if th then once(tx, th + 1, tz, "torch") end
                end
            end
        end
    end
    return list, unseen, bridged
end

-- A road with a railway beside it, one straight piece along x or z (a road is two legs: along one axis, then
-- the other; rails only join side by side). It keeps a steady grade, a block up or down every 4 steps at most:
-- through a mountain it tunnels, over a valley it runs on a deck with a parapet. Across it: the road 3 wide, the
-- rail beside it on one side, a torch every 8 steps on the other. The rail sits on the inside of the turn, so the
-- two legs' rails meet at a curve. A powered rail on a redstone block on every slope and every 24 blocks.
-- c: the road so far (px pz: where this piece starts, y: the road's level, leg, kx kz: the turn, x z: the end,
-- since: steps since the level last changed). Returns the blocks, the end reached (the piece stops where the
-- ground is out of sight) and the level there; nil if nothing of it is in sight
local RAIL_GRADE, RAIL_POWER = 4, 24
local function rail_road(c, ex, ez, item, span)
    local list, keys = {}, {}
    local function put(x, y, z, what, prio)
        local k = x .. "," .. y .. "," .. z
        local o = keys[k]
        if o and o.prio >= prio then return end
        if o then o.b[4], o.prio = what, prio
        else local b = { x, y, z, what }; list[#list + 1] = b; keys[k] = { b = b, prio = prio } end
    end
    local along_x = ex ~= c.px
    local d = along_x and (ex > c.px and 1 or -1) or (ez > c.pz and 1 or -1)
    local n = along_x and math.abs(ex - c.px) or math.abs(ez - c.pz)
    -- The turn: which way each leg goes, and the side of the rail (the inside of the turn)
    local d1 = c.kx ~= c.sx and (c.kx > c.sx and 1 or -1) or (c.kz > c.sz and 1 or -1)
    local d2 = (c.x ~= c.kx or c.z ~= c.kz) and ((c.x ~= c.kx) and (c.x > c.kx and 1 or -1) or (c.z > c.kz and 1 or -1)) or 1
    local side = c.leg == 1 and d2 or -d1
    local function at(i) return along_x and c.px + d * i or c.px, along_x and c.pz or c.pz + d * i end
    local function across(x, z, w) return along_x and x or x + w * side, along_x and z + w * side or z end
    local function axis(x, z) return along_x and x or z end
    local K = axis(c.kx, c.kz)
    -- The ground along the way, the piece cut short where it is out of sight
    local h = {}
    for i = 0, n do
        local x, z = at(i)
        local g = ground_height(x, z, c.y + span, c.y - span)
        if g == nil then
            if block_at(x, c.y, z) == nil then n = i - 1 break end
            g = c.y - span     -- loaded, nothing within span below: deep
        end
        h[i] = g
    end
    if n < 0 then return nil end
    -- The level, step by step: toward the ground ahead (the middle height of the next 16 steps), one block per
    -- RAIL_GRADE steps at most, and flat within 5 of the turn (a curved rail can't slope)
    local level, L, since = {}, (c.first and not c.joined) and h[0] or c.y, c.since or RAIL_GRADE
    for i = 0, n do
        local ahead = {}
        for j = i, math.min(n, i + 16) do ahead[#ahead + 1] = h[j] end
        table.sort(ahead)
        local target = ahead[(#ahead + 1) // 2]
        local x, z = at(i)
        local near_turn = c.has_turn and math.abs(axis(x, z) - K) < 5
        if i > 0 and since >= RAIL_GRADE and not near_turn and target ~= L then
            L = L + (target > L and 1 or -1)
            since = 0
        end
        -- (A piece's first step is the last one of the piece before, already counted)
        if i > 0 or c.first then since = since + 1 end
        level[i] = L
    end
    local names = along_x and { [1] = "east", [-1] = "west" } or { [1] = "south", [-1] = "north" }
    for i = 0, n do
        local x, z = at(i)
        local Li = level[i]
        local drop = h[i] < Li - 2
        for w = -2, 2 do
            local bx, bz = across(x, z, w)
            put(bx, Li, bz, item, 2)
            for up = 1, 3 do put(bx, Li + up, bz, AIR, 1) end
        end
        if drop then
            for _, w in ipairs({ -2, 3 }) do
                local bx, bz = across(x, z, w)
                put(bx, Li, bz, item, 2)
                put(bx, Li + 1, bz, item, 3)
            end
        end
        if axis(x, z) % 8 == 0 then
            local tx, tz = across(x, z, -2)
            put(tx, Li + (drop and 2 or 1), tz, "torch", 4)
        end
        -- The rail, but not where the other leg's rail runs: leg 1 stops 2 before the turn, where leg 2's curve is
        local a = axis(x, z)
        local rx, rz = across(x, z, 2)
        local rail
        if c.leg == 1 and c.has_turn and (a - K) * d > -3 then rail = nil
        elseif c.leg == 2 and (a - K) * d < 2 then rail = nil
        elseif c.leg == 2 and (a - K) * d == 2 then
            -- The curve: back toward leg 1, on along leg 2
            local xs = along_x and (d > 0 and "east" or "west") or (d1 > 0 and "west" or "east")
            local zs = along_x and (d1 > 0 and "north" or "south") or (d > 0 and "south" or "north")
            rail = "rail[shape=" .. zs .. "_" .. xs .. "]"
        else
            -- Up toward the higher neighbour, if one is
            local nxt, prv = level[i + 1], level[i - 1] or (c.prev_level or Li)
            local shape = along_x and "east_west" or "north_south"
            if nxt == Li + 1 then shape = "ascending_" .. names[d]
            elseif prv == Li + 1 then shape = "ascending_" .. names[-d] end
            if shape:match("^ascending") or a % RAIL_POWER == 0 then
                rail = "powered_rail[shape=" .. shape .. "]"
                put(rx, Li, rz, "redstone_block", 3)
            else
                rail = "rail[shape=" .. shape .. "]"
            end
        end
        if rail then put(rx, Li + 1, rz, rail, 4) end
        -- A milestone every 200 blocks of road: a sign beside the road and one beside the railway, how far back to
        -- the castle along the road and how far on to the place (c.d0: the road's length from the castle at its start)
        local from = (c.d0 or 0) + math.abs(x - c.sx) + math.abs(z - c.sz)
        if from > 0 and from % 200 == 0 then
            local left = math.abs(c.x - x) + math.abs(c.z - z)
            local text = (c.origin_name or "Castle") .. ": " .. from .. "|-> " .. c.name .. "|" .. left .. " blocks|"
            local sx1, sz1 = across(x, z, -2)
            put(sx1, Li + (drop and 2 or 1), sz1, (c.sign or "spruce_sign") .. "|" .. text, 5)
            local sx2, sz2 = across(x, z, 3)
            put(sx2, Li, sz2, item, 3)
            put(sx2, Li + 2, sz2, AIR, 3)
            put(sx2, Li + 1, sz2, (c.sign or "spruce_sign") .. "|" .. text, 5)
        end
    end
    c.since, c.prev_level = since, level[n - 1] or level[n]
    local endx, endz = at(n)
    -- What was laid, for a branch to leave it later: the axis, the road's middle line, the rail's line, the level
    -- at each step (by its coordinate along the axis)
    local laid = { ax = along_x, line = along_x and c.pz or c.px, rail = along_x and c.pz + 2 * side or c.px + 2 * side, lv = {} }
    for i = 0, n do local x, z = at(i); laid.lv[axis(x, z)] = level[i] end
    return list, endx, endz, level[n], level[0], laid
end

-- The castle's site is kept in a file: the plugin's own memory does not survive a restart of the program
local SITE_FILE = "../castle_site.txt"

local function save_site()
    local f = io.open(SITE_FILE, "w")
    if f and job_site then
        f:write(job_site.x, " ", job_site.y, " ", job_site.z, " ", job_site.floors or 1, "\n")
        f:close()
    end
end

local function load_site()
    local f = io.open(SITE_FILE, "r")
    if not f then return nil end
    local x, y, z, floors = f:read("n", "n", "n", "n")
    f:close()
    if x and y and z then return { x = x, y = y, z = z, floors = floors or 1, plots = plots(x, z) } end
end

-- A miniature that goes through every kind of work in minutes: levelling, paved floor, a piece of the
-- 5-wide wall, a house and one farm plot
local function miniature(cx, y, cz, item, paving)
    local stages = {}
    local function stage(name, fill) local list = {}; fill(list); stages[#stages + 1] = { name = "test " .. name, blocks = list } end
    stage("floor", function(l) for x = cx - 1, cx + 1 do for z = cz - 1, cz + 1 do add(l, x, y - 1, z, paving) end end end)
    stage("wall", function(l)
        for x = cx - 3, cx + 3 do
            for i = 0, THICK - 1 do
                local side = i == 0 or i == THICK - 1
                for yy = y, y + (side and 3 or 2) do add(l, x, yy, cz - 8 + i, item) end
            end
        end
    end)
    stage("house", function(l) house(l, cx - 7, y, cz + 2, item) end)
    stage("farm plot", function(l) plot(l, cx + 7, y, cz + 4) end)
    -- Last, as it is the longest: the chests need planks, so trees, and a crafting table
    stage("vault", function(l) vault(l, cx, y, cz, item) end)
    stages[#stages].after = function()
        for _, c in ipairs(vault_chests(cx, y, cz)) do
            pcall(bot.call, "vault", { category = c[1], x = c[2], y = c[3], z = c[4] })
        end
    end
    return stages
end

-- Stages bigger than one hand-over are cut in parts
local function chunked(stages)
    local out = {}
    for _, s in ipairs(stages) do
        local parts = math.max(1, math.ceil(#s.blocks / CHUNK))
        for i = 1, parts do
            local part = {}
            for k = (i - 1) * CHUNK + 1, math.min(i * CHUNK, #s.blocks) do part[#part + 1] = s.blocks[k] end
            out[#out + 1] = { name = parts > 1 and (s.name .. " " .. i .. "/" .. parts) or s.name, blocks = part,
                after = i == parts and s.after or nil }
        end
    end
    return out
end

-- A job dropped before anything stood (still levelling or lighting) leaves no castle to protect: its box is
-- forgotten, or no bot may ever dig there again (the team moved on 8 times and bots ended up walled in underground)
local function forget_unbuilt_site(old)
    if not old or not old.site or not old.stages then return end
    local stage = old.stages[old.index]
    if stage and not (stage.name:match("^levelling") or stage.name:match("^lighting")) then return end
    local b = old.site
    local x1, y1, z1 = math.min(b[1], b[4]), math.min(b[2], b[5]), math.min(b[3], b[6])
    local x2, y2, z2 = math.max(b[1], b[4]), math.max(b[2], b[5]), math.max(b[3], b[6])
    local ok, list = pcall(bot.call, "sites", {})
    if not ok or type(list) ~= "table" then return end
    for i = #list, 1, -1 do       -- from the end: removing one shifts the ones after it
        local e = list[i]
        if e.name == "castle" and e.x1 == x1 and e.y1 == y1 and e.z1 == z1 and e.x2 == x2 and e.y2 == y2 and e.z2 == z2 then
            pcall(bot.call, "sites", { remove = i })
        end
    end
end

-- The blocks of a stage in n parts for n bots: sorted by x then z and cut in runs of (almost) the same size,
-- so each part is a strip of the site. Each part: { blocks, x1, x2, z1, z2 }
local function split_parts(blocks, n)
    local sorted = {}
    for i, b in ipairs(blocks) do sorted[i] = b end
    table.sort(sorted, function(a, b) if a[1] ~= b[1] then return a[1] < b[1] end if a[3] ~= b[3] then return a[3] < b[3] end return a[2] < b[2] end)
    local parts, size = {}, math.ceil(#sorted / math.max(n, 1))
    for i = 1, n do
        local p = { blocks = {} }
        for k = (i - 1) * size + 1, math.min(i * size, #sorted) do
            local b = sorted[k]
            p.blocks[#p.blocks + 1] = b
            p.x1, p.x2 = math.min(p.x1 or b[1], b[1]), math.max(p.x2 or b[1], b[1])
            p.z1, p.z2 = math.min(p.z1 or b[3], b[3]), math.max(p.z2 or b[3], b[3])
        end
        p.x1, p.x2, p.z1, p.z2 = p.x1 or 0, p.x2 or 0, p.z1 or 0, p.z2 or 0
        parts[i] = p
    end
    return parts
end

-- The blocks of a stage in n strips of the same width across x1..x2 (blocks outside go to the nearest end):
-- the same cut on every host for the same site, whatever blocks each one planned
local function strip_parts(blocks, n, x1, x2)
    local parts, width = {}, (x2 - x1 + 1) / n
    for i = 1, n do
        parts[i] = { blocks = {}, x1 = math.floor(x1 + (i - 1) * width), x2 = math.floor(x1 + i * width) - 1, z1 = math.huge, z2 = -math.huge }
    end
    for _, b in ipairs(blocks) do
        local i = math.max(1, math.min(n, math.floor((b[1] - x1) / width) + 1))
        local p = parts[i]
        p.blocks[#p.blocks + 1] = b
        p.z1, p.z2 = math.min(p.z1, b[3]), math.max(p.z2, b[3])
    end
    for _, p in ipairs(parts) do if #p.blocks == 0 then p.z1, p.z2 = 0, 0 end end
    return parts
end

local function hand_over()
    CREATIVE_CONSTRUCTION = job.creative or false
    local s = job.stages[job.index]
    -- With the site as a box: the bots take their stone, dirt and wood outside it, not out of what is built
    if job.creative then
        local members = {}
        for _, b in ipairs(bot.call("bots")) do
            local ok, st = pcall(bot.call, "status", { bot = b.name })
            if ok and st.game_mode == "creative" and st.dimension == "minecraft:overworld" and not (roadman and roadman.bot == b.name) then members[#members + 1] = b.name end
        end
        -- One part per bot, never two bots on one. Alone: strips across the site (by x, then z) with as many blocks
        -- each. With bots of other hosts on the same site (creative plugin): strips of the site of the same width,
        -- one per bot of every host in the same order everywhere, so the hosts cut it alike though each plans the
        -- stage from its own view of the ground; this host's bots take theirs
        local crew = TEAM_CREW and TEAM_CREW(members) or {}
        local parts, index, total = nil, {}, #members
        if #crew > #members then
            parts, total = strip_parts(s.blocks, #crew, job.site[1], job.site[4]), #crew
            local id = NETLINK_BUILD and NETLINK_BUILD.id or "local"
            for i, key in ipairs(crew) do
                local host, name = key:match("^(.-):(.*)$")
                if host == id then index[name] = i end
            end
        else
            parts = split_parts(s.blocks, #members)
            for i, name in ipairs(members) do index[name] = i end
        end
        job.parts = {}
        for _, name in ipairs(members) do
            local i = index[name]
            local p = i and parts[i] or { blocks = {}, x1 = 0, x2 = 0, z1 = 0, z2 = 0 }
            job.parts[name] = { index = i, blocks = #p.blocks, done = #p.blocks == 0 }
            if #p.blocks > 0 then
                bot.call("team_member", { bot = name, objective = "build", plan = "blocks", blocks = p.blocks,
                    site = job.site, name = "castle", members = { name }, leader = name, solo = true })
                bot.call("autopilot", { bot = name, enabled = true })
                pcall(bot.call, "chat", { bot = name, message = string.format("[castle] %s takes part %d/%d of %s: x %d..%d z %d..%d, %d blocks",
                    name, i, total, s.name, p.x1, p.x2, p.z1, p.z2, #p.blocks) })
            end
        end
    else
        bot.call("team", { objective = "build", plan = "blocks", blocks = s.blocks, site = job.site, name = s.name:match("^road") and "road" or "castle" })
    end
    pcall(bot.call, "chat", { message = "[castle] " .. s.name .. " (" .. job.index .. "/" .. #job.stages .. "): " .. #s.blocks .. " blocks" })
end

-- How the stage under way stands. Each bot reports how many spots of the plan are not standing yet, as of its
-- last look. Only the bots that looked in the last minute count: one kept busy by something more urgent would
-- report an old number. Returns: what each bot says, the lowest count among those working, how many are working
local function progress()
    local left, lowest, working = {}, nil, 0
    for _, b in ipairs(bot.call("bots")) do
        local ok, st = pcall(bot.call, "status", { bot = b.name })
        -- (Creative: only the bots on the site; one far away sees a cell or two of it and would report 1 left)
        local here = not job.creative or (st and st.position and job.site and st.position.x >= job.site[1] and st.position.x <= job.site[4]
            and st.position.z >= job.site[3] and st.position.z <= job.site[6])
        if ok and type(st) == "table" and st.build_left ~= nil and here and (not job.creative or (st.game_mode == "creative" and st.dimension == "minecraft:overworld")) then
            local active = st.build_idle ~= nil and st.build_idle < 60
            left[b.name] = active and st.build_left or "away"
            if active then
                working = working + 1
                if lowest == nil or st.build_left < lowest then lowest = st.build_left end
            end
        end
    end
    if job.creative and job.parts then
        -- Each bot reports its own part: the stage stands when every part does (a part seen done stays done; one
        -- whose bot is away is not done). What is left is the most any part has
        local most, all_done = 0, true
        for name, part in pairs(job.parts) do
            if type(left[name]) == "number" and left[name] == 0 then part.done = true end
            if not part.done then
                all_done = false
                if type(left[name]) == "number" and left[name] > most then most = left[name] end
            end
        end
        lowest = all_done and 0 or math.max(most, 1)
    end
    return left, lowest, working
end

-- A stage is over when a bot sees all of it standing. It is also over when nothing has moved for five
-- minutes: what is left is beyond the bots (spots they can't reach, a material they can't get), and the
-- rest of the castle should not wait for it. And when nobody at all has been on it for three minutes
local function stage_over()
    local _, lowest, working = progress()
    local now = os.time()
    job.since = job.since or now
    if working == 0 then
        job.idle_since = job.idle_since or now
        return now - job.idle_since > 180, "nobody on it"
    end
    job.idle_since = nil
    if lowest == 0 then return true, "done" end
    if job.best == nil or lowest < job.best then
        job.best, job.best_at = lowest, now
    end
    -- (Creative: a stage takes a minute, so its last stragglers get 60 seconds, not 5 minutes)
    return now - job.best_at > ((job.creative and lowest < 100) and 60 or 300), lowest .. " spots left that the bots could not do"
end

-- Stock the pens: for each kind of animal the bot has the food of, open the gate, lead the animals within
-- 48 blocks inside, breed them if two or more came, then leave without the food in hand (so they stay) and
-- shut the gate behind. A pen with a fence around it is also what keeps its animals from being hunted
local function ranch(args)
    job_site = job_site or load_site()
    if not job_site then error("no castle site yet: run castle start first") end
    local cx, y, cz = job_site.x, job_site.y, job_site.z
    local function try(tool, a) a.bot = args.bot; return pcall(bot.call, tool, a) end
    local function gate(p, open)
        -- A click toggles it: only when it is not already as wanted
        local ok, b = try("get_block", { x = p.gate_x, y = y, z = p.gate_z })
        if ok and b.state and b.state.open ~= nil and (b.state.open == "true") ~= open then
            try("interact_block", { x = p.gate_x, y = y, z = p.gate_z })
        end
    end
    local builder = args.bot
    if not builder then
        for _, b in ipairs(bot.call("bots")) do
            local ok, st = pcall(bot.call, "status", {bot = b.name})
            if ok and st.game_mode == "creative" and st.dimension == "minecraft:overworld" then builder = b.name; break end
        end
    end
    local okst, st = pcall(bot.call, "status", {bot = builder})
    if okst and st.game_mode == "creative" then
        args.bot = builder
        local report, stocked = {}, 0
        for _, p in ipairs(pens(cx, cz)) do
            local function animals()
                local count = 0
                local ok, list = try("entities", {type = p.type})
                for _, animal in ipairs(ok and list or {}) do
                    local at = animal.position
                    if at and math.abs(at.x - p.x) < 4 and math.abs(at.z - p.z) < 4 and math.abs(at.y - y) < 3 then count = count + 1 end
                end
                return count
            end
            gate(p, false)
            local okgate, entrance = try("get_block", {x = p.gate_x, y = y, z = p.gate_z})
            local okfloor, ground = try("get_block", {x = p.x, y = y - 1, z = p.z})
            local before = animals()
            local r = {pen = p.type, at = p.x .. " " .. p.z, spawned = 0}
            if okgate and entrance.block and entrance.block:match("_fence_gate$") and entrance.state and entrance.state.open == "false"
                and okfloor and ground.block and ground.block ~= "minecraft:air" and ground.block ~= "minecraft:water" then
                for _ = before + 1, 2 do
                    local ok = try("use_item", {item = p.type .. "_spawn_egg", x = p.x, y = y - 1, z = p.z})
                    if not ok then break end
                end
                local count = animals()
                r.spawned = math.max(0, count - before)
                if count >= 2 then stocked = stocked + 1 end
                r.note = count >= 2 and "stocked with spawn eggs" or "waiting for animals; will retry"
            else
                r.note = "waiting for a closed pen and solid floor"
            end
            report[#report + 1] = r
        end
        return {pens = report, stocked = stocked}
    end
    local report, stocked = {}, 0
    for _, p in ipairs(pens(cx, cz)) do
        local have, other = 0, nil
        for _, slot in ipairs(bot.call("inventory", { bot = args.bot })) do
            if slot.item == p.food then have = have + slot.count elseif not other then other = slot.item end
        end
        local r = { pen = p.type, at = p.x .. " " .. p.z }
        if have < 3 then
            r.note = "needs 3 " .. p.food:gsub("minecraft:", "") .. " (1 to lead them, 2 to breed)"
        else
            gate(p, true)
            local ok, res = try("lure", { type = p.type, x = p.x, y = y, z = p.z })
            if ok then
                r.arrived = res.arrived
                if res.arrived >= 2 then
                    r.bred = try("breed", { type = p.type, food = p.food, radius = 6 })
                    stocked = stocked + 1
                end
            else
                r.note = tostring(res)
            end
            -- Out again with something else in hand, and the gate shut
            if other then try("equip", { item = other }) end
            try("goto", { x = p.out_x, y = y, z = p.gate_z, tolerance = 1 })
            gate(p, false)
        end
        report[#report + 1] = r
    end
    return { pens = report, stocked = stocked }
end

-- When a bot finds a category's chests full on every floor, the vault gets another floor underneath:
-- the same room, 5 blocks lower, with its own chests. As a stage of its own, next in line
local vault_growing = false

local function grow_vault()
    if vault_growing then return end
    local ok, v = pcall(bot.call, "vault")
    if not ok or type(v) ~= "table" or not v.full then return end
    job_site = job_site or load_site()
    if not job_site then return end
    local k = job_site.floors or 1          -- floors so far: the new one is number k (counting from 0)
    local cx, y, cz = job_site.x, job_site.y, job_site.z
    local list = {}
    vault(list, cx, y, cz, "stone", k)
    local stage = { name = "vault floor " .. (k + 1), blocks = list, after = function()
        for _, c in ipairs(vault_chests(cx, y, cz, k)) do
            pcall(bot.call, "vault", { category = c[1], x = c[2], y = c[3], z = c[4] })
        end
        pcall(bot.call, "vault", { full = false })
        job_site.floors = k + 1
        save_site()
        vault_growing = false
    end }
    vault_growing = true
    if job then
        table.insert(job.stages, job.index + 1, stage)
    else
        job = { stages = { stage }, index = 1 }
        hand_over()
    end
    pcall(bot.call, "chat", { message = "[castle] the vault is full: digging floor " .. (k + 1) })
end

-- The road builder: one bot, out of the team, whose whole job is roads. From the origin to each of the team's
-- mines and to each place it was given, one after the other, 32 blocks at a time (the ground has to be in its
-- sight to be planned), with a signpost at each end saying where the road leads. Mines opened later get theirs
roadman = restore(ROADS_FILE)     -- the road builder, as it was before a restart (nil: none)
local function keep(r)
    save(ROADS_FILE, r and { bot = r.bot, origin = r.origin, places = r.places, done = r.done, laid = r.laid, patience = 300, item = r.item,
        sign = r.sign, explore = r.explore, reach = r.reach, rail = r.rail, castle = r.castle, net = r.net })
end
-- What an explorer looks for (structure ids or #tags for /locate), when not told
local STRUCTURES = "#minecraft:village,pillager_outpost,ruined_portal,desert_pyramid,jungle_pyramid,igloo,swamp_hut,mineshaft,trail_ruins,trial_chambers"

-- An explorer with no road left to lay: the nearest structure of each kind, seen from where it stands (the end
-- of its last road), within reach of the origin and not known yet. True when it found one
local function explore(r)
    local found = false
    for name in r.explore:gmatch("[%w_:#]+") do
        local ok, at = pcall(bot.call, "locate", { bot = r.bot, kind = "structure", name = name })
        if ok and type(at) == "table" and at.x and math.max(math.abs(at.x - r.origin.x), math.abs(at.z - r.origin.z)) <= r.reach then
            local known = false
            for _, p in ipairs(r.places) do
                if math.abs(p.x - at.x) < 48 and math.abs(p.z - at.z) < 48 then known = true end
            end
            if not known then
                local label = name:gsub("^#?minecraft:", ""):gsub("_", " ")
                r.places[#r.places + 1] = { name = label:sub(1, 1):upper() .. label:sub(2), what = "found by " .. r.bot, x = at.x, z = at.z }
                pcall(bot.call, "chat", { message = "[explore] " .. label .. " at " .. at.x .. " " .. at.z .. ": laying a road to it" })
                found = true
            end
        end
    end
    keep(r)
    return found
end
-- The next road of the network: none laid yet, the main road, to the farthest place (from the castle, from 4
-- blocks beyond the moat on the side it lies). Then the place nearest to the roads laid, by a branch from the
-- nearest point of them: it leaves the road at a right angle, 3 blocks from its middle, at the road's level, and
-- the railway of that road is cut there for 5 blocks (it ends and starts again; the branch has its own)
local function rail_route(r, places)
    r.net = r.net or {}
    local best, best_d, best_j
    for _, p in ipairs(places) do
        if not r.done[p.x .. " " .. p.z] then
            if #r.net == 0 then
                local d = (p.x - r.origin.x) ^ 2 + (p.z - r.origin.z) ^ 2
                if not best or d > best_d then best, best_d = p, d end
            else
                for _, seg in ipairs(r.net) do
                    local lo, hi
                    for a in pairs(seg.lv) do lo, hi = math.min(lo or a, a), math.max(hi or a, a) end
                    if lo then
                        local pa, pp = seg.ax and p.x or p.z, seg.ax and p.z or p.x
                        local ja = math.max(lo, math.min(hi, pa))
                        local d = (pa - ja) ^ 2 + (pp - seg.line) ^ 2
                        if not best or d < best_d then best, best_d, best_j = p, d, { seg = seg, a = ja, lo = lo, hi = hi } end
                    end
                end
            end
        end
    end
    if not best then return nil end
    local p = best
    local c = { name = p.name, what = p.what or "", x = p.x, z = p.z, y = r.origin.y, first = true }
    local sx, sz, first_x
    if not best_j then
        sx, sz = r.origin.x, r.origin.z
        local dx, dz = p.x - sx, p.z - sz
        first_x = math.abs(dx) >= math.abs(dz)
        if r.castle then
            local out = OUTER + 4
            if first_x then sx = sx + (dx >= 0 and out or -out) else sz = sz + (dz >= 0 and out or -out) end
        end
    else
        local seg, ja = best_j.seg, best_j.a
        local pa, pp = seg.ax and p.x or p.z, seg.ax and p.z or p.x
        local a, perp
        if math.abs(pp - seg.line) > 3 then
            -- Off to the side: away at a right angle
            a, perp = ja, seg.line + (pp > seg.line and 3 or -3)
            first_x = not seg.ax
        else
            -- On past the road's end: on along it
            local dir = (ja == best_j.hi) and 1 or -1
            a, perp = ja + 3 * dir, seg.line
            first_x = seg.ax
        end
        if seg.ax then sx, sz = a, perp else sx, sz = perp, a end
        c.y, c.joined = seg.lv[ja], true
        c.d0 = (seg.d0 or 0) + math.abs(ja - (seg.a0 or ja)) + 3
        -- The cut in that road's railway (rails at its level + 1)
        c.cut = {}
        for da = -2, 2 do
            local l = seg.lv[ja + da]
            if l then
                local rx, rz = seg.ax and ja + da or seg.rail, seg.ax and seg.rail or ja + da
                c.cut[#c.cut + 1] = { rx, l + 1, rz, AIR }
            end
        end
    end
    c.sx, c.sz, c.px, c.pz = sx, sz, sx, sz
    if not best_j then c.d0 = math.abs(sx - r.origin.x) + math.abs(sz - r.origin.z) end
    c.sign, c.origin_name = r.sign, r.origin.name
    c.kx, c.kz = first_x and p.x or sx, first_x and sz or p.z
    c.leg, c.has_turn = 1, not (c.kx == p.x and c.kz == p.z)
    if c.kx == sx and c.kz == sz then c.kx, c.kz, c.has_turn = p.x, p.z, false end
    return c
end

-- One piece of a road with its railway (rail_road), up to 32 blocks along the leg under way
local function rail_tick(r, c, st, now)
    local tx, tz = c.leg == 1 and c.kx or c.x, c.leg == 1 and c.kz or c.z
    if c.px == tx and c.pz == tz then
        if c.leg == 1 and c.has_turn then c.leg = 2; return end
        r.net = r.net or {}
        for _, seg in ipairs(c.segs or {}) do r.net[#r.net + 1] = seg end
        r.done[c.x .. " " .. c.z] = true
        r.laid = r.laid + 1
        keep(r)
        r.cur = nil
        return
    end
    local ex = c.px + math.max(-32, math.min(32, tx - c.px))
    local ez = c.pz + math.max(-32, math.min(32, tz - c.pz))
    local away = math.max(math.abs(st.position.x - c.px), math.abs(st.position.z - c.pz))
    local order = { bot = r.bot, objective = "build", plan = "blocks", members = { r.bot }, leader = r.bot, count = 1, solo = true, name = "road" }
    r.best, r.best_at, r.busy = nil, now, true
    viewer = r.bot
    local first = c.first
    local blocks, endx, endz, y, y0, laid = rail_road(c, ex, ez, r.item, 24)
    c.first_level = first and y0 or nil
    if laid then
        -- One segment per leg, its levels gathered piece by piece
        c.segs = c.segs or {}
        local seg = c.segs[c.leg]
        if not seg then
            -- (where the leg starts along its axis, and the road's length from the castle there: for milestones)
            local lx, lz = c.leg == 1 and c.sx or c.kx, c.leg == 1 and c.sz or c.kz
            seg = { ax = laid.ax, line = laid.line, rail = laid.rail, lv = {}, a0 = laid.ax and lx or lz,
                d0 = (c.d0 or 0) + math.abs(lx - c.sx) + math.abs(lz - c.sz) }
            c.segs[c.leg] = seg
        end
        for a, l in pairs(laid.lv) do seg.lv[a] = l end
    end
    if laid and first and c.cut then
        -- The junction: the railway of the road it leaves ends here and starts again beyond it
        for _, b in ipairs(c.cut) do blocks[#blocks + 1] = b end
    end
    viewer = nil
    if not blocks then
        if away <= 24 then
            -- In sight of the bot and still nothing: skip a block on (void, or the world's edge)
            c.px, c.pz = c.px + (ex ~= c.px and (ex > c.px and 1 or -1) or 0), c.pz + (ez ~= c.pz and (ez > c.pz and 1 or -1) or 0)
            r.busy = false
            return
        end
        -- Out of the bot's sight: a torch to put there takes it there
        order.blocks = { { c.px, c.y + 1, c.pz, "torch" } }
        r.patience, r.note = 120, "going to " .. c.px .. " " .. c.pz
        pcall(bot.call, "team_member", order)
        return
    end
    local far = math.floor(math.sqrt((c.x - r.origin.x) ^ 2 + (c.z - r.origin.z) ^ 2) + 0.5)
    -- A signpost just beyond each end of the road, on a block of its own
    local ux, uz = ex - c.px, ez - c.pz
    ux, uz = ux ~= 0 and (ux > 0 and 1 or -1) or 0, uz ~= 0 and (uz > 0 and 1 or -1) or 0
    local function post(x, z, level, text)
        add(blocks, x, level, z, r.item)
        add(blocks, x, level + 1, z, r.sign .. "|" .. text)
    end
    if c.first then
        post(c.px - ux, c.pz - uz, c.first_level or y, c.name .. "|" .. c.what .. "|at " .. c.x .. " " .. c.z .. "|" .. far .. " blocks")
    end
    if (c.leg == 2 or not c.has_turn) and endx == c.x and endz == c.z then
        post(c.x + ux, c.z + uz, y, r.origin.name .. "|at " .. r.origin.x .. " " .. r.origin.z .. "|" .. far .. " blocks|")
    end
    c.first, c.px, c.pz, c.y = false, endx, endz, y
    order.blocks = blocks
    r.patience = 300
    r.note = "road and railway to " .. c.name .. " at " .. c.x .. " " .. c.z .. " (leg " .. c.leg .. "): at " .. endx .. " " .. endz
    keep(r)
    pcall(bot.call, "team_member", order)
end

local function road_tick()
    local r = roadman
    if not r then return end
    local ok, st = pcall(bot.call, "status", { bot = r.bot })
    if not ok or type(st) ~= "table" or not st.position then return end
    local now = os.time()
    if r.busy then
        -- A piece is being laid: wait for it, 5 minutes at most without a block more
        if st.building and (st.build_left or 0) > 0 then
            if r.best == nil or st.build_left < r.best then r.best, r.best_at = st.build_left, now end
            if now - r.best_at < r.patience then return end
        end
        r.busy = false
    end
    if not r.cur then
        local places = {}
        for _, p in ipairs(r.places) do places[#places + 1] = p end
        local okm, mines = pcall(bot.call, "mines")
        for _, m in ipairs(okm and mines or {}) do
            local what = (m.resources or ""):gsub("_ore", ""):gsub(",", ", ")
            places[#places + 1] = { name = "Mine", what = what ~= "" and what or "ore", x = m.x, z = m.z }
        end
        if r.rail then
            r.cur = rail_route(r, places)
        else
            for _, p in ipairs(places) do
                if not r.done[p.x .. " " .. p.z] then
                    r.cur = { name = p.name, what = p.what or "", x = p.x, z = p.z, px = r.origin.x, pz = r.origin.z, y = r.origin.y, first = true }
                    break
                end
            end
        end
        if not r.cur and r.explore then
            if explore(r) then return end
            -- Nothing new from here (/locate gives the nearest: the one it stands at): 256 blocks on, a different
            -- way each time and never beyond reach of the origin, and a look from there. A torch to put there takes it
            r.heading = (r.heading or 0) % 8 + 1
            local a = r.heading * math.pi / 4
            local tx, tz = st.position.x + 256 * math.cos(a), st.position.z + 256 * math.sin(a)
            local out = math.max(math.abs(tx - r.origin.x), math.abs(tz - r.origin.z)) / r.reach
            if out > 1 then tx, tz = r.origin.x + (tx - r.origin.x) / out, r.origin.z + (tz - r.origin.z) / out end
            tx, tz = math.floor(tx + 0.5), math.floor(tz + 0.5)
            r.best, r.best_at, r.busy, r.patience = nil, now, true, 180
            r.note = "exploring toward " .. tx .. " " .. tz .. " (" .. #r.places .. " places found)"
            pcall(bot.call, "team_member", { bot = r.bot, objective = "build", plan = "blocks", members = { r.bot }, leader = r.bot, count = 1,
                solo = true, name = "explore", blocks = { { tx, math.floor(st.position.y) + 1, tz, "torch" } } })
            return
        end
        if not r.cur then r.note = "all roads laid" return end
    end
    local c = r.cur
    local dx, dz = c.x - c.px, c.z - c.pz
    local len = math.sqrt(dx * dx + dz * dz)
    if len < 2 then
        r.done[c.x .. " " .. c.z] = true
        r.laid = r.laid + 1
        keep(r)
        r.cur = nil
        return
    end
    if r.rail then return rail_tick(r, c, st, now) end
    local step = math.min(len, 32)
    local ex, ez = math.floor(c.px + dx / len * step + 0.5), math.floor(c.pz + dz / len * step + 0.5)
    local here_y = math.floor(st.position.y)
    local away = math.max(math.abs(st.position.x - c.px), math.abs(st.position.z - c.pz))
    viewer = r.bot
    -- Looking around the level the road has reached, not 40 up and down: each look goes to the bot's own
    -- process and back, and thousands of them hold up every other plugin call
    local blocks = road(c.px, c.pz, ex, ez, c.y, r.item, 16)
    if #blocks == 0 and away <= 24 then
        -- A slope steeper than that: look the whole way up and down from where the bot stands
        blocks = road(c.px, c.pz, ex, ez, here_y, r.item, 40)
    end
    -- The level the road has reached: its last block (they come in order along the way)
    for _, b in ipairs(blocks) do
        if b[4] == r.item then c.y = b[2] end
    end
    local far = math.floor(math.sqrt((c.x - r.origin.x) ^ 2 + (c.z - r.origin.z) ^ 2) + 0.5)
    local function post(x, z, l1, l2, l3, l4)
        local h = ground_height(x, z, c.y + 16, c.y - 16)
        if h then add(blocks, x, h + 1, z, r.sign .. "|" .. l1 .. "|" .. l2 .. "|" .. l3 .. "|" .. l4) end
    end
    if #blocks > 0 and c.first then
        -- Each road's sign at the origin stands a little further along it, so they don't pile up
        local along = 4 + 2 * r.laid
        post(math.floor(c.px + dx / len * along + 0.5) - 2, math.floor(c.pz + dz / len * along + 0.5), c.name, c.what, "at " .. c.x .. " " .. c.z, far .. " blocks")
    end
    if #blocks > 0 and step == len then
        post(c.x - 2, c.z, r.origin.name, "at " .. r.origin.x .. " " .. r.origin.z, far .. " blocks", "")
    end
    -- A waypoint sign every 128 blocks, 3 to the side of the road: where it leads and how far is left
    local from_start = function(x, z) return math.sqrt((x - r.origin.x) ^ 2 + (z - r.origin.z) ^ 2) end
    if #blocks > 0 and step < len and math.floor(from_start(ex, ez) / 128) > math.floor(from_start(c.px, c.pz) / 128) then
        post(math.floor(ex - dz / len * 3 + 0.5), math.floor(ez + dx / len * 3 + 0.5), "-> " .. c.name, c.what,
            math.floor(len - step + 0.5) .. " blocks", "<- " .. r.origin.name)
    end
    viewer = nil
    local order = { bot = r.bot, objective = "build", plan = "blocks", members = { r.bot }, leader = r.bot, count = 1, solo = true, name = "road" }
    r.best, r.best_at, r.busy = nil, now, true
    if #blocks == 0 and away > 24 then
        -- The start of this piece is out of the bot's sight: a torch to put there takes it there
        order.blocks = { { c.px, c.y + 1, c.pz, "torch" } }
        r.patience = 120
        r.note = "going to " .. c.px .. " " .. c.pz
    else
        -- (Nothing to plan though the bot is there: sea or void, the road picks up again further on)
        c.px, c.pz, c.first = ex, ez, c.first and #blocks == 0
        if #blocks == 0 then r.busy = false return end
        order.blocks = blocks
        r.patience = 300
        r.note = "road to " .. c.name .. " at " .. c.x .. " " .. c.z .. ": " .. math.floor(len - step + 0.5) .. " blocks to go"
    end
    pcall(bot.call, "team_member", order)
end

local advance_castle

-- Every 20 seconds: roads, vault and survival construction maintenance
bot.every(20, function()
    if MIGRATING then return end      -- the bots are moving to a new place (migrate plugin)
    local ok, err = pcall(road_tick)
    if not ok then io.stderr:write("[roads] ", tostring(err), "\n") end
    pcall(grow_vault)
    -- The pens: a try when they are finished, then every ten minutes, six times at most (animals wander in,
    -- the plots give wheat). Not while a stage is being handed over
    if ranch_due and not job then
        ranch_due = ranch_due - 1
        if ranch_due <= 0 then
            ranch_tries = ranch_tries + 1
            local ok, r = pcall(ranch, {})
            ranch_due = (ok and r.stocked >= 4 or ranch_tries >= 6) and nil or 30
        end
    end
    if job and not job.creative then advance_castle() end
end)


advance_castle = function()
    if not job then return end
    local over, why = stage_over()
    if not over then return end
    io.stderr:write(os.date("%H:%M:%S"), " [castle] stage ", job.stages[job.index].name, " over: ", tostring(why), "\n")
    if why ~= "done" then
        pcall(bot.call, "chat", { message = "[castle] moving on from " .. job.stages[job.index].name .. ": " .. why })
    end
    if job.stages[job.index].after then job.stages[job.index].after() end
    job.index = job.index + 1
    job.best, job.best_at, job.idle_since, job.since = nil, nil, nil, nil
    if job.saved then
        job.saved.index = job.index
        job.saved.stage = job.stages[job.index] and job.stages[job.index].name or nil
        save(JOB_FILE, job.saved)
    end
    if job.index > #job.stages then
        pcall(bot.call, "chat", { message = "[castle] finished. Run castle farms to water, till and sow the plots" })
        job = nil
        CREATIVE_CONSTRUCTION = false
        save(JOB_FILE, nil)
    else
        hand_over()
    end
end
bot.every(2, function()
    if not MIGRATING and job and job.creative then advance_castle() end
end)

-- Water, till and sow the plots, with the bot the tool is called for: needs one water_bucket per plot, a hoe, seeds
local function farms(args)
    if not job_site then error("no castle site yet: run castle start first") end
    local cx, y, cz = job_site.x, job_site.y, job_site.z
    local have = {}
    for _, slot in ipairs(bot.call("inventory", { bot = args.bot })) do have[slot.item] = (have[slot.item] or 0) + slot.count end
    local hoe, seed
    for item in pairs(have) do
        if item:match("_hoe$") then hoe = item end
        if item == "minecraft:wheat_seeds" or item == "minecraft:carrot" or item == "minecraft:potato" or item == "minecraft:beetroot_seeds" then seed = item end
    end
    if not hoe then error("needs a hoe (craft_item wooden_hoe)") end
    local watered, tilled, sown = 0, 0, 0
    local function try(tool, a) a.bot = args.bot; return (pcall(bot.call, tool, a)) end
    for _, p in ipairs(job_site.plots) do
        try("goto", { x = p[1] + 1, y = y, z = p[2], tolerance = 2 })
        -- Emptying the bucket on the bottom of the hole fills the hole
        if have["minecraft:water_bucket"] and try("use_item", { item = "water_bucket", x = p[1], y = y - 1, z = p[2] }) then
            watered = watered + 1
        end
        try("equip", { item = hoe })
        for dx = -4, 4 do
            for dz = -4, 4 do
                if not (dx == 0 and dz == 0) and try("interact_block", { x = p[1] + dx, y = y - 1, z = p[2] + dz }) then
                    tilled = tilled + 1
                    if seed and try("place_block", { item = seed, x = p[1] + dx, y = y, z = p[2] + dz }) then sown = sown + 1 end
                    try("equip", { item = hoe })
                end
            end
        end
    end
    return { plots_watered = watered, tilled = tilled, sown = sown,
        note = (watered == 0 and "no water_bucket: farmland more than 4 blocks from water dries out. " or "") .. (seed and "" or "no seeds: tilled soil with no crop turns back to dirt") }
end

local castle_tool = {
    name = "castle",
    description = "Build a castle with the whole team: level the ground, then a paved floor, an underground vault under the keep with a double chest per category of item (food, weapons, armor, equipment, valuables, materials, building blocks) where the bots then store everything sorted (another floor is dug underneath by itself when a category fills up), a keep, a first wall, a village of 7 houses with glass-pane windows (the bots dig sand and smelt the glass) and pitched roofs of wooden stairs, a second wall, 7 farm plots, 4 fenced pens (cow, sheep, chicken, pig) that the bots stock by luring animals in, and a third wall, each wall 5 blocks wide (a 3-wide walkway with 1 block on each side standing 1 block higher) with a tower on its four corners, a gatehouse around the south gate, and a moat around it all, 5 wide and 3 deep, filled with water by the bots (they craft buckets and make a well), crossed by a fixed wooden bridge from the gatehouse (269x269 blocks in all). action: ranch (after the pens are built: for each pen the bot has the food for, open the gate, lead the animals within 48 blocks inside, breed them, shut the gate; start does it by itself once the pens stand, then every ten minutes), water (only the water, for a moat already dug: the bots craft buckets from iron, make a well that never runs dry, and fill the ditch from it; start does this by itself at the end), roads (after the castle: a 3-wide road with torches from the outer gate to each of the team's mines, see the mines tool, with bridges (a deck with a parapet on each side) over water and over ravines, and a signpost at each end saying which mine it leads to, what it gives and how far; item: road block, default cobblestone; sign: default spruce_sign), survey (find flat sites near the bot; size=test for small ones), test (a miniature at x y z going through every kind of work: levelling, paved floor, a piece of wall, a house, a farm plot), roadbot (takes one bot out of the team for good, its whole job being roads: from x y z, or where it stands, named `from` on the signs, default Castle, to every mine of the team, those opened later too, and to the places given: structures = names for /locate like \"village_plains,mineshaft\", places = { {name, what, x, z} }; one road after the other, with a signpost at each end saying what the road leads to, where and how far; bot: which one, default the last; stop=true gives it back; with no argument, what it is doing), explore (the same road builder as an explorer: it finds the structures itself with /locate, from the origin and then from the end of each road it lays, so it reaches further and further: structures = what to look for, default villages, pillager outposts, ruined portals, desert and jungle temples, igloos, swamp huts, mineshafts, trail ruins, trial chambers; radius = how far from the origin, default 1500; each find is announced in the chat and gets a road with a signpost at both ends and a waypoint sign every 128 blocks saying where it leads and how far is left; stop=true gives the bot back), plan (what and how many blocks), start (level and build, stage by stage, automatically), level (only the levelling of the whole site, x y z as for start), status, farms (water, till and sow the plots, needs water buckets, a hoe and seeds), stop. x z: centre; y: floor level to stand on. item: wall block, default stone, which the bots make themselves: mine cobblestone, mine coal for fuel, build a furnace each, smelt. floor: paving of the courtyard, the keep and the houses, default stone_bricks (crafted from that stone). item=cobblestone skips the smelting and is far quicker.",
    params = { creative = "boolean", action = "string", x = "number", y = "number", z = "number", item = "string", floor = "string", sign = "string", size = "string", bot = "string", rail = "boolean" },
    run = function(args)
        local action = args.action or "status"
        if action == "status" then
            if not job then return { building = false, site = job_site or load_site() } end
            local left = progress()
            return { building = true, stage = job.stages[job.index].name, stage_number = job.index, stages = #job.stages, blocks_left = left, site = job_site, creative = job.creative }
        elseif action == "stop" then
            forget_unbuilt_site(job)
            job = nil
            CREATIVE_CONSTRUCTION = false
            return "castle job dropped"
        elseif action == "farms" then
            job_site = job_site or load_site()
            return farms(args)
        elseif action == "water" then
            -- Just the water of the moat, for a moat that is already dug
            job_site = job_site or load_site()
            if not job_site then error("no castle site yet: run castle start first") end
            if job then error("a castle job is under way (" .. job.stages[job.index].name .. "): the water is its last stages anyway") end
            local cx, y, cz = job_site.x, job_site.y, job_site.z
            local w, m = {}, {}
            well(w, cx, y, cz)
            moat_water(m, cx, y, cz)
            job = { stages = { { name = "moat well", blocks = w }, { name = "moat water", blocks = m } }, index = 1 }
            hand_over()
            return { started = true, buckets_to_pour = #m, note = "each bot makes a bucket from 3 iron ingots, fills it at the well and empties it in the ditch" }
        elseif action == "ranch" then
            return ranch(args)
        elseif action == "rehand" then
            -- The current stage handed over again (after bots changed groups: their orders were cleared)
            if not job then return "no castle job" end
            hand_over()
            return { stage = job.stages[job.index].name }
        elseif action == "roadbot" or action == "explore" then
            -- One bot taken out of the team to do nothing but roads
            if args.stop then
                if roadman then pcall(bot.call, "team_member", { bot = roadman.bot, objective = "disband", members = {}, leader = "", count = 1, solo = false }) end
                roadman = nil
                save(ROADS_FILE, nil)
                return "the road builder is back with the others"
            end
            if not args.bot and roadman then
                return { bot = roadman.bot, roads_laid = roadman.laid, doing = roadman.note, places = roadman.places }
            end
            local names = bot.call("bots")
            local who = args.bot or names[#names].name
            local st = bot.call("status", { bot = who })
            job_site = job_site or load_site()
            local origin = { name = args.from or "Castle", x = math.floor(args.x or st.position.x), y = math.floor(args.y or st.position.y), z = math.floor(args.z or st.position.z) }
            -- Structures to find with /locate (the bot asking has to be an operator): "village_plains,mineshaft"
            local places, not_found = args.places or {}, {}
            for name in (args.structures or ""):gmatch("[%w_:#]+") do
                local ok, at = pcall(bot.call, "locate", { bot = who, kind = "structure", name = name })
                if not (ok and type(at) == "table" and at.x) then ok, at = pcall(bot.call, "locate", { kind = "structure", name = name }) end
                if ok and type(at) == "table" and at.x then
                    places[#places + 1] = { name = name:gsub("^#?minecraft:", ""):gsub("_", " "), x = at.x, z = at.z }
                else
                    not_found[#not_found + 1] = name
                end
            end
            -- (rail=false: the old road alone, along a straight line and over the hills)
            local castle_origin = job_site and math.abs(origin.x - job_site.x) <= 8 and math.abs(origin.z - job_site.z) <= 8
            roadman = { bot = who, origin = origin, places = places, done = {}, laid = 0, patience = 300,
                item = args.item or "cobblestone", sign = args.sign or "spruce_sign", rail = args.rail ~= false, castle = castle_origin or nil }
            if action == "explore" then
                roadman.explore = args.structures or STRUCTURES
                roadman.reach = math.floor(args.radius or 1500)
            end
            keep(roadman)
            if action == "explore" then
                return { bot = who, from = origin, looks_for = roadman.explore, within = roadman.reach,
                    note = "this bot now explores: it finds these structures (/locate, from the end of each road it lays) and lays a road to each, with signposts at both ends and a waypoint sign every 128 blocks; the team's mines get roads too" }
            end
            return { bot = who, from = origin, places = places, not_found = not_found,
                note = "this bot now only builds roads: to these places and to every mine of the team (mines tool), with a signpost at each end" }
        elseif action == "roads" then
            -- From the castle's outer gate to each of the team's mines (see the mines tool)
            job_site = job_site or load_site()
            if not job_site then error("no castle site yet: run castle start first") end
            if job then error("a castle job is under way (" .. job.stages[job.index].name .. "): roads come after, or castle stop first") end
            -- From the far end of the strip of ground that crosses the moat in front of the gate
            local gate_x, gate_z = job_site.x, job_site.z + OUTER + 1
            local stages, report = {}, {}
            local sign = args.sign or "spruce_sign"
            for _, m in ipairs(bot.call("mines")) do
                local blocks, unseen, bridged = road(gate_x, gate_z, m.x, m.z, job_site.y, args.item or "cobblestone")
                -- A signpost at each end, beside the road: which mine this road leads to, and the way back
                local what = (m.resources or ""):gsub("_ore", ""):gsub(",", ", ")
                if what == "" then what = "ore" end
                local far = math.floor(math.sqrt((m.x - gate_x) ^ 2 + (m.z - gate_z) ^ 2) + 0.5)
                local function post(x, z, l1, l2, l3, l4)
                    local h = ground_height(x, z, job_site.y + 40, job_site.y - 40)
                    if h then add(blocks, x, h + 1, z, sign .. "|" .. l1 .. "|" .. l2 .. "|" .. l3 .. "|" .. l4) end
                end
                -- Each road's sign at the gate stands a little further along it, so they don't pile up
                local along = 4 + 2 * #stages
                local dx, dz = m.x - gate_x, m.z - gate_z
                local len = math.max(1, math.sqrt(dx * dx + dz * dz))
                post(math.floor(gate_x + dx / len * along + 0.5) - 2, math.floor(gate_z + dz / len * along + 0.5), "Mine", what, "at " .. m.x .. " " .. m.z, far .. " blocks")
                post(m.x - 2, m.z, "Castle", "at " .. job_site.x .. " " .. job_site.z, far .. " blocks", "")
                if #blocks > 0 then stages[#stages + 1] = { name = "road to the mine at " .. m.x .. " " .. m.z, blocks = blocks } end
                report[#report + 1] = { mine = m.x .. " " .. m.y .. " " .. m.z, blocks = #blocks, out_of_sight = unseen, bridge_steps = bridged }
            end
            if #stages == 0 then
                return { started = false, roads = report, note = "no mine known yet (bots record one when they dig down for ore; mines {x,y,z} declares one), or none within sight" }
            end
            job = { stages = chunked(stages), index = 1 }
            hand_over()
            return { started = true, roads = report, note = "3 wide, in the ground, bridges over water and ravines, torches alongside; parts out of sight are not planned: run roads again from further along" }
        end
        -- Stone, as asked: the bots mine cobblestone and coal, each builds a furnace and smelts
        local item = args.item or "stone"
        local cx, y, cz
        if action ~= "survey" then
            if not (args.x and args.y and args.z) then error("needs x, y and z: the centre and the floor level") end
            cx, y, cz = math.floor(args.x), math.floor(args.y), math.floor(args.z)
            -- The ground is looked at through the eyes of the bot nearest to the centre (or the one named):
            -- the first bot may be far away, and what it can't see is not levelled
            viewer = args.bot
            if not viewer then
                local best
                for _, b in ipairs(bot.call("bots")) do
                    local ok, st = pcall(bot.call, "status", { bot = b.name })
                    if ok and type(st) == "table" and st.position then
                        local d = (st.position.x - cx) ^ 2 + (st.position.z - cz) ^ 2
                        if not best or d < best then best, viewer = d, b.name end
                    end
                end
            end
        end
        if action == "survey" then
            -- Flat sites for the castle (default) or for the miniature test (size = "test")
            local half = args.size == "test" and 12 or OUTER
            local me = bot.call("status", { bot = args.bot }).position
            return { half_size = half, sites = survey(half, me.x, me.z, args.spawn_x or 5, args.spawn_z or 15),
                note = "y is the floor level to stand on; rough is the height difference across the site" }
        elseif action == "test" then
            pcall(bot.call, "goto", { x = cx, y = y, z = cz, tolerance = 6 })
            local ground, unseen = level(cx, y, cz, "cobblestone", 12)
            local mini = miniature(cx, y, cz, item, args.floor or "stone_bricks")
            table.insert(mini, 1, { name = "test lighting", blocks = lighting(cx, cz, 12, y) })
            table.insert(mini, 1, { name = "test levelling", blocks = ground })
            job = { stages = chunked(mini), index = 1, site = { cx - 14, y - 12, cz - 14, cx + 14, y + 40, cz + 14 } }
            job_site = { x = cx, y = y, z = cz, plots = { { cx + 7, cz + 4 } } }
            hand_over()
            return { started = true, stages = #job.stages, levelling_blocks = #ground, unseen_columns = unseen }
        end
        if action == "level" then
            -- Only the levelling of the whole site, moat included: start afterwards finds little left to do
            pcall(bot.call, "goto", { x = cx, y = y, z = cz, tolerance = 8 })
            local ground, unseen = level(cx, y, cz, "cobblestone")
            job = { stages = chunked({ { name = "levelling", blocks = ground } }), index = 1,
                site = { cx - OUTER, y - 12, cz - OUTER, cx + OUTER, y + 40, cz + OUTER } }
            hand_over()
            return { started = true, stages = #job.stages, levelling_blocks = #ground, unseen_columns = unseen }
        end
        local stages = structures(cx, y, cz, item, args.floor or "stone_bricks", args.creative)
        if action == "plan" then
            local out, total = {}, 0
            for _, s in ipairs(stages) do out[#out + 1] = { stage = s.name, blocks = #s.blocks }; total = total + #s.blocks end
            return { centre = { x = cx, y = y, z = cz }, footprint = (2 * OUTER + 1) .. "x" .. (2 * OUTER + 1),
                moat = { wide = MOAT_WIDE, deep = MOAT_DEEP, gap_from_the_towers = MOAT_GAP },
                wall_width = THICK, walkway = THICK - 2 * PARAPET, raised_on_each_side = PARAPET,
                walls_at = { R1, R2, R3 }, stages = out, blocks = total, note = "plus the levelling, which depends on the terrain" }
        elseif action == "start" then
            -- The terrain can only be read where the bot sees it: stand in the middle first
            pcall(bot.call, "goto", { x = cx, y = y, z = cz, tolerance = 8 })
            local ground, unseen = level(cx, y, cz, "cobblestone")
            -- Right after the levelling, before anything is built: the site and its margin, lit
            table.insert(stages, 1, { name = "lighting", blocks = lighting(cx, cz, OUTER, y) })
            table.insert(stages, 1, { name = "levelling", blocks = ground })
            local new_site = { cx - OUTER, y - 12, cz - OUTER, cx + OUTER, y + 40, cz + OUTER }
            if job and job.site and table.concat(job.site, " ") ~= table.concat(new_site, " ") then forget_unbuilt_site(job) end
            job = { stages = chunked(stages), index = 1, site = new_site, creative = args.creative }
            job_site = { x = cx, y = y, z = cz, plots = plots(cx, cz) }
            save_site()
            hand_over()
            return { started = true, stages = #job.stages, levelling_blocks = #ground,
                note = unseen > 0 and (unseen .. " columns were out of sight and are not levelled") or "the next stages start by themselves" }
        end
        error("action must be survey, plan, start, level, test, status, farms, ranch, water, roads or stop")
    end,
}

-- Starting a job keeps how it was started; stopping it forgets it
local castle_run = castle_tool.run
castle_tool.run = function(args)
    local result = castle_run(args)
    local action = args.action
    if action == "stop" then
        save(JOB_FILE, nil)
    elseif job and type(result) == "table" and result.started and (action == "start" or action == "test" or action == "level" or action == "roads") then
        local kept = {}
        for k, v in pairs(args) do if k ~= "bot" then kept[k] = v end end
        job.saved = { args = kept, index = job.index, stage = job.stages[job.index] and job.stages[job.index].name or nil }
        save(JOB_FILE, job.saved)
    end
    return result
end
bot.tool(castle_tool)

-- After a restart: the castle job again, at the stage it had reached, once the bots are back (2 minutes,
-- for them to join and for the one planning it to see the ground)
local surveyed_at   -- when the bots were sent over the site, before taking a creative castle up again
bot.every(20, function()
    if MIGRATING then return end      -- the bots are moving to a new place (migrate plugin)
    if not castle_saved or job or os.time() - loaded_at < 120 then return end
    local saved = castle_saved
    -- The ground is looked at through the eyes of the bot nearest the centre: wait (10 minutes at most) for one
    -- within 48 blocks of it, or the levelling plan leaves out all it cannot see, for good
    local args = saved.args or {}
    -- Creative: the bots spread over the site first, one to a spot of a grid (they share one world, so between
    -- them every column of it is loaded), and the plan is made 20 seconds later from the whole ground
    if args.creative and args.action == "start" and args.x and args.z then
        if not surveyed_at then
            local crew = {}
            for _, b in ipairs(bot.call("bots") or {}) do
                local ok, st = pcall(bot.call, "status", { bot = b.name })
                if ok and type(st) == "table" and st.game_mode == "creative" and st.dimension == "minecraft:overworld" then crew[#crew + 1] = b.name end
            end
            if #crew == 0 then return end
            local k = math.ceil(math.sqrt(#crew))
            for i, name in ipairs(crew) do
                local gx, gz = (i - 1) % k, (i - 1) // k
                local x = math.floor(args.x - OUTER + (gx + 0.5) * 2 * OUTER / k)
                local z = math.floor(args.z - OUTER + (gz + 0.5) * 2 * OUTER / k)
                pcall(bot.call, "chat", { bot = name, message = string.format("/tp @s %d %d %d", x, (args.y or 70) + 10, z) })
            end
            pcall(bot.call, "chat", { message = string.format("[castle] %d bots survey the site before the plan", #crew) })
            surveyed_at = os.time()
            return
        end
        if os.time() - surveyed_at < 20 then return end
    elseif args.x and args.z and os.time() - loaded_at < 600 then
        local near = false
        for _, b in ipairs(bot.call("bots") or {}) do
            local ok, st = pcall(bot.call, "status", { bot = b.name })
            if ok and type(st) == "table" and st.position and math.abs(st.position.x - args.x) <= 48 and math.abs(st.position.z - args.z) <= 48 then
                near = true
                break
            end
        end
        if not near then return end
    end
    castle_saved = nil
    local ok, err = pcall(castle_tool.run, saved.args)
    if ok and job then
        -- Where it was, by the stage's name: the levelling and lighting are planned again from the ground as it
        -- is now (only what is left, from the start of the list); the rest is the same plan, found by name
        -- (its number moved, the levelling being shorter). Taken by number, a restart skipped levelling still to do
        local name = saved.stage or ""
        if name:match("^levelling") or name:match("^lighting") or name:match("^test levelling") or name:match("^test lighting") then
            job.index = 1
        else
            job.index = math.max(1, math.min(saved.index or 1, #job.stages))
            for i, s in ipairs(job.stages) do
                if s.name == name then job.index = i; break end
            end
        end
        job.saved.index = job.index
        save(JOB_FILE, job.saved)
        hand_over()
        io.stderr:write(os.date("%H:%M:%S"), " [castle] taken up again at stage ", job.index, " of ", #job.stages,
            type(err) == "table" and err.note and (" (" .. err.note .. ")") or "", "\n")
    else
        -- (A bot not joined yet, say): kept, tried again in 20 seconds
        castle_saved = saved
        io.stderr:write("[castle] could not take the job up again: ", tostring(err), "\n")
    end
end)
