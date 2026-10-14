-- Offline check of netlink: lua5.4 test_netlink.lua plugins/netlink.lua
-- Two programs (ids aaaaaa and bbbbbb) on one simulated chat: A sends its network, the chat loses one
-- segment, B asks for it again, gets it, checks it, acknowledges and merges the network
local src = io.open(arg[1]):read("a")
local clock = 1000
os.time = function() return clock end
local listeners, lines, dropped = {}, {}, false
local function instance(id, name, brain)
    local f = io.open("../netlink_id.txt", "w"); f:write(id, "\n"); f:close()
    local inst = { every = {}, merged = nil }
    local env = setmetatable({}, { __index = _G })
    env.bot = {
        call = function(tool, a)
            if tool == "bots" then return { { name = name } } end
            if tool == "chat" then
                lines[#lines + 1] = a.message
                -- The chat loses the second segment of the first message, once
                if not dropped and a.message:match("^%[nn%] aaaaaa %x+ 2/") then dropped = true return "sent" end
                for _, l in ipairs(listeners) do l("<" .. name .. "> " .. a.message) end
                return "sent"
            end
            if tool == "brain" then
                if a.export then return brain end
                if a.merge then inst.merged = a.merge return { mixed = 0.25 } end
            end
            return {}
        end,
        tool = function(t) inst.tool = t end,
        every = function(s, f) inst.every[#inst.every + 1] = { s = s, f = f } end,
        on_chat = function(f) listeners[#listeners + 1] = f end,
        log = print,
    }
    inst.lib = load(src, "netlink", "t", env)()
    return inst
end
local w = {}
for i = 1, 235 do w[i] = math.sin(i) * 0.8 end
local A = instance("aaaaaa", "BotA1", { samples = 30, w = w })
local B = instance("bbbbbb", "BotB1", { samples = 0, w = w })
os.remove("../netlink_id.txt")

-- The pieces
local lib = A.lib
local bytes = ""
for i = 0, 255 do bytes = bytes .. string.char(i) end
for n = 0, 5 do assert(lib.b64decode(lib.b64encode(bytes:sub(1, 250 + n))) == bytes:sub(1, 250 + n), "base64 round trip") end
local back = lib.unpack(lib.pack({ w = w, samples = 30 }))
for i = 1, #w do assert(math.abs(back.w[i] - w[i]) <= 0.8 / 127 + 1e-6, "8-bit weights within a step") end
assert(back.samples == 30)

-- The exchange: A sends, the pumps run every 2 seconds for a minute and a half
A.tool.run({ send = true })
for _ = 1, 45 do
    clock = clock + 2
    for _, inst in ipairs({ A, B }) do for _, e in ipairs(inst.every) do if e.s <= 2 then e.f() end end end
end
local nack, ack = false, false
for _, l in ipairs(lines) do
    if l:match("^%[nn%] bbbbbb NACK aaaaaa") then nack = true end
    if l:match("^%[nn%] bbbbbb ACK aaaaaa") then ack = true end
    assert(#l <= 256, "a chat line fits: " .. #l)
end
assert(dropped and nack, "the lost segment is asked for again")
assert(ack, "the whole message is acknowledged")
assert(B.merged and B.merged.samples == 30 and #B.merged.w == 235, "B merged A's network")
local st = A.tool.run({})
assert(st.counters.resent == 1 and st.counters.acks == 1, "A resent one segment and got the acknowledgement")
print(string.format("netlink plugin OK: %d chat lines, longest %d characters", #lines, (function() local m = 0 for _, l in ipairs(lines) do m = math.max(m, #l) end return m end)()))
