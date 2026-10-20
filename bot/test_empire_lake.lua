-- Run in an empty temporary directory: lua5.4 /path/to/test_empire_lake.lua /path/to/plugins/empire.lua
-- A lot over a frozen lake is left empty: the next dry lot is given instead, and the wet one is not offered again.
local lake = function(x, z) return x >= 0 and x < 18 and z >= 0 and z < 18 end
bot = {
    call = function(name, a)
        if name == "get_block" then
            -- The lake: stone up to 60, water to 69, ice at 70. Elsewhere grass up to 70
            local function at(y)
                if lake(a.x, a.z) then return y <= 60 and "minecraft:stone" or y <= 69 and "minecraft:water" or y == 70 and "minecraft:ice" or "minecraft:air" end
                return y <= 70 and "minecraft:grass_block" or "minecraft:air"
            end
            local y = a.y
            if a.y_min then
                -- down to the first solid block (water is not solid)
                while y > a.y_min and (at(y) == "minecraft:air" or at(y) == "minecraft:water") do y = y - 1 end
            end
            return { block = at(y), y = y }
        end
        if name == "bots" then return {} end
        return {}
    end,
    tool = function() end, every = function() end, on_chat = function() end,
}
local f = assert(io.open(arg[1])); local src = f:read("a"); f:close()
src = src:gsub("local function lot%(", "function lot("):gsub("local state =", "state =")
assert(load(src, "empire"))()
state.town = { x = 0, y = 71, z = 0 }
local first = lot()
assert(first.x == 18 and first.z == 0 and first.y == 71, "the dry lot east of the lake: " .. first.x .. " " .. first.y .. " " .. first.z)
local second = lot()
assert(second.x == 36, "the lake lot is not offered again: " .. second.x)
print("empire lake OK: lots under water are left empty")
