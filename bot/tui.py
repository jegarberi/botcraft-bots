#!/usr/bin/env python3
"""Terminal control panel for bot_mcp, through its web port (start it with --web PORT).

    python3 tui.py [http://127.0.0.1:8090]

Up/Down pick a bot. A toggles actions for all bots. a autopilot on/off, e gear-up on/off, G gather, m make, s store in chests, f farm,
T drop items (keep armor and tools) and wait 10 s, w follow a player, W stop following,
l find a structure and go, d store all and disconnect, D every bot stores all and disconnects, c connect a bot, t team order, x disband the team,
: any tool call (name {json args}, the picked bot is added), q quit (the bots keep running)."""
import curses, json, sys, textwrap, threading, time, urllib.request

URL = (sys.argv[1] if len(sys.argv) > 1 else "http://127.0.0.1:8090").rstrip("/")


def state():
    with urllib.request.urlopen(URL + "/state", timeout=10) as r:
        return json.load(r)


def call(name, args):
    body = json.dumps({"name": name, "arguments": args}).encode()
    request = urllib.request.Request(URL + "/call", body, {"X-Bot": "1", "Content-Type": "application/json"})
    # Tool calls can take minutes (walking, crafting): the panel waits, as the web page does
    with urllib.request.urlopen(request, timeout=600) as r:
        r = json.load(r)
    result = r["result"] if isinstance(r["result"], str) else json.dumps(r["result"])
    return name + (": " if r["ok"] else " failed: ") + result


shown = {"message": ""}


def run(name, args):
    """A tool call in the background: some take minutes (walking, crafting), the panel keeps refreshing"""
    if args.get("bot") == "*":
        for bot in state().get("bots", []):
            run(name, {**args, "bot": bot["name"]})
        return
    shown["message"] = name + "…"

    def work():
        try:
            shown["message"] = call(name, args)
        except Exception as e:
            shown["message"] = "%s failed: %s" % (name, e)
    threading.Thread(target=work, daemon=True).start()


def short(position):
    return "%d %d %d" % (position["x"], position["y"], position["z"]) if isinstance(position, dict) else "?"


def items(d):
    return ", ".join("%s %s" % (k, v) for k, v in sorted(d.items(), key=lambda kv: -kv[1] if isinstance(kv[1], int) else 0)) if isinstance(d, dict) else ""


def ask(screen, prompt):
    h, w = screen.getmaxyx()
    screen.move(h - 1, 0)
    screen.clrtoeol()
    screen.addnstr(h - 1, 0, prompt, w - 1, curses.A_BOLD)
    curses.echo()
    curses.curs_set(1)
    screen.timeout(-1)
    try:
        text = screen.getstr(h - 1, min(len(prompt), w - 2), 400).decode(errors="replace").strip()
    finally:
        curses.noecho()
        curses.curs_set(0)
        screen.timeout(250)
    return text


def choose_list(screen, title, options):
    selected = 0
    while True:
        h, w = screen.getmaxyx()
        screen.erase()
        screen.addnstr(0, 0, title + " (up/down, Enter; Esc cancels)", w - 1, curses.A_BOLD)
        visible = max(1, h - 2)
        first = max(0, selected - visible + 1)
        for i in range(first, min(len(options), first + visible)):
            screen.addnstr(i - first + 1, 0, options[i], w - 1, curses.A_REVERSE if i == selected else 0)
        screen.refresh()
        key = screen.getch()
        if key in (27, ord("q")):
            return -1
        if key == curses.KEY_UP:
            selected = max(0, selected - 1)
        elif key == curses.KEY_DOWN:
            selected = min(len(options) - 1, selected + 1)
        elif key in (10, 13, curses.KEY_ENTER):
            return selected


def choose_action(screen, target):
    actions = [("a", "Toggle autopilot"), ("e", "Toggle gear up"), ("G", "Gather"),
               ("m", "Make"), ("s", "Store in chests"), ("f", "Farm"),
               ("l", "Find and go"), ("d", "Store all and disconnect"), (":", "Custom tool"),
               ("B", "Build castle (whole team)"), ("r", "Build roads to mines (whole team)"),
               ("F", "Create crop farm (near water)"), ("M", "Mine ores / open mine as needed"),
               ("H", "Build shelter"), ("V", "Plant castle farms"), ("P", "Stock castle animal pens"),
               ("T", "Drop items (keep armor and tools), wait 10 s"), ("w", "Follow a player"), ("W", "Stop following, back to work"), ("J", "Central coordinator on/off (this host)")]
    selected = choose_list(screen, "Action for " + target, [label for _, label in actions])
    return -1 if selected < 0 else ord(actions[selected][0])


def main(screen):
    curses.curs_set(0)
    curses.use_default_colors()
    screen.timeout(250)
    data, picked, fetched = {"bots": []}, 0, 0.0
    all_bots = False
    while True:
        if time.time() - fetched > 2:
            try:
                data = state()
            except Exception as e:
                shown["message"] = "can't reach %s: %s" % (URL, e)
            fetched = time.time()
        bots = data.get("bots", [])
        picked = max(0, min(picked, len(bots) - 1))
        h, w = screen.getmaxyx()
        screen.erase()
        line = lambda y, text, attr=0: y < h - 1 and screen.addnstr(y, 0, text, w - 1, attr)
        menu = "botcraft  %s  %d bots   Enter action list  ↑↓ pick  a autopilot  e gear  G gather  m make  s store  f farm  T drop  w follow  W unfollow  l find  d disconnect  D all  A target all  J central coordinator  c connect  t team  x disband  : tool  q quit" % (URL, len(bots))
        menu = ("[ALL BOTS]  " if all_bots else "[picked bot]  ") + menu
        menu_lines = textwrap.wrap(menu, max(1, w - 1))
        for y, part in enumerate(menu_lines):
            line(y, part, curses.A_REVERSE)
        line(len(menu_lines), "%-12s %-22s %6s %4s %-15s %s" % ("bot", "state", "health", "food", "position", "doing"), curses.A_BOLD)
        top = len(menu_lines) + 1
        rows = max(3, h - 12 - len(menu_lines) + 1)
        first = max(0, picked - rows + 1)
        for i, b in enumerate(bots[first:first + rows]):
            flags = ("A" if b.get("autopilot") else "-") + ("G" if b.get("gear") else "-")
            text = "%-12s %-22s %6s %4s %-15s %s %s" % (b.get("name", "?")[:12], str(b.get("state", ""))[:22], b.get("health", ""), b.get("food", ""),
                                                      short(b.get("position")), flags, b.get("last_note", ""))
            line(top + i, text, curses.A_REVERSE if first + i == picked else 0)
        if bots:
            b = bots[picked]
            y = top + rows + 1
            equipment = b.get("equipment") or {}
            details = [
                "%s holds %s; worn: %s" % (b.get("name"), b.get("held_item", ""), ", ".join(v for v in equipment.values() if v) or "nothing"),
                "inventory: " + items(b.get("inventory")),
                "goal: %s %s   making: %s   team leader: %s   deaths: %s   stuck: %s" % (b.get("gather", ""), b.get("gather_left", ""), b.get("making", ""),
                                                                               b.get("team_leader", ""), b.get("deaths", 0), b.get("times_stuck", 0)),
                "flow: " + " | ".join(b.get("flow", [])[-6:]) if isinstance(b.get("flow"), list) else "flow: " + str(b.get("flow", "")),
            ]
            for text in details:
                for part in textwrap.wrap(text, max(20, w - 1))[:2]:
                    line(y, part)
                    y += 1
        line(h - 2, shown["message"][: w - 1], curses.A_BOLD)
        screen.refresh()

        key = screen.getch()
        if key in (10, 13, curses.KEY_ENTER):
            key = choose_action(screen, "ALL BOTS" if all_bots else bots[picked]["name"] if bots else "no bot")
        if key == -1:
            continue
        name = ("*" if all_bots else bots[picked]["name"]) if bots else None
        try:
            if key in (ord("q"), 27):
                return
            elif key == curses.KEY_UP:
                picked -= 1
            elif key == curses.KEY_DOWN:
                picked += 1
            elif key == ord("A"):
                all_bots = not all_bots
            elif key == ord("c"):
                who = ask(screen, "connect bot named: ")
                who and run("connect", {"name": who})
            elif key == ord("J"):
                choice = choose_list(screen, "Central coordinator (this host)", ["Switch on", "Switch off"])
                if choice == 0:
                    connected = [b["name"] for b in bots]
                    if not connected:
                        shown["message"] = "no bot connected: connect a bot before enabling the coordinator"
                        continue
                    selected = choose_list(screen, "Which bot will be the coordinator?", connected)
                    if selected >= 0:
                        run("central", {"mode": "leader", "bot": connected[selected]})
                elif choice == 1:
                    run("central", {"mode": "off"})
            elif key == ord("t"):
                order = ask(screen, "team: objective (gather/attack/build/follow) what count [x1 y1 z1 x2 y2 z2]: ").split()
                if order:
                    what = order[1] if len(order) > 1 else ""
                    args = {"objective": order[0], "block": what, "type": what, "item": what, "count": int(order[2]) if len(order) > 2 else 16}
                    args.update(zip(["x1", "y1", "z1", "x2", "y2", "z2"], map(int, order[3:9])))
                    run("team", args)
            elif key == ord("D"):
                if ask(screen, "every bot stores everything and disconnects? (y/n) ").lower().startswith("y"):
                    run("disconnect_all", {})
            elif key == ord("x"):
                run("team", {"objective": "disband"})
            elif name is None:
                shown["message"] = "no bot connected: c to connect one"
            elif key == ord("a"):
                run("autopilot", {"bot": name, "enabled": not bots[picked].get("autopilot")})
            elif key == ord("e"):
                run("autopilot", {"bot": name, "enabled": bool(bots[picked].get("autopilot")), "gear": not bots[picked].get("gear")})
            elif key in (ord("B"), ord("r"), ord("V"), ord("P")):
                args = {"action": {ord("B"): "start", ord("r"): "roads", ord("V"): "farms", ord("P"): "ranch"}[key]}
                if key == ord("B"):
                    at = ask(screen, "castle centre x y z (floor level): ").split()
                    if len(at) != 3:
                        shown["message"] = "enter three integer coordinates"
                        continue
                    args.update(zip(["x", "y", "z"], map(int, at)))
                run("castle", args)
            elif key == ord("F"):
                run("make_farm", {"bot": name, "radius": 4})
            elif key == ord("H"):
                at = bots[picked].get("position")
                if at:
                    run("team_member", {"bot": name, "objective": "build", "plan": "shelter", "item": "cobblestone",
                                        "x1": int(at["x"]), "y1": int(at["y"]), "z1": int(at["z"])})
            elif key in (ord("G"), ord("M")):
                resources = ["coal_ore", "iron_ore", "copper_ore", "gold_ore", "diamond_ore", "redstone_ore", "lapis_ore", "emerald_ore", "iron_ore,coal_ore"]
                if key == ord("G"):
                    resources += ["oak_log", "spruce_log", "birch_log", "cobblestone", "stone", "sand", "gravel", "dirt"]
                selected = choose_list(screen, "Resource", resources + ["Custom block ids..."])
                if selected < 0:
                    continue
                resource = ask(screen, "block ids, comma separated: ") if selected == len(resources) else resources[selected]
                if resource:
                    count = int(ask(screen, "count (default 16): ") or "16")
                    if count < 1:
                        raise ValueError("count must be positive")
                    run("autopilot", {"bot": name, "enabled": True, "gather": resource, "count": count})
            elif key == ord("m"):
                what = ask(screen, "make (item) and count: ").split()
                if what:
                    run("make", {"bot": name, "item": what[0], "count": int(what[1]) if len(what) > 1 else 1})
            elif key == ord("s"):
                run("store", {"bot": name})
            elif key == ord("f"):
                run("farm", {"bot": name})
            elif key == ord("l"):
                what = ask(screen, "find and go to (structure, e.g. village_plains, mineshaft): ")
                if what:
                    run("locate", {"bot": name, "kind": "structure", "name": what, "go": True})
            elif key == ord("T"):
                run("drop", {"bot": name})
            elif key == ord("w"):
                player = ask(screen, "follow player named: ")
                if player:
                    run("team_member", {"bot": name, "objective": "follow", "members": [], "leader": player, "count": 1, "solo": True})
            elif key == ord("W"):
                run("team_member", {"bot": name, "objective": "disband", "members": [], "leader": "", "count": 1, "solo": False})
            elif key == ord("d"):
                if ask(screen, "store everything and disconnect %s? (y/n) " % name).lower().startswith("y"):
                    run("disconnect", {"bot": name})
            elif key == ord(":"):
                text = ask(screen, "tool {json args} (for %s): " % name)
                if text:
                    tool, _, rest = text.partition(" ")
                    args = json.loads(rest) if rest.strip() else {}
                    args.setdefault("bot", name)
                    run(tool, args)
            fetched = 0.0
        except Exception as e:
            shown["message"] = "error: %s" % e


if __name__ == "__main__":
    curses.wrapper(main)
