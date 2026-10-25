-- Offline multi-host central coordination: lua5.4 test_central.lua plugins/netlink.lua
local source = assert(io.open(arg[1])):read('a')
local now, instances, lines = 1000, {}, {}
local function instance(id, names, secret)
    local inst = { timers = {}, tools = {}, assignments = {}, names = names }
    local env = setmetatable({}, {__index = _G})
    env.os = setmetatable({time = function() return now end}, {__index = os})
    inst.files = inst.files or {}
    env.io = {open = function(path, mode)
        if mode == 'w' then
            return {write = function(_, text) inst.files[path] = text end, close = function() end}
        end
        local value = path:find('netlink_id') and id or path:find('netlink_key') and secret or inst.files[path]
        if not value then return nil end
        return {read = function() return value end, close = function() end}
    end}
    inst.travels, inst.towns, inst.votes = {}, {}, {}
    env.bot = {
        call = function(tool, a)
            if tool == 'bots' then local out = {} for _, n in ipairs(names) do out[#out+1] = {name=n} end return out end
            if tool == 'status' then
                local p = (inst.positions or {})[a.bot] or {x=0, y=64, z=0}
                return {health=8, food=5, position=p}
            end
            if tool == 'settle' and a.survey then
                local out = {}
                for _, n in ipairs(names) do
                    local p = (inst.positions or {})[n] or {x=0, y=64, z=0}
                    out[#out+1] = {name=n, trade='miners', x=p.x, y=p.y, z=p.z, place={wood=0.5, ore=1, water=0.2, food=0, biome=0.8, hostile=0, lava=0}}
                end
                return out
            end
            if tool == 'settle' and a.vote then
                inst.votes[#inst.votes+1] = a.remote
                local best = a.remote[1]
                return {winner={x=best.x, y=best.y, z=best.z}, voters=#a.remote + #names}
            end
            if tool == 'travel' then inst.travels[#inst.travels+1] = a return 'arrived' end
            if tool == 'empire' then inst.towns[#inst.towns+1] = a.town return {} end
            if tool == 'groups' then inst.assignments[#inst.assignments+1] = a.assign return a.assign end
            if tool == 'chat' then
                lines[#lines+1] = a.message
                for _, peer in ipairs(instances) do peer.chat('<' .. a.bot .. '> ' .. a.message) end
                return true
            end
            return {}
        end,
        tool = function(t) inst.tools[t.name] = t.run end,
        spawn = function(_, f) f() return true end,
        busy = function() return false end,
        every = function(s, f) inst.timers[s] = f end,
        on_chat = function(f) inst.chat = f end,
    }
    inst.lib = assert(load(source, 'netlink', 't', env))()
    inst.env = env
    instances[#instances+1] = inst
    return inst
end
local leader = instance('aaaaaa', {'Brain'}, 'secret')
local worker = instance('bbbbbb', {'Worker1', 'Worker2'}, 'secret')
local other = instance('cccccc', {'Other'}, 'wrong-secret')
local unsigned = instance('dddddd', {'Unsigned'})
-- The worker's bots stand 1500 blocks from the leader's
worker.positions = {Worker1 = {x=1000, y=70, z=1500}, Worker2 = {x=1010, y=70, z=1500}}
leader.positions = {Brain = {x=0, y=64, z=0}}
assert(not pcall(unsigned.tools.central, {mode='leader'}))
leader.tools.central{mode='leader', bot='Brain'}
worker.tools.central{mode='worker', central='aaaaaa'}
other.tools.central{mode='worker', central='aaaaaa'}
assert(worker.env.CENTRAL_ACTIVE)
local lib = leader.lib
for _, raw in ipairs({'', string.rep('\0', 200), string.rep('\255', 600), 'abcabc'}) do
    assert(lib.decompress(lib.compress(raw)) == raw)
end
assert(#lib.compress(string.rep('\0',200)) == 3)
assert(not lib.decompress('\255') and not lib.decompress('\255\0a'))
assert(not lib.decompress(string.rep('\255\255a',24)))
worker.timers[60]()
other.timers[60]()
for i=1,150 do
    now=now+2
    for _, inst in ipairs(instances) do inst.timers[2]() end
    if i==30 then leader.timers[60]() end
end
assert(#worker.assignments == 1, 'worker applies central assignments')
assert(#other.assignments == 0, 'different key rejected')
local report = leader.tools.central{}.reports
assert(report.bbbbbb and not report.cccccc)
local roles = worker.assignments[1]
assert(#roles.farmers == 1 and #roles.butchers == 1, 'hungry community prioritizes food across hosts')
-- Replay delivered command segments: no second execution.
local before = #worker.assignments
for _, line in ipairs(lines) do worker.chat('<Brain> ' .. line) end
assert(#worker.assignments == before)
for _, line in ipairs(lines) do assert(#line <= 256) end
local received = {}
leader.env.NETLINK_BUILD.listen(function(from, raw)
    received[#received+1] = {from=from, raw=raw}
    return 'ok'
end)
local ballot = 'V' .. string.rep('\0', 200)
worker.env.NETLINK_BUILD.send(ballot)
other.env.NETLINK_BUILD.send('Vforged')
for _=1,40 do
    now=now+2
    for _, inst in ipairs(instances) do inst.timers[2]() end
end
assert(#received == 1 and received[1].from == 'bbbbbb' and received[1].raw == ballot, 'compressed construction messages require the shared key')
-- Locations: the worker's bots stand 1500 blocks away; the leader has them all vote and sends the place back
local function run_minutes(n)
    for m = 1, n do
        worker.timers[60]()
        for _ = 1, 30 do now = now + 2 for _, inst in ipairs(instances) do inst.timers[2]() end end
        leader.timers[60]()
        for _ = 1, 30 do now = now + 2 for _, inst in ipairs(instances) do inst.timers[2]() end end
    end
end
run_minutes(5)
local located = leader.tools.central{}.located
assert(located.bbbbbb and located.bbbbbb.bots == 2, 'the worker reports where its bots are')
assert(#leader.votes == 1 and #leader.votes[1] == 2 and leader.votes[1][1].name == 'Worker1' and leader.votes[1][1].place.ore == 1,
    'the leader votes once with the other host\'s bots, places included')
local s = leader.tools.central{}.settlement
assert(s and s.x == 1000 and s.z == 1500, 'the winner is the settlement')
assert(leader.towns[1].x == 1000 and #leader.travels == 1 and leader.travels[1].bot == 'Brain', 'the leader\'s far bot walks there')
run_minutes(1)
assert(worker.towns[1] and worker.towns[1].x == 1000 and worker.towns[1].z == 1500, 'the worker\'s town moves to the settlement')
assert(#worker.travels == 0, 'bots already there do not walk')
run_minutes(6)
assert(#leader.votes == 1, 'the settlement stays: no vote every 5 minutes')
leader.tools.central{settle=true}
run_minutes(1)
assert(#leader.votes == 2, 'settle=true votes again')
-- A restart keeps the mode and the settlement
local restarted = instance('aaaaaa', {'Brain'}, 'secret')
restarted.files = leader.files
restarted.lib = assert(load(source, 'netlink', 't', restarted.env))()
assert(restarted.tools.central{}.mode == 'leader' and restarted.tools.central{}.settlement.x == 1000, 'mode and settlement outlive a restart')
-- A forged place from a host with another key is never taken
assert(not leader.tools.central{}.located.cccccc)
worker.tools.central{mode='off'}
assert(not worker.env.CENTRAL_ACTIVE)
print('central coordination OK: multi-host reports, food priorities, signed authority, replay rejection, bounded compression')
