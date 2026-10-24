-- Offline shared-site council: lua5.4 test_creative.lua plugins/creative.lua
local source = assert(io.open(arg[1])):read('a')
local now, instances, messages = 1000, {}, {}
local function instance(id, roster, signed)
    local inst = {roster = roster, builds = {}, chats = {}}
    local env = setmetatable({}, {__index = _G})
    env.os = setmetatable({time = function() return now end}, {__index = os})
    env.NETLINK_BUILD = {id = id, signed = signed, checksum = function(data)
        local h = 2166136261
        for i = 1, #data do h = ((h ~ data:byte(i)) * 16777619) & 0xffffffff end
        return string.format('%08x', h)
    end,
    listen = function(fn) inst.receive = fn end,
    send = function(raw) messages[#messages + 1] = {from = id, raw = raw}; return 'queued' end}
    env.bot = {
        call = function(tool, a)
            a = a or {}
            if tool == 'bots' then local rows = {} for _, b in ipairs(inst.roster) do rows[#rows+1] = {name=b.name} end return rows end
            if tool == 'status' then for _, b in ipairs(inst.roster) do if b.name == a.bot then return b end end end
            if tool == 'get_block' then return {block = a.y < 70 and 'minecraft:stone' or 'minecraft:air'} end
            if tool == 'castle' then
                if a.action == 'start' then
                    inst.builds[#inst.builds+1] = a
                    inst.site = {x=a.x, y=a.y, z=a.z}
                    return {started=true}
                end
                return {building=false, site=inst.site}
            end
            if tool == 'chat' then inst.chats[#inst.chats+1] = a.message; return true end
            return {}
        end,
        every = function(_, fn) inst.tick = fn end,
        tool = function(t) inst.tool = t.run end,
    }
    inst.lib = assert(load(source, 'creative', 't', env))()
    instances[#instances+1] = inst
    return inst
end
local function bot(name, x, creative, dimension)
    return {name=name, position={x=x,y=70,z=0}, game_mode=creative and 'creative' or 'survival', dimension=dimension or 'minecraft:overworld'}
end
local function pump(seconds)
    for _=1,seconds//20 do
        now=now+20
        for _, inst in ipairs(instances) do inst.tick() end
        local queue=messages; messages={}
        for _, message in ipairs(queue) do
            for _, inst in ipairs(instances) do
                if inst.receive then inst.receive(message.from, message.raw) end
            end
        end
    end
end
local A = instance('aaaaaa', {bot('CreativeA', 0, true), bot('SurvivalA', 32, false)}, true)
local B = instance('bbbbbb', {bot('CreativeB', 64, true), bot('SurvivalB', 96, false)}, true)
pump(120)
assert(#A.builds==0 and #B.builds==0, 'wait for votes')
-- Join during deliberation: this bot must participate too.
B.roster[#B.roster+1] = bot('LateBot', 32, false)
pump(480)
assert(#A.builds==1 and #B.builds==1, 'one build on each creative host')
local a,b = A.builds[1],B.builds[1]
assert(a.x==b.x and a.y==b.y and a.z==b.z and a.creative and b.creative, 'same coordinates approved on both hosts')
assert(A.tool().decision.voters==5 and B.tool().decision.voters==5, 'survival and late bots voted')
assert(A.chats[1]:match('5 bots agreed: castle, village and farms at 32 70 0'), A.chats[1])
assert(A.receive('ffffff', 'C' .. A.tool().decision.hash)=='rejected:authority')
assert(A.receive('ffffff', 'V' .. A.lib.encode({{name='Later',x=500,y=70,z=0,score=0,creative=true}}))=='rejected:site_already_decided')
pump(300)
assert(#A.builds==1 and #B.builds==1, 'no restart each tick')
local packed = A.lib.encode({{name='Valid',x=0,y=70,z=0,score=0,creative=true}})
assert(A.lib.decode(packed)[1].creative)
assert(not pcall(A.lib.decode, packed:sub(1,-2)), 'truncation rejected')
-- No creative bots: no construction. Later gamemode changes trigger it; no key means a local council.
instances,messages={},{}
local C = instance('cccccc', {bot('Local', 10, false)}, false)
pump(300)
assert(#C.builds==0)
C.roster[1].game_mode='creative'
pump(300)
assert(#C.builds==1)
-- Existing buildings must not be automatically replaced.
instances,messages={},{}
local D = instance('dddddd', {bot('Existing', 0, true)}, false)
D.site={x=100,y=70,z=100}
pump(360)
assert(#D.builds==0)
print('creative council OK: all-bot votes, late joins, matching host approvals, coordinate chat, mode changes, existing-site preservation')
