# Botcraft bots

A team of Minecraft bots built on [Botcraft](https://github.com/adepierre/Botcraft) (C++), controlled over MCP
(stdio and HTTP), a web panel, a terminal panel and Lua plugins. Each bot plays on its own: it gears up, eats,
fights, gathers, farms, breeds, mines (wide lit mines with stairs), stores what it collects in chests, and
remembers what it was doing across restarts. Together they build a castle (see `bot/castle_preview.png`).

## Run

    ./run.sh                 # window + web panel on http://127.0.0.1:8090 (MCP at /mcp)
    ./run.sh --headless      # no window; Ctrl-C or SIGTERM: the bots store their items and leave
    ./run.sh --tui           # headless in the background, with the terminal panel; q leaves it, the bots keep going
    ./run.sh --headless --tui  # the same, but closing the panel stops the bots (they store their items first)
    ./run.sh --bots 15 --delay 8 --autopilot --chests --bind 0.0.0.0

The terminal panel is its own program, `bot/bin/bot_tui`: open it from any terminal while the bots run. It talks
to the program and to each bot over unix sockets (`bot/run/.program.sock`, `bot/run/<name>.sock`, JSON-RPC lines
as on stdin), so it can come and go without touching the bots; `Q` in it stops the program.

The first run clones Botcraft at the pinned commit, applies `patches/botcraft.diff` (protocol and stability
fixes, pathfinding changes) and extracts recipes from the game jar Botcraft downloads.

## Panels

The terminal panel (`bot_tui`): every bot's state, health, food, position and what it is doing; the picked bot's
gear, inventory, goal and decision flow below. Keys on the top line (`n` shows the bot's neural network).

![Terminal panel](docs/tui.png)

The web panel (http://127.0.0.1:8090): team orders, and per bot its controls, equipment, inventory, what it
remembers seeing, and its decision tree with the branch it is on.

![Web panel](docs/web.png)

## Layout

- `bot/mcp.cpp` the bots, their decision tree and the MCP tools; `bot/tui.cpp` the terminal panel (`bin/bot_tui`, over unix sockets)
- `bot/plugins/` Lua plugins: `castle` (the castle, roads, road builder), `groups` (hunters, farmers, breeders,
  miners, builders), `players` (orders from players in the chat), `berries`, `example`
- `bot/web/` the web panel
- `bot/test_*` offline checks of the plugins (`lua5.4 test_castle.lua plugins/castle.lua`, ...)
