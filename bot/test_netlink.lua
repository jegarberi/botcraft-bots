-- Offline check of netlink (protocol N2): lua5.4 test_netlink.lua plugins/netlink.lua
local src = io.open(arg[1]):read("a")
local clock = 1000
os.time = function() return clock end
local ARCH = "brain:17x10x5+critic:v2"

-- One simulated chat for several programs. drop(line) says whether the chat loses a line
local listeners, lines = {}, {}
local drop = function() return false end
local function instance(id, name, key, opts)
    opts = opts or {}
    local f = io.open("../netlink_id.txt", "w"); f:write(id, "\n"); f:close()
    if key then f = io.open("../netlink_key.txt", "w"); f:write(key, "\n"); f:close() else os.remove("../netlink_key.txt") end
    local inst = { every = {}, merges = {}, name = name }
    local env = setmetatable({}, { __index = _G })
    env.bot = {
        call = function(tool, a)
            if tool == "bots" then return { { name = name } } end
            if tool == "chat" then
                lines[#lines + 1] = a.message
                if drop(a.message) then return "sent" end
                for _, l in ipairs(listeners) do l(l == inst.listener and "" or (opts.format or "<%s> %s"):format(name, a.message)) end
                return "sent"
            end
            if tool == "brain" then
                if a.export then return { samples = opts.samples or 30, w = opts.w, arch = opts.arch or ARCH } end
                if a.merge then
                    -- Like the brain: the same lessons from the same program once
                    for _, m in ipairs(inst.merges) do
                        if m.origin == a.merge.origin and m.samples >= a.merge.samples then error("rejected:already_merged") end
                    end
                    inst.merges[#inst.merges + 1] = a.merge
                    return { mixed = 0.25 }
                end
            end
            return {}
        end,
        tool = function(t) inst.tool = t end,
        every = function(s, fn) inst.every[#inst.every + 1] = { s = s, f = fn } end,
        on_chat = function(fn) listeners[#listeners + 1] = fn; inst.listener = fn end,
        log = print,
    }
    inst.lib = load(src, "netlink", "t", env)()
    return inst
end
local function pump(insts, seconds)
    for _ = 1, seconds // 2 do
        clock = clock + 2
        for _, inst in ipairs(insts) do for _, e in ipairs(inst.every) do if e.s <= 2 then e.f() end end end
    end
end
local function said(pattern) for _, l in ipairs(lines) do if l:match(pattern) then return true end end return false end

local w = {}
for i = 1, 236 do w[i] = math.sin(i) * 0.8 end

-- The pieces
local lib = instance("aaaaaa", "BotA1", nil, { w = w }).lib
listeners = {}
assert(lib.hex(lib.sha256("abc")) == "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad", "SHA-256")
assert(lib.hex(lib.sha256("")) == "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855", "SHA-256 of nothing")
assert(lib.hex(lib.hmac("Jefe", "what do ya want for nothing?")) == "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843", "HMAC-SHA256 (RFC 4231)")
local bytes = ""
for i = 0, 255 do bytes = bytes .. string.char(i) end
for n = 0, 5 do assert(lib.b64decode(lib.b64encode(bytes:sub(1, 250 + n))) == bytes:sub(1, 250 + n), "base64 round trip") end
local packed = lib.pack({ w = w, samples = 30, arch = ARCH })
local back = assert(lib.unpack(packed, ARCH))
for i = 1, #w do assert(math.abs(back.w[i] - w[i]) <= 0.8 / 32767 + 1e-6, "16-bit weights within a step") end
assert(back.samples == 30 and back.origin == "aaaaaa")
local _, why = lib.unpack(packed, "brain:other")
assert(why == "other_architecture", "a network of another shape is refused")
local nan = packed:sub(1, 19) .. string.pack(">f", 0 / 0) .. packed:sub(24)   -- the scale: bytes 20 to 23
_, why = lib.unpack(nan, ARCH)
assert(why == "not_finite", "a NaN scale is refused")

-- 1. Unsigned, a lost segment asked for again, merged once, the ACK says ok
lines = {}
local dropped = false
drop = function(l) if not dropped and l:match("^%[nn%] aaaaaa %x+ 2/") then dropped = true return true end return false end
local A = instance("aaaaaa", "BotA1", nil, { w = w })
local B = instance("bbbbbb", "BotB1", nil, { w = w, samples = 0 })
A.tool.run({ send = true })
pump({ A, B }, 90)
assert(dropped and said("^%[nn%] bbbbbb NACK aaaaaa"), "the lost segment is asked for again")
assert(#B.merges == 1 and B.merges[1].origin == "aaaaaa" and B.merges[1].samples == 30 and #B.merges[1].w == 236, "B merged A's network")
assert(said("^%[nn%] bbbbbb ACK aaaaaa %x+ ok$"), "the acknowledgement says it was merged")
local st = A.tool.run({})
assert(st.counters.resent == 1 and st.counters.acks == 1, "A resent one segment and got the acknowledgement")
-- The same message again (a resend crossing an ACK): not delivered twice
for _, l in ipairs(lines) do if l:match("^%[nn%] aaaaaa %x+ %d/") then for _, fn in ipairs(listeners) do fn("<BotA1> " .. l) end end end
assert(#B.merges == 1, "a message already delivered is not merged again")
-- The same lessons in a new message: the brain refuses them, the ACK says so
drop = function() return false end
A.tool.run({ send = true })
pump({ A, B }, 30)
assert(#B.merges == 1 and said("ACK aaaaaa %x+ rejected:already_merged"), "the same lessons are merged once")

-- 2. Signed: same secret merged; no signature, or another secret, refused. Lines as "name: text"
listeners, lines = {}, {}
local C = instance("cccccc", "BotC1", "s3cret", { w = w, samples = 40, format = "%s: %s" })
local D = instance("dddddd", "BotD1", "s3cret", { w = w, samples = 0 })
local E = instance("eeeeee", "BotE1", nil, { w = w, samples = 50 })
local F = instance("ffffff", "BotF1", "other", { w = w, samples = 60 })
os.remove("../netlink_key.txt")
os.remove("../netlink_id.txt")
C.tool.run({ send = true })
E.tool.run({ send = true })
F.tool.run({ send = true })
pump({ C, D, E, F }, 120)
local from = {}
for _, m in ipairs(D.merges) do from[m.origin] = true end
assert(from.cccccc, "a message signed with the same secret is merged (sent as 'name: text')")
assert(not from.eeeeee and not from.ffffff, "unsigned and wrongly signed messages are refused")
assert(said("ACK eeeeee %x+ rejected:signature") and said("ACK ffffff %x+ rejected:signature"))
assert(D.tool.run({}).signed and not E.tool.run({}).signed)
for _, l in ipairs(lines) do assert(#l <= 256, "a chat line fits: " .. #l) end
print(string.format("netlink plugin OK: %d chat lines, longest %d characters", #lines,
    (function() local m = 0 for _, l in ipairs(lines) do m = math.max(m, #l) end return m end)()))
