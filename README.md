# Botcraft bots

A team of Minecraft bots built on [Botcraft](https://github.com/adepierre/Botcraft) (C++), controlled over MCP
(stdio and HTTP), a web panel, a terminal panel and Lua plugins. Each bot plays on its own: it gears up, eats,
fights, gathers, farms, breeds, mines (wide lit mines with stairs), stores what it collects in chests, and
remembers what it was doing across restarts. Together they build a castle (see `bot/castle_preview.png`).

## Run

    ./run.sh                 # window + web panel on http://127.0.0.1:8090 (MCP at /mcp)
    ./run.sh --headless      # no window; Ctrl-C or SIGTERM: the bots store their items and leave
    ./run.sh --tui           # headless, with a terminal panel
    ./run.sh --bots 15 --delay 8 --autopilot --chests --bind 0.0.0.0

The first run clones Botcraft at the pinned commit, applies `patches/botcraft.diff` (protocol and stability
fixes, pathfinding changes) and extracts recipes from the game jar Botcraft downloads.

## Layout

- `bot/mcp.cpp` the bots, their decision tree and the MCP tools; `bot/tui.cpp` the terminal panel
- `bot/plugins/` Lua plugins: `castle` (the castle, roads, road builder), `groups` (hunters, farmers, breeders,
  miners, builders), `players` (orders from players in the chat), `berries`, `example`
- `bot/web/` the web panel
- `bot/test_*` offline checks of the plugins (`lua5.4 test_castle.lua plugins/castle.lua`, ...)
