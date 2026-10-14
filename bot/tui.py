#!/usr/bin/env python3
"""Terminal control panel for bot_mcp, through its web port (start it with --web PORT).

    python3 tui.py [http://127.0.0.1:8090]

Up/Down pick a bot. a autopilot on/off, e gear-up on/off, G gather, m make, s store in chests, f farm,
l find a structure and go, d store all and disconnect, c connect a bot, t team order, x disband the team,
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


def main(screen):
    curses.curs_set(0)
    curses.use_default_colors()
    screen.timeout(250)
    data, picked, fetched = {"bots": []}, 0, 0.0
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
        line(0, "botcraft  %s  %d bots   ↑↓ pick  a autopilot  e gear  G gather  m make  s store  f farm  l find  d disconnect  c connect  t team  x disband  : tool  q quit"
             % (URL, len(bots)), curses.A_REVERSE)
        line(1, "%-12s %-22s %6s %4s %-15s %s" % ("bot", "state", "health", "food", "position", "doing"), curses.A_BOLD)
        top = 2
        rows = max(3, h - 12)
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
        if key == -1:
            continue
        name = bots[picked]["name"] if bots else None
        try:
            if key in (ord("q"), 27):
                return
            elif key == curses.KEY_UP:
                picked -= 1
            elif key == curses.KEY_DOWN:
                picked += 1
            elif key == ord("c"):
                who = ask(screen, "connect bot named: ")
                who and run("connect", {"name": who})
            elif key == ord("t"):
                order = ask(screen, "team: objective (gather/attack/build/follow) what count [x1 y1 z1 x2 y2 z2]: ").split()
                if order:
                    what = order[1] if len(order) > 1 else ""
                    args = {"objective": order[0], "block": what, "type": what, "item": what, "count": int(order[2]) if len(order) > 2 else 16}
                    args.update(zip(["x1", "y1", "z1", "x2", "y2", "z2"], map(int, order[3:9])))
                    run("team", args)
            elif key == ord("x"):
                run("team", {"objective": "disband"})
            elif name is None:
                shown["message"] = "no bot connected: c to connect one"
            elif key == ord("a"):
                run("autopilot", {"bot": name, "enabled": not bots[picked].get("autopilot")})
            elif key == ord("e"):
                run("autopilot", {"bot": name, "enabled": bool(bots[picked].get("autopilot")), "gear": not bots[picked].get("gear")})
            elif key == ord("G"):
                what = ask(screen, "gather (block ids, comma separated) and count: ").split()
                if what:
                    run("autopilot", {"bot": name, "enabled": True, "gather": what[0], "count": int(what[1]) if len(what) > 1 else 16})
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
