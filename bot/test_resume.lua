-- Offline check: a castle job taken up after a restart stays where it was through a second restart (the saved
-- stage's name is the one it resumed at). Run from a copy of bot/ (it writes ../castle_job.lua):
-- lua5.4 test_resume.lua plugins/castle.lua
local f = io.open("../castle_job.lua")
if f then f:close(); error("../castle_job.lua exists: run from a copy") end
f = io.open("../castle_job.lua", "w")
f:write('return {["index"]=16,["stage"]="keep",["args"]={["action"]="start",["x"]=0,["y"]=70,["z"]=0}}')
f:close()
local tool, timers = nil, {}
bot = { call = function(name, a)
    if name == "get_block" then return { block = a.y < 70 and "minecraft:stone" or "minecraft:air" } end
    if name == "status" then return { position = { x = 0, y = 70, z = 0 }, build_left = 5, build_idle = 0, game_mode = "survival", dimension = "minecraft:overworld" } end
    if name == "bots" then return { { name = "Bot1" } } end
    if name == "sites" or name == "inventory" or name == "mines" then return {} end
    return {}
end, tool = function(t) tool = t end, every = function(s, fn) timers[s] = timers[s] or {}; table.insert(timers[s], fn) end,
    on_chat = function() end, log = function() end }
local real_time = os.time
local ok, err = pcall(function()
    load(io.open(arg[1]):read("a"), "castle")()
    os.time = function() return real_time() + 300 end  -- the bots have been back for long enough
    for _, fn in ipairs(timers[20]) do fn() end
    local st = tool.run({ action = "status" })
    assert(st.building and st.stage == "keep", "taken up at the keep, got " .. tostring(st.stage))
    local saved = dofile("../castle_job.lua")
    assert(saved.stage == "keep", "the saved stage is the keep, for the next restart: " .. tostring(saved.stage))
end)
os.time = real_time
os.remove("../castle_job.lua")
os.remove("../castle_site.txt")
assert(ok, err)
print("resume OK")
