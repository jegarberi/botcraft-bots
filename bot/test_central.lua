-- Offline multi-host central coordination: lua5.4 test_central.lua plugins/netlink.lua
local source = assert(io.open(arg[1])):read('a')
local now, instances, lines = 1000, {}, {}
local function instance(id, names, secret)
    local inst = { timers = {}, tools = {}, assignments = {}, names = names }
    local env = setmetatable({}, {__index = _G})
    env.os = setmetatable({time = function() return now end}, {__index = os})
    env.io = {open = function(path, mode)
        local value = path:find('netlink_id') and id or secret
        if not value then return nil end
        return {read = function() return value end, close = function() end}
    end}
    env.bot = {
        call = function(tool, a)
            if tool == 'bots' then local out = {} for _, n in ipairs(names) do out[#out+1] = {name=n} end return out end
            if tool == 'status' then return {health=8, food=5} end
            if tool == 'groups' then inst.assignments[#inst.assignments+1] = a.assign return a.assign end
            if tool == 'chat' then
                lines[#lines+1] = a.message
                for _, peer in ipairs(instances) do peer.chat('<' .. a.bot .. '> ' .. a.message) end
                return true
            end
            return {}
        end,
        tool = function(t) inst.tools[t.name] = t.run end,
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
worker.tools.central{mode='off'}
assert(not worker.env.CENTRAL_ACTIVE)
print('central coordination OK: multi-host reports, food priorities, signed authority, replay rejection, bounded compression')
