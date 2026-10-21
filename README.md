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

The web panel has a “Spawn additional bots” count; in the terminal panel press `C` and enter a count and base name.
Both queue 1–100 additional bots with unused numbered names (`Worker1`, `Worker2`, … for base `Worker`) and the configured delay between joins.

The terminal panel is its own program, `bot/bin/bot_tui`: open it from any terminal while the bots run. It talks
to the program and to each bot over unix sockets (`bot/run/.program.sock`, `bot/run/<name>.sock`, JSON-RPC lines
as on stdin), so it can come and go without touching the bots; `Q` in it stops the program.

The first run clones Botcraft at the pinned commit, applies `patches/botcraft.diff` (protocol and stability
fixes, pathfinding changes) and extracts recipes from the game jar Botcraft downloads.

## Panels

The terminal panel (`bot_tui`): every bot's state, health, food, position and what it is doing; the picked bot's
gear, inventory, goal and decision flow below. Keys on the top line (`n` shows the bot's neural network).
Press Enter for actions: build the castle, roads to mines, crop farms or a shelter, plant castle farms,
stock animal pens, or mine ores. Gather and Mine open a resource list with a custom-ID option.
The web Action dropdown has the same jobs and a resource selector. Castle construction needs its centre
and floor level (`x y z`); roads need a saved castle site and mine entrances. Castle and road jobs run once
for the whole team; bot actions use the selected bot or the all-bots target.

![Terminal panel](docs/tui.png)

The web panel (http://127.0.0.1:8090): team orders, and per bot its controls, equipment, inventory, what it
remembers seeing, and its decision tree with the branch it is on.

![Web panel](docs/web.png)

## Layout

- `bot/mcp.cpp` the bots, their decision tree and the MCP tools; `bot/tui.cpp` the terminal panel (`bin/bot_tui`, over unix sockets)
- `bot/plugins/` Lua plugins: `castle` (the castle, roads, road builder, explorer), `groups` (bakers, farmers, butchers, blacksmiths,
  miners, builders), `players` (orders from players in the chat), `berries`, `example`, `empire` (optional, off until
  `empire {start=true}`: an Age of Empires II style computer player that pays for ages and techs with what the bots
  gather, splits them over the trades by age and need instead of the brain, and sends the soldiers out in waves;
  `./run.sh --empire` plays from the start, `./run.sh --village` too, a house for every bot first, then its trade)
- `bot/web/` the web panel
- `bot/test_*` offline checks of the plugins (`lua5.4 test_castle.lua plugins/castle.lua`, ...)

## Where the bots keep what they know

Each bot's memory (places it saw, its own bed and field, what it was doing) and what the bots share (finds,
chests and their contents, building sites, mines, trades) go to the store `BOTCRAFT_DB` names:

| `BOTCRAFT_DB` | Store |
|---|---|
| unset (or `sqlite`) | SQLite files: `bot/memory/<bot>.sqlite` and `bot/botcraft.sqlite` (the default) |
| `postgresql://user:password@host:5432/db` | PostgreSQL: one set of tables, the memories keyed by bot name (created on first use) |
| `valkey://[:password@]host:6379[/db]` | Valkey (or Redis, also `redis://`): hashes and sets under `bc:` |

`BOTCRAFT_DB_POOL` sets how many connections the program opens to a PostgreSQL or Valkey server (default 8). The
two are built in when libpq and hiredis are installed at build time. Nothing is copied from one store to another:
a new store starts empty: the bots learn their surroundings again within minutes and the groups plugin hands the
trades out again from its own saved state, but building sites, mines and what the chests hold start over. `bot/test_store.sh` runs the same checks on all three, on a throwaway PostgreSQL and Valkey.

## Basics first

A bot that joins covers its basics before anything else, in this order: a place for the night (a bed, or a 3×3
hut it builds of dirt), wood (8 planks' worth), a crafting table, a furnace (8 cobblestone), wheat seeds (from grass), water by soil (or a
bucket to make some), its own wheat field, a composter by the field, then bread from its wheat. A bot deep
underground digs up to the surface first. Meanwhile the plugins' work waits: their tool calls for it are refused,
and its trade's network skips its turns. A step that fails is tried again 5 minutes later; meanwhile the bot gets
on with other work. Once they are covered, it goes back to its field to harvest and bake whenever the wheat is
ripe. At night, a bot with no bed closes itself into its hut. Each harvest plants at least one more crop than
was cut (grass is broken for seeds when short), and saplings and seeds beyond 32 go in the composter, for bone meal.
`status` shows `basics` while a bot is on them.

## Neural policies

The full description (architecture, inputs, rewards and penalties, learning, evolution, sharing) is in
[docs/neural-network.md](docs/neural-network.md).

Each bot has its own trade-selection network (116 inputs, five hidden layers of 10 units) and its own
action policy for each trade (104 inputs and five hidden layers of 8 units), including whether a build is active, its remaining blocks,
and how long it has been idle. Both use actor-critic learning and residual hidden layers; old saved
weights migrate with neutral added layers, and new actions are appended to preserve existing action indices.
Both input layers include cyclical time of day, rain/thunder strength and one-hot biome categories from
Java 26.3 (67 biomes plus unknown for unloaded/custom biomes). These inputs are appended with zero weights
when loading old policies, so existing learned behavior is preserved. Weather and biome are also in `status`.
Builders and miners can gather construction stone and logs, builders and blacksmiths can make storage
chests, bakers can cook available food, and blacksmiths can make and distribute building axes.
The web and terminal network views show all three layers. Peer learning requires the same architecture.
Both policies subtract 1 point per 10 seconds spent idle with autopilot enabled, measured as
`stats.idle_seconds`. Sleeping, working in place and manually paused bots do not count.
Both policies also subtract construction defects from their rewards: 10 points per one-block-wide
mine slice or road cell, 10 per newly placed chest facing differently from an adjacent chest,
and twice the plan's block count for a completed roofed building with no walkable entrance.
Access checks include stairs and doors; unloaded surroundings and plans beyond the audit's
128×128×64 / 262,144-cell limit are skipped. Defect counters appear in each bot's `stats`.

Every five game days, the two policies with the best average reward are kept unchanged; the others receive
independent copies of either parent with small mutations (Gaussian noise, standard deviation 0.05).
Action policies compete within the same trade, since different trades have different actions. Their evolution
continues in village/empire mode, which controls trade assignments while enabled. Old shared action policies
seed individual policies without discarding their learned weights.

The empire and village modes are described in full in [docs/empire-village.md](docs/empire-village.md).
The plan of every building the bots put up (castle, town, village, farms, mines) is in
[docs/buildings.md](docs/buildings.md), drawn from the plugins' own block lists by `bot/gen_plans.lua`.
Village construction reserves supply workers, prioritizes feeding hungry teammates, and allows shops to start
as their own builders finish their homes. Roads share saved elevations and use stone stairs on slopes, with a
longer approach when needed. Unloaded terrain waits for a survey; failed placements remain pending and retry.
Completion requires the named build to be verified complete, rather than merely idle.
