-- Example plugin: one new tool for the LLM, one chat command for players.

-- A tool is a function of its arguments. bot.call() runs any other tool and raises on failure.
bot.tool{
    name = "pillar",
    description = "Stack `height` blocks of `item` under the bot's feet position, one column next to it.",
    params = { item = "string", height = "number" },
    run = function(args)
        local pos = bot.call("status").position
        local x, y, z = math.floor(pos.x) + 1, math.floor(pos.y), math.floor(pos.z)
        for i = 0, (args.height or 3) - 1 do
            bot.call("place_block", { item = args.item, x = x, y = y + i, z = z })
        end
        return "pillar built at " .. x .. " " .. y .. " " .. z
    end,
}

-- Chat lines look like "<Player> text". Say "bot come" in game and the bot walks to the nearest player.
bot.on_chat(function(line)
    if line:match("^<[^>]+> bot come$") then
        local players = bot.call("entities", { type = "player", radius = 64 })
        if players[1] then
            local p = players[1].position
            bot.call("goto", { x = p.x, y = p.y, z = p.z, tolerance = 2 })
        else
            bot.call("chat", { message = "I can't see you" })
        end
    end
end)
