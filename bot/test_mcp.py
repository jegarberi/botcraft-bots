#!/usr/bin/env python3
"""Smoke test: start bot_mcp, speak MCP over stdio, call tools.
Usage: test_mcp.py [server options ...] -- [tool[:json_args] ...]"""
import base64, json, subprocess, sys, time
opts, steps = (sys.argv[1:sys.argv.index("--")], sys.argv[sys.argv.index("--") + 1:]) if "--" in sys.argv else ([], sys.argv[1:])
p = subprocess.Popen(["./bin/bot_mcp", *opts], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=open("test_mcp.log", "w"), text=True)
n = 0
def rpc(method, params=None):
    global n; n += 1
    p.stdin.write(json.dumps({"jsonrpc": "2.0", "id": n, "method": method, "params": params or {}}) + "\n"); p.stdin.flush()
    line = p.stdout.readline()
    assert line, "server died, see test_mcp.log"
    return json.loads(line)
r = rpc("initialize", {"protocolVersion": "2025-06-18", "capabilities": {}, "clientInfo": {"name": "test", "version": "0"}})
assert r["result"]["serverInfo"]["name"] == "botcraft", r
p.stdin.write('{"jsonrpc":"2.0","method":"notifications/initialized"}\n'); p.stdin.flush()
names = [t["name"] for t in rpc("tools/list")["result"]["tools"]]
assert "status" in names and "collect" in names, names
print("tools:", " ".join(names))
time.sleep(3)
for step in steps or ["status"]:
    name, _, arg = step.partition(":")
    if name == "sleep":
        time.sleep(float(arg)); continue
    t = time.time()
    r = rpc("tools/call", {"name": name, "arguments": json.loads(arg) if arg else {}})["result"]
    c = r["content"][0]
    if c["type"] == "image":
        data = base64.b64decode(c["data"]); open("test_snapshot.png", "wb").write(data)
        text = f"image {len(data)} bytes -> test_snapshot.png"
    else:
        text = c["text"][:6000]
    print(f"{name} {arg} [{time.time() - t:.1f}s] {'ERROR ' if r['isError'] else ''}{text}")
p.stdin.close()
print("exit", p.wait(timeout=30))
