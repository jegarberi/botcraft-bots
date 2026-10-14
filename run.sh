#!/usr/bin/env bash
# Build the bot (only what changed) and start it.
#   ./run.sh                      window + control panel (http://127.0.0.1:8090) + decision tree on
#   ./run.sh --headless [opts]    no window, no views, no stdin: a service with the control panel, stops on Ctrl-C/SIGTERM
#   ./run.sh --tui [opts]         headless, with a terminal control panel; q stores the items and stops the bots
#                                 (python3 bot/tui.py URL attaches to a program already running, over its web port)
#   ./run.sh --bots 3 --delay 10  any bot_mcp options replace the defaults (see ./run.sh --help)
# Build output goes to stderr, so this script can also be used as the MCP server command.
set -euo pipefail
cd "$(dirname "$0")"
# Botcraft at the commit the bot is built against, with our fixes (patches/botcraft.diff)
if [ ! -d Botcraft ]; then
    git clone https://github.com/adepierre/Botcraft.git Botcraft >&2
    git -C Botcraft checkout -q 3e54657a3ccd250657c887baa46a0e43cb0515bf
    git -C Botcraft apply ../patches/botcraft.diff
fi
cd bot

[ -f build/build.ninja ] || cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release >&2
cmake --build build >&2
# Recipes and item tags come from the game's own data, inside the client jar Botcraft downloaded
[ -d data/data/minecraft/recipe ] || unzip -o -q ../Botcraft/Assets/26.3/client.jar 'data/minecraft/recipe/*' 'data/minecraft/tags/item/*' -d data >&2

mode=${1:-}
case "$mode" in
    --headless|--tui)
        shift
        # The panel's port: the one given, else 8090
        port=8090
        args=("$@")
        for ((i = 0; i < ${#args[@]}; i++)); do [ "${args[i]}" = --web ] && port=${args[i+1]}; done
        [[ " $* " == *" --web "* ]] || set -- --web "$port" "$@"
        [ ${#args[@]} -gt 0 ] || set -- "$@" --autopilot --chests
        if [ "$mode" = --headless ]; then
            echo "Control panel: http://127.0.0.1:$port  (Ctrl-C to store items and quit)" >&2
            exec bin/bot_mcp --headless "$@"
        fi
        # The panel runs inside the program and talks to each bot through its socket (bot/run/<name>.sock)
        echo "Logs: bot/run.log" >&2
        exec bin/bot_mcp --headless --tui "$@"
        ;;
    "")
        set -- --gui --autopilot --chests --web 8090 --snapshot snapshot.png
        echo "Control panel: http://127.0.0.1:8090  (Ctrl-D to store items and quit)" >&2
        exec bin/bot_mcp "$@"
        ;;
    *)
        exec bin/bot_mcp "$@"
        ;;
esac
