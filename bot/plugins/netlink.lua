-- Learning shared between programs on different computers whose bots play on the same server, through the
-- only thing they have in common: the game chat. A small TCP-like layer:
--   every program has an id (kept in a file); a message is a string of bytes, base64-encoded and cut into
--   segments that fit a chat line:   [nn] <from> <msg> <seq>/<total> <checksum> <data>
--   the receivers put the segments back together, check the checksum (FNV-1a of the whole message) and
--   acknowledge:                      [nn] <from> ACK <to> <msg>
--   a message still missing segments 20 seconds after its first one asks for them again, and the sender
--   (which keeps what it sent for 5 minutes) sends them again:   [nn] <from> NACK <to> <msg> <seq,seq,...>
--   one line every 2 seconds at most leaves this program: servers kick chat spammers.
-- What goes over it: the brain's network (its weights as 8-bit numbers with one scale, and how many lessons
-- it learnt from), every 10 minutes when it learnt something new. Each program merges what it receives
-- (the brain tool's merge), so the bots everywhere learn from each other.
--   netlink {}                what it sent and received, the peers seen
--   netlink { send = true }   send the network now
local ID_FILE = "../netlink_id.txt"
local SEGMENT = 150                 -- base64 characters a line
local KEEP = 300                    -- seconds a sent message is kept for resending
local ASK_AFTER = 20                -- seconds before missing segments are asked for

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil
end

-- ---------------------------------------------------------------- base64, checksum
local B64 = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"
local B64_INDEX = {}
for i = 1, 64 do B64_INDEX[B64:sub(i, i)] = i - 1 end

local function b64encode(data)
    local out = {}
    for i = 1, #data, 3 do
        local a, b, c = data:byte(i, i + 2)
        local n = (a << 16) | ((b or 0) << 8) | (c or 0)
        out[#out + 1] = B64:sub((n >> 18) + 1, (n >> 18) + 1) .. B64:sub(((n >> 12) & 63) + 1, ((n >> 12) & 63) + 1) ..
            (b and B64:sub(((n >> 6) & 63) + 1, ((n >> 6) & 63) + 1) or "=") .. (c and B64:sub((n & 63) + 1, (n & 63) + 1) or "=")
    end
    return table.concat(out)
end

local function b64decode(text)
    local out = {}
    for i = 1, #text, 4 do
        local chunk = text:sub(i, i + 3)
        local n, pad = 0, 0
        for j = 1, 4 do
            local ch = chunk:sub(j, j)
            if ch == "=" or ch == "" then pad = pad + 1; n = n << 6
            else
                local v = B64_INDEX[ch]
                if not v then return nil end
                n = (n << 6) | v
            end
        end
        out[#out + 1] = string.char((n >> 16) & 255)
        if pad < 2 then out[#out + 1] = string.char((n >> 8) & 255) end
        if pad < 1 then out[#out + 1] = string.char(n & 255) end
    end
    return table.concat(out)
end

local function checksum(data)
    local h = 2166136261
    for i = 1, #data do h = ((h ~ data:byte(i)) * 16777619) & 0xffffffff end
    return string.format("%08x", h)
end

-- ---------------------------------------------------------------- this program's id
local function load_id()
    local f = io.open(ID_FILE, "r")
    local id = f and f:read("l")
    if f then f:close() end
    if id and id:match("^%x+$") then return id end
    math.randomseed(os.time() + math.floor(os.clock() * 1e6))
    id = string.format("%06x", math.random(0, 0xffffff))
    f = io.open(ID_FILE, "w")
    if f then f:write(id, "\n"); f:close() end
    return id
end
local ME = load_id()

-- ---------------------------------------------------------------- the network as bytes
-- "N1", how many numbers, lessons, scale, then each number as -127..127 of the scale
local function pack_network(net)
    local scale = 1e-9
    for _, v in ipairs(net.w) do scale = math.max(scale, math.abs(v)) end
    local parts = { string.pack(">c2I2I4f", "N1", #net.w, net.samples or 0, scale) }
    for _, v in ipairs(net.w) do parts[#parts + 1] = string.pack(">i1", math.floor(v / scale * 127 + 0.5)) end
    return table.concat(parts)
end

local function unpack_network(data)
    if #data < 12 or data:sub(1, 2) ~= "N1" then return nil end
    local _, count, samples, scale, pos = string.unpack(">c2I2I4f", data)
    if #data < pos - 1 + count then return nil end
    local w = {}
    for i = 1, count do
        local q
        q, pos = string.unpack(">i1", data, pos)
        w[i] = q / 127 * scale
    end
    return { w = w, samples = samples }
end

-- ---------------------------------------------------------------- the transport
local outbox = {}        -- lines waiting to go out, one every 2 seconds
local sent = {}          -- msg -> { at, segments = { seq -> line } }
local pending = {}       -- from .. " " .. msg -> { from, msg, total, chk, parts, first, asked }
local peers = {}         -- from -> { last_seen, received, merged }
local counters = { sent_messages = 0, sent_lines = 0, received_messages = 0, bad_checksum = 0, resent = 0, acks = 0 }
local next_msg = 0
local handlers = {}

local function speaker()
    local bots = call("bots", {}) or {}
    return bots[1] and bots[1].name or nil
end

local function send(data)
    next_msg = next_msg + 1
    local msg = string.format("%x", (os.time() * 16 + next_msg) % 0xfffff)
    local text, chk = b64encode(data), checksum(data)
    local total = math.ceil(#text / SEGMENT)
    local segments = {}
    for seq = 1, total do
        segments[seq] = string.format("[nn] %s %s %d/%d %s %s", ME, msg, seq, total, chk, text:sub((seq - 1) * SEGMENT + 1, seq * SEGMENT))
        outbox[#outbox + 1] = segments[seq]
    end
    sent[msg] = { at = os.time(), segments = segments }
    counters.sent_messages = counters.sent_messages + 1
    return msg, total
end

handlers.network = function(from, data)
    local net = unpack_network(data)
    if not net then return end
    local r = call("brain", { merge = net })
    peers[from].merged = (peers[from].merged or 0) + (r and 1 or 0)
end

local function deliver(p)
    local text = table.concat(p.parts)
    local data = b64decode(text)
    if not data or checksum(data) ~= p.chk then
        counters.bad_checksum = counters.bad_checksum + 1
        return
    end
    counters.received_messages = counters.received_messages + 1
    peers[p.from].received = (peers[p.from].received or 0) + 1
    outbox[#outbox + 1] = string.format("[nn] %s ACK %s %s", ME, p.from, p.msg)
    if data:sub(1, 2) == "N1" then handlers.network(p.from, data) end
end

bot.on_chat(function(line)
    local rest = line:match("^<[^>]+> %[nn%] (.+)$")
    if not rest then return end
    local from, a, b, c, d, e = rest:match("^(%x+) (%S+) (%S+) ?(%S*) ?(%S*) ?(%S*)$")
    if not from or from == ME then return end
    peers[from] = peers[from] or {}
    peers[from].last_seen = os.time()
    if a == "ACK" then
        if b == ME then counters.acks = counters.acks + 1 end
        return
    end
    if a == "NACK" then
        -- b: to whom, c: msg, d: the missing segments
        if b ~= ME or not sent[c] then return end
        for seq in d:gmatch("%d+") do
            local segment = sent[c].segments[tonumber(seq)]
            if segment then outbox[#outbox + 1] = segment; counters.resent = counters.resent + 1 end
        end
        return
    end
    -- A segment: a = msg, b = seq/total, c = checksum, d = data
    local seq, total = b:match("^(%d+)/(%d+)$")
    seq, total = tonumber(seq), tonumber(total)
    if not seq or not total or total > 64 or seq < 1 or seq > total then return end
    local key = from .. " " .. a
    local p = pending[key]
    if not p then
        p = { from = from, msg = a, total = total, chk = c, parts = {}, first = os.time(), count = 0 }
        pending[key] = p
    end
    if not p.parts[seq] then p.parts[seq] = d; p.count = p.count + 1 end
    if p.count == p.total then
        pending[key] = nil
        deliver(p)
    end
end)

-- The pump: one line out every 2 seconds; missing segments asked for; old messages forgotten
bot.every(2, function()
    local now = os.time()
    if #outbox > 0 then
        local who = speaker()
        if who then
            call("chat", { bot = who, message = table.remove(outbox, 1) })
            counters.sent_lines = counters.sent_lines + 1
        end
    end
    for key, p in pairs(pending) do
        if now - p.first > ASK_AFTER and not p.asked then
            local missing = {}
            for seq = 1, p.total do if not p.parts[seq] then missing[#missing + 1] = seq end end
            outbox[#outbox + 1] = string.format("[nn] %s NACK %s %s %s", ME, p.from, p.msg, table.concat(missing, ","))
            p.asked = now
        elseif p.asked and now - p.asked > ASK_AFTER * 3 then
            pending[key] = nil          -- given up on
        end
    end
    for msg, s in pairs(sent) do
        if now - s.at > KEEP then sent[msg] = nil end
    end
end)

-- Every 10 minutes: the network, if it learnt something since the last time
local last_samples = -1
local function share()
    local net = call("brain", { export = true })
    if type(net) ~= "table" or not net.w then return nil end
    if net.samples == last_samples then return nil end
    last_samples = net.samples
    return send(pack_network(net))
end
bot.every(600, function() share() end)

bot.tool{
    name = "netlink",
    description = "Learning shared with bots run by other programs (other computers) on the same server, over the game chat: the brain's network is sent every 10 minutes when it learnt something (base64 in numbered, checksummed segments; receivers acknowledge, ask again for missing segments, and merge it into their own brain). With no argument: this program's id, the peers seen and what went back and forth. send=true sends the network now.",
    params = { send = "boolean" },
    run = function(args)
        if args.send then
            last_samples = -1
            local msg, total = share()
            return msg and { message = msg, segments = total } or "the brain has no network to send"
        end
        local list = {}
        for id, p in pairs(peers) do list[id] = { seconds_ago = os.time() - (p.last_seen or 0), received = p.received or 0, merged = p.merged or 0 } end
        return { id = ME, peers = list, counters = counters, waiting_lines = #outbox }
    end,
}

-- For the offline check
return { b64encode = b64encode, b64decode = b64decode, checksum = checksum, pack = pack_network, unpack = unpack_network }
