-- Learning shared between programs on different computers whose bots play on the same server, through the
-- only thing they have in common: the game chat. A small TCP-like layer:
--   every program has an id (kept in a file); a message is a string of bytes, base64-encoded and cut into
--   segments that fit a chat line:   [nn] <from> <msg> <seq>/<total> <checksum> <signature> <data>
--   the receivers put the segments back together, check the checksum (FNV-1a of the whole message) and the
--   signature, and acknowledge with what became of it:   [nn] <from> ACK <to> <msg> <ok|rejected:why>
--   a message still missing segments 20 seconds after its first one asks for them again, up to three times,
--   and the sender (which keeps what it sent for 5 minutes) sends them again:
--                                     [nn] <from> NACK <to> <msg> <seq,seq,...>
--   one line every 2 seconds at most leaves this program (servers kick chat spammers), 64 lines waiting at most.
-- What goes over it (protocol N2): the brain's network, every 10 minutes when it learnt something new:
--   "N2", protocol version, a fingerprint of the network's architecture, the lessons it learnt from (also the
--   update's id: the same lessons are merged once), the sending program's id, the weights as 16-bit numbers of
--   one scale. The receiver checks it all (version, architecture, finite numbers) before the brain merges it,
--   and the brain keeps the merge on trial (undone if the days after go worse).
-- Signatures: with the same secret in netlink_key.txt (next to the bot) on every computer, each message
-- carries an HMAC-SHA256 of its content, and unsigned or forged ones are rejected. Without the file messages
-- go unsigned (and the ones received are taken as they come).
--   netlink {}                what it sent and received, the peers seen
--   netlink { send = true }   send the network now
local ID_FILE = "../netlink_id.txt"
local KEY_FILE = "../netlink_key.txt"
local SEGMENT = 150                 -- base64 characters a line
local KEEP = 300                    -- seconds a sent message is kept for resending
local ASK_AFTER = 20                -- seconds between requests for missing segments
local ASK_TIMES = 3                 -- requests before a message is given up
local OUTBOX_MAX = 64               -- lines waiting to be sent, at most (the oldest go first)
local PROTOCOL = 2

local function call(tool, args)
    local ok, r = pcall(bot.call, tool, args)
    return ok and r or nil, (not ok) and tostring(r) or nil
end

-- ---------------------------------------------------------------- base64, checksum, SHA-256, HMAC
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

local K = {
    0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5, 0xd807aa98, 0x12835b01,
    0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174, 0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc,
    0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da, 0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147,
    0x06ca6351, 0x14292967, 0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
    0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070, 0x19a4c116, 0x1e376c08,
    0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3, 0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208,
    0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2,
}

local function sha256(message)
    local function rrot(x, n) return ((x >> n) | (x << (32 - n))) & 0xffffffff end
    local h = { 0x6a09e667, 0xbb67ae85, 0x3c6ef372, 0xa54ff53a, 0x510e527f, 0x9b05688c, 0x1f83d9ab, 0x5be0cd19 }
    local length = #message
    message = message .. "\128" .. string.rep("\0", (55 - length) % 64) .. string.pack(">I8", length * 8)
    for chunk = 1, #message, 64 do
        local w = {}
        for j = 0, 15 do w[j] = string.unpack(">I4", message, chunk + j * 4) end
        for j = 16, 63 do
            local s0 = rrot(w[j - 15], 7) ~ rrot(w[j - 15], 18) ~ (w[j - 15] >> 3)
            local s1 = rrot(w[j - 2], 17) ~ rrot(w[j - 2], 19) ~ (w[j - 2] >> 10)
            w[j] = (w[j - 16] + s0 + w[j - 7] + s1) & 0xffffffff
        end
        local a, b, c, d, e, f, g, hh = h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8]
        for j = 0, 63 do
            local t1 = (hh + (rrot(e, 6) ~ rrot(e, 11) ~ rrot(e, 25)) + ((e & f) ~ ((~e) & g)) + K[j + 1] + w[j]) & 0xffffffff
            local t2 = ((rrot(a, 2) ~ rrot(a, 13) ~ rrot(a, 22)) + ((a & b) ~ (a & c) ~ (b & c))) & 0xffffffff
            hh, g, f, e, d, c, b, a = g, f, e, (d + t1) & 0xffffffff, c, b, a, (t1 + t2) & 0xffffffff
        end
        h[1], h[2], h[3], h[4] = (h[1] + a) & 0xffffffff, (h[2] + b) & 0xffffffff, (h[3] + c) & 0xffffffff, (h[4] + d) & 0xffffffff
        h[5], h[6], h[7], h[8] = (h[5] + e) & 0xffffffff, (h[6] + f) & 0xffffffff, (h[7] + g) & 0xffffffff, (h[8] + hh) & 0xffffffff
    end
    return string.pack(">I4I4I4I4I4I4I4I4", h[1], h[2], h[3], h[4], h[5], h[6], h[7], h[8])
end

local function hmac(key, message)
    if #key > 64 then key = sha256(key) end
    key = key .. string.rep("\0", 64 - #key)
    local inner, outer = {}, {}
    for i = 1, 64 do
        inner[i] = string.char(key:byte(i) ~ 0x36)
        outer[i] = string.char(key:byte(i) ~ 0x5c)
    end
    return sha256(table.concat(outer) .. sha256(table.concat(inner) .. message))
end

local function hex(bytes) return (bytes:gsub(".", function(ch) return string.format("%02x", ch:byte()) end)) end

-- ---------------------------------------------------------------- this program's id and secret
local function load_id()
    local f = io.open(ID_FILE, "r")
    local id = f and f:read("l")
    if f then f:close() end
    if id and id:match("^%x%x%x%x%x%x$") then return id end
    math.randomseed(os.time() + math.floor(os.clock() * 1e6))
    id = string.format("%06x", math.random(0, 0xffffff))
    f = io.open(ID_FILE, "w")
    if f then f:write(id, "\n"); f:close() end
    return id
end
local ME = load_id()

local function load_key()
    local f = io.open(KEY_FILE, "r")
    if not f then return nil end
    local key = (f:read("a") or ""):gsub("%s+$", "")
    f:close()
    return key ~= "" and key or nil
end
local KEY = load_key()

-- 16 hex digits of the HMAC: enough to make forging hopeless over a chat
local function signature(data) return KEY and hex(hmac(KEY, data)):sub(1, 16) or "-" end

-- ---------------------------------------------------------------- the network as bytes (protocol N2)
local HEADER = ">c2BI4I4c6I2f"   -- "N2", protocol, architecture fingerprint, lessons, origin, count, scale

local function fingerprint(arch) return tonumber(checksum(arch or ""), 16) end

local function pack_network(net)
    local scale = 1e-9
    for _, v in ipairs(net.w) do scale = math.max(scale, math.abs(v)) end
    local parts = { string.pack(HEADER, "N2", PROTOCOL, fingerprint(net.arch), net.samples or 0, ME, #net.w, scale) }
    for _, v in ipairs(net.w) do parts[#parts + 1] = string.pack(">i2", math.floor(v / scale * 32767 + 0.5)) end
    return table.concat(parts)
end

-- nil and why when it is not a network this program can take
local function unpack_network(data, local_arch)
    if #data < string.packsize(HEADER) or data:sub(1, 2) ~= "N2" then return nil, "not_N2" end
    local _, protocol, arch, samples, origin, count, scale, pos = string.unpack(HEADER, data)
    if protocol ~= PROTOCOL then return nil, "protocol_" .. protocol end
    if local_arch and arch ~= fingerprint(local_arch) then return nil, "other_architecture" end
    if scale ~= scale or scale == math.huge or scale <= 0 then return nil, "not_finite" end
    if #data ~= pos - 1 + 2 * count then return nil, "truncated" end
    local w = {}
    for i = 1, count do
        local q
        q, pos = string.unpack(">i2", data, pos)
        w[i] = q / 32767 * scale
    end
    return { w = w, samples = samples, origin = origin }
end

-- ---------------------------------------------------------------- the transport
local outbox = {}        -- lines waiting to go out, one every 2 seconds
local sent = {}          -- msg -> { at, segments = { seq -> line } }
local pending = {}       -- from .. " " .. msg -> { from, msg, total, chk, sig, parts, first, asked, times }
local done = {}          -- from .. " " .. msg -> when: a message already delivered is not delivered again
local peers = {}         -- from -> { last_seen, received, merged, rejected }
local counters = { sent_messages = 0, sent_lines = 0, received_messages = 0, bad_checksum = 0, bad_signature = 0,
    unsigned = 0, rejected = 0, resent = 0, acks = 0, dropped_lines = 0 }
local acks = {}          -- msg -> { from -> result }
local next_msg = 0

local function queue(line)
    outbox[#outbox + 1] = line
    while #outbox > OUTBOX_MAX do
        table.remove(outbox, 1)
        counters.dropped_lines = counters.dropped_lines + 1
    end
end

local function speaker()
    local bots = call("bots", {}) or {}
    return bots[1] and bots[1].name or nil
end

local function send(data)
    next_msg = next_msg + 1
    local msg = string.format("%x", (os.time() * 16 + next_msg) % 0xfffff)
    local text, chk, sig = b64encode(data), checksum(data), signature(data)
    local total = math.ceil(#text / SEGMENT)
    local segments = {}
    for seq = 1, total do
        segments[seq] = string.format("[nn] %s %s %d/%d %s %s %s", ME, msg, seq, total, chk, sig, text:sub((seq - 1) * SEGMENT + 1, seq * SEGMENT))
        queue(segments[seq])
    end
    sent[msg] = { at = os.time(), segments = segments }
    counters.sent_messages = counters.sent_messages + 1
    return msg, total
end

-- A whole message: checked, handed to the brain, and what became of it acknowledged
local function deliver(p)
    local data = b64decode(table.concat(p.parts))
    local result
    if not data or checksum(data) ~= p.chk then
        counters.bad_checksum = counters.bad_checksum + 1
        result = "rejected:checksum"
    elseif KEY and p.sig ~= signature(data) then
        counters.bad_signature = counters.bad_signature + 1
        result = "rejected:signature"
    else
        if not KEY then counters.unsigned = counters.unsigned + 1 end
        counters.received_messages = counters.received_messages + 1
        peers[p.from].received = (peers[p.from].received or 0) + 1
        local mine = call("brain", { export = true })
        local net, why = unpack_network(data, type(mine) == "table" and mine.arch or nil)
        if not net then
            result = "rejected:" .. why
        elseif net.origin ~= p.from then
            result = "rejected:origin"
        else
            local r, err = call("brain", { merge = { w = net.w, samples = net.samples, origin = net.origin, arch = mine.arch } })
            result = r and "ok" or ("rejected:" .. ((err or ""):match("rejected:(%S+)") or "brain"))
        end
    end
    if result == "ok" then
        peers[p.from].merged = (peers[p.from].merged or 0) + 1
    else
        counters.rejected = counters.rejected + 1
        peers[p.from].rejected = (peers[p.from].rejected or 0) + 1
    end
    queue(string.format("[nn] %s ACK %s %s %s", ME, p.from, p.msg, result))
end

-- Any line with "[nn] " in it, whatever the server puts before it ("<name> ", "name: ", ranks...)
bot.on_chat(function(line)
    local rest = line:match("%[nn%] (.+)$")
    if not rest then return end
    local words = {}
    for w in rest:gmatch("%S+") do words[#words + 1] = w end
    local from = words[1]
    if not from or not from:match("^%x%x%x%x%x%x$") or from == ME then return end
    peers[from] = peers[from] or {}
    peers[from].last_seen = os.time()
    if words[2] == "ACK" then
        -- ACK <to> <msg> <result>
        if words[3] == ME then
            counters.acks = counters.acks + 1
            acks[words[4] or "?"] = acks[words[4] or "?"] or {}
            acks[words[4] or "?"][from] = words[5] or "?"
        end
        return
    end
    if words[2] == "NACK" then
        -- NACK <to> <msg> <missing segments>
        if words[3] ~= ME or not sent[words[4] or ""] then return end
        for seq in (words[5] or ""):gmatch("%d+") do
            local segment = sent[words[4]].segments[tonumber(seq)]
            if segment then queue(segment); counters.resent = counters.resent + 1 end
        end
        return
    end
    -- A segment: <msg> <seq>/<total> <checksum> <signature> <data>
    local msg, position, chk, sig, data = words[2], words[3], words[4], words[5], words[6]
    if not (msg and position and chk and sig and data) then return end
    local seq, total = position:match("^(%d+)/(%d+)$")
    seq, total = tonumber(seq), tonumber(total)
    if not seq or not total or total > 64 or seq < 1 or seq > total then return end
    local key = from .. " " .. msg
    if done[key] then return end
    local p = pending[key]
    if not p then
        p = { from = from, msg = msg, total = total, chk = chk, sig = sig, parts = {}, first = os.time(), count = 0, times = 0 }
        pending[key] = p
    end
    if not p.parts[seq] then p.parts[seq] = data; p.count = p.count + 1 end
    if p.count == p.total then
        pending[key] = nil
        done[key] = os.time()
        deliver(p)
    end
end)

-- The pump: one line out every 2 seconds; missing segments asked for again (3 times); old ones forgotten
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
        if now - (p.asked or p.first) > ASK_AFTER then
            if p.times >= ASK_TIMES then
                pending[key] = nil          -- given up on
            else
                local missing = {}
                for seq = 1, p.total do if not p.parts[seq] then missing[#missing + 1] = seq end end
                queue(string.format("[nn] %s NACK %s %s %s", ME, p.from, p.msg, table.concat(missing, ",")))
                p.asked, p.times = now, p.times + 1
            end
        end
    end
    for msg, s in pairs(sent) do
        if now - s.at > KEEP then sent[msg] = nil end
    end
    for key, at in pairs(done) do
        if now - at > 3600 then done[key] = nil end
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
    description = "Learning shared with bots run by other programs (other computers) on the same server, over the game chat (protocol N2): the brain's network is sent every 10 minutes when it learnt something (16-bit weights, architecture fingerprint, lesson count and sender id; base64 in numbered, checksummed segments, signed with HMAC-SHA256 when netlink_key.txt holds a shared secret). Receivers check it all, acknowledge with the result (ok or rejected:why), ask again for missing segments, and the brain merges it on trial. With no argument: this program's id, whether it signs, the peers seen, counters and the acknowledgements of its last messages. send=true sends the network now.",
    params = { send = "boolean" },
    run = function(args)
        if args.send then
            last_samples = -1
            local msg, total = share()
            return msg and { message = msg, segments = total } or "the brain has no network to send"
        end
        local list = {}
        for id, p in pairs(peers) do
            list[id] = { seconds_ago = os.time() - (p.last_seen or 0), received = p.received or 0, merged = p.merged or 0, rejected = p.rejected or 0 }
        end
        return { id = ME, signed = KEY ~= nil, peers = list, counters = counters, waiting_lines = #outbox, acks = acks }
    end,
}

-- For the offline check
return { b64encode = b64encode, b64decode = b64decode, checksum = checksum, sha256 = sha256, hmac = hmac, hex = hex,
    pack = pack_network, unpack = unpack_network, id = ME }
