-- Java 26.3: every bot proposes a site; creative bots build the agreed castle, village and planted farms.
-- Cross-host voting uses signed, compressed netlink chat. Without a key the council is local to this host.
local loaded = os.time()
local ballots, acknowledgements = {}, {}
local local_vote, changed, last_sent, registered = nil, loaded, 0, false
local decision, approved, started, announced = nil, nil, false, false
local site_sent
local note = "waiting for bots to join"
local function call(tool, args)
    local ok, result = pcall(bot.call, tool, args or {})
    return ok and result or nil
end
local function ground(name, x, z, y)
    for h = y + 8, y - 16, -1 do
        local b = call("get_block", {bot = name, x = x, y = h, z = z})
        if not b or not b.block then return nil end
        local block = b.block
        if block ~= "minecraft:air" and block ~= "minecraft:cave_air" and not block:match("leaves$")
            and not block:match("_log$") and not block:match("grass$") and not block:match("fern$")
            and not block:match("flower") and block ~= "minecraft:snow" then return h, block end
    end
end
local function propose(name, st)
    local p = st.position
    local x, z, y = math.floor(p.x), math.floor(p.z), math.floor(p.y)
    local floor = ground(name, x, z, y)
    if not floor or floor + 1 < -48 or floor + 1 > 270 or math.abs(x) > 29999000 or math.abs(z) > 29999000 then return nil end
    local low, high, unseen, wet = floor, floor, 0, 0
    for dx = -128, 128, 128 do for dz = -128, 128, 128 do
        local h, block = ground(name, x + dx, z + dz, floor)
        if not h then unseen = unseen + 1
        else
            low, high = math.min(low, h), math.max(high, h)
            if block == "minecraft:water" or block == "minecraft:lava" or block:match("ice$") then wet = wet + 1 end
        end
    end end
    return {name = name, creative = st.game_mode == "creative", x = x, y = floor + 1, z = z,
        score = unseen * 10000 + wet * 1000 + (high - low) * 100}
end
local function encode(rows)
    local out = {}
    for _, r in ipairs(rows) do out[#out + 1] = string.pack(">s1Bi4i2i4I4", r.name, r.creative and 1 or 0, r.x, r.y, r.z, r.score) end
    return table.concat(out)
end
local function decode(raw)
    local rows, seen, pos = {}, {}, 1
    while pos <= #raw do
        local name, creative, x, y, z, score
        name, creative, x, y, z, score, pos = string.unpack(">s1Bi4i2i4I4", raw, pos)
        if #rows >= 100 or not name:match("^[%w_]+$") or #name > 16 or seen[name] or creative > 1
            or math.abs(x) > 29999000 or math.abs(z) > 29999000 or y < -48 or y > 270 or score > 1000000 then error("invalid ballot") end
        rows[#rows + 1] = {name = name, creative = creative == 1, x = x, y = y, z = z, score = score}
        seen[name] = true
    end
    return rows
end
local function election()
    local hosts, candidates, has_creative, canonical = {}, {}, false, {}
    for id in pairs(ballots) do hosts[#hosts + 1] = id end
    table.sort(hosts)
    for _, id in ipairs(hosts) do
        local ballot = ballots[id]
        canonical[#canonical + 1] = id .. string.pack(">I2", #ballot.raw) .. ballot.raw
        for _, r in ipairs(ballot.rows) do
            candidates[#candidates + 1] = r
            has_creative = has_creative or r.creative
        end
    end
    if not has_creative then return nil end
    local winner, best
    for _, candidate in ipairs(candidates) do
        local score = candidate.score
        for _, voter in ipairs(candidates) do score = score + (math.abs(voter.x - candidate.x) + math.abs(voter.z - candidate.z)) / 64 end
        if not best or score < best then winner, best = candidate, score end
    end
    return {host = hosts[1], hosts = hosts, site = winner, hash = NETLINK_BUILD.checksum(table.concat(canonical)), voters = #candidates}
end
local function invalidate()
    changed, approved, acknowledgements = os.time(), nil, {}
end
local function receive(from, raw)
    if #raw > 5000 then return "rejected:ballot_size" end
    local kind = raw:sub(1, 1)
    -- S: the site of a team already building. A host still undecided joins it (a decided one keeps its own:
    -- two teams can't pull each other back and forth)
    if kind == "S" then
        if decision then return "ok:already_decided" end
        if #raw ~= 13 then return "rejected:payload" end
        local x, y, z, voters = string.unpack(">i4i2i4I2", raw, 2)
        if math.abs(x) > 29999000 or math.abs(z) > 29999000 or y < -48 or y > 270 then return "rejected:payload" end
        decision = {host = from, hosts = {from}, site = {name = "team", creative = true, x = x, y = y, z = z}, voters = voters, joined = true}
        note = "joined the team of host " .. from
        return "ok"
    end
    if kind == "V" then
        local content = raw:sub(2)
        if decision and (not ballots[from] or ballots[from].raw ~= content) then return "rejected:site_already_decided" end
        local rows = decode(content)
        if #rows == 0 then return "rejected:empty" end
        local count = 0 for _ in pairs(ballots) do count = count + 1 end
        if not ballots[from] and count >= 32 then return "rejected:capacity" end
        if not ballots[from] or ballots[from].raw ~= content then invalidate() end
        ballots[from] = {raw = content, rows = rows, at = os.time()}
        return "ok"
    end
    local current = election()
    if not current or raw:sub(2) ~= current.hash then return "rejected:vote_set" end
    if kind == "D" and from == current.host then
        approved = current.hash
        NETLINK_BUILD.send("A" .. approved)
        return "ok"
    elseif kind == "A" and ballots[from] and current.host == NETLINK_BUILD.id then
        acknowledgements[from] = current.hash
        return "ok"
    elseif kind == "C" and from == current.host and approved == current.hash then
        decision = current
        return "ok"
    end
    return "rejected:authority"
end
local function build()
    if started or not decision then return end
    local castle = call("castle", {action = "status"})
    if not castle then return end
    if castle.building or castle.site then started = true; note = "existing castle retained"; return end
    local builder
    for _, b in ipairs(call("bots") or {}) do
        local st = call("status", {bot = b.name})
        if st and st.game_mode == "creative" and st.dimension == "minecraft:overworld" then builder = b.name; break end
    end
    if not builder then note = "site agreed; waiting for a creative builder"; return end
    local p = decision.site
    local result = call("castle", {action = "start", bot = builder, creative = true, x = p.x, y = p.y, z = p.z})
    started = type(result) == "table" and result.started or false
    note = started and "building castle, village and planted farms" or "castle could not start; retrying"
end
bot.every(20, function()
    if not NETLINK_BUILD or MIGRATING or os.time() - loaded < 120 then return end
    if not registered then NETLINK_BUILD.listen(receive); registered = true end
    local id, now = NETLINK_BUILD.id, os.time()
    -- A castle already standing here (the vote was before a restart): that is the team's site, no new vote
    if not decision then
        local castle = call("castle", {action = "status"})
        local site = castle and castle.site
        if site and site.x and site.z then
            decision = {host = id, hosts = {id}, site = {name = "castle", creative = true, x = site.x, y = site.y or 70, z = site.z},
                voters = #(call("bots") or {}), existing = true}
            announced = true
        end
    end
    -- Every minute: the team's site to the other hosts, so one connecting later joins it instead of voting alone
    if decision and NETLINK_BUILD.signed and now - (site_sent or 0) >= 60 then
        local p = decision.site
        NETLINK_BUILD.send("S" .. string.pack(">i4i2i4I2", p.x, p.y, p.z, math.min(decision.voters or 0, 65535)))
        site_sent = now
    end
    if not local_vote then
        local rows, roster = {}, call("bots") or {}
        for _, b in ipairs(roster) do
            local st = call("status", {bot = b.name})
            -- Every bot in the overworld votes, including survival bots.
            if not st or not st.position then return end
            if st.dimension == "minecraft:overworld" then
                local proposal = propose(b.name, st)
                if not proposal then note = "waiting for loaded terrain for " .. b.name; return end
                rows[#rows + 1] = proposal
            end
        end
        if #rows == 0 then return end
        table.sort(rows, function(a, b) return a.name < b.name end)
        local_vote = encode(rows)
        ballots[id] = {raw = local_vote, rows = rows, at = now}
        invalidate()
    end
    -- Include bots that joined during the voting window; keep existing proposals fixed so votes converge.
    if not decision then
        local previous, rows = {}, {}
        for _, r in ipairs(ballots[id].rows) do previous[r.name] = r end
        for _, b in ipairs(call("bots") or {}) do
            local st = call("status", {bot = b.name})
            if not st or not st.position then return end
            if st.dimension == "minecraft:overworld" then
                local row = previous[b.name] or propose(b.name, st)
                if not row then return end
                row.creative = st.game_mode == "creative"
                rows[#rows + 1] = row
            end
        end
        table.sort(rows, function(a, b) return a.name < b.name end)
        local content = encode(rows)
        if content ~= local_vote then
            local_vote = content
            ballots[id] = {raw = content, rows = rows, at = now}
            invalidate()
        end
    end
    ballots[id].at = now
    for host, ballot in pairs(ballots) do
        if host ~= id and now - ballot.at > 240 then ballots[host] = nil; invalidate() end
    end
    local current = election()
    if now - last_sent >= 60 then
        if NETLINK_BUILD.signed then NETLINK_BUILD.send("V" .. local_vote) end
        -- (Once decided, no more D/C: sent every minute for ever, every other host answers rejected:authority)
        if current and current.host == id and not decision and now - changed >= 120 then
            approved = current.hash
            acknowledgements[id] = current.hash
            local all = true
            for _, host in ipairs(current.hosts) do if acknowledgements[host] ~= current.hash then all = false end end
            if all then
                decision = current
                if NETLINK_BUILD.signed then NETLINK_BUILD.send("C" .. current.hash) end
            elseif NETLINK_BUILD.signed then NETLINK_BUILD.send("D" .. current.hash) end
        end
        last_sent = now
    end
    if decision and not announced then
        local p = decision.site
        local message = decision.joined and string.format("[build] joining host %s's team: castle, village and farms at %d %d %d (overworld)", decision.host, p.x, p.y, p.z)
            or string.format("[build] %d bots agreed: castle, village and farms at %d %d %d (overworld)", decision.voters, p.x, p.y, p.z)
        if call("chat", {message = message}) then announced = true end
    end
    note = decision and note or "collecting site proposals and host approvals"
    build()
end)
bot.tool{
    name = "creative_build",
    description = "Automatic creative construction council: every overworld bot proposes a location; authenticated hosts approve the same vote set before creative bots build the castle, village and planted farms. Reports the votes, agreed coordinates and current progress. Existing castles are retained.",
    params = {},
    run = function() return {note = note, started = started, decision = decision, ballots = ballots, signed = NETLINK_BUILD and NETLINK_BUILD.signed or false} end,
}
return {encode = encode, decode = decode, election = election}
