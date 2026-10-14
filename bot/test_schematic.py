#!/usr/bin/env python3
"""Check of the schematic tool against the running program: writes a small structure file (a 3x2x3 cobblestone
shell with a stair facing east and a block of another mod), then asks for its materials. python3 test_schematic.py"""
import gzip, json, struct, urllib.request

def s(text): return struct.pack(">H", len(text)) + text.encode()
def named(tag, name): return bytes([tag]) + s(name)
def ints(name, values): return named(9, name) + bytes([3]) + struct.pack(">i", len(values)) + b"".join(struct.pack(">i", v) for v in values)
def compounds(name, items): return named(9, name) + bytes([10]) + struct.pack(">i", len(items)) + b"".join(i + b"\0" for i in items)

palette = [named(8, "Name") + s("minecraft:cobblestone"),
           named(8, "Name") + s("minecraft:oak_stairs") + named(10, "Properties") + named(8, "facing") + s("east") + b"\0",
           named(8, "Name") + s("create:cogwheel"), named(8, "Name") + s("minecraft:air")]
cells = [(x, y, z, 0) for x in range(3) for y in range(2) for z in range(3) if (x, z) != (1, 1)] + [(1, 0, 1, 1), (1, 1, 1, 2), (1, 2, 1, 3)]
blocks = [ints("pos", c[:3]) + named(3, "state") + struct.pack(">i", c[3]) for c in cells]
nbt = named(10, "") + ints("size", [3, 3, 3]) + compounds("palette", palette) + compounds("blocks", blocks) + b"\0"
gzip.open("schematics/test_shell.nbt", "wb").write(nbt)

request = urllib.request.Request("http://127.0.0.1:8090/call", json.dumps({"name": "schematic", "arguments": {"file": "test_shell.nbt"}}).encode(), {"X-Bot": "1"})
r = json.load(urllib.request.urlopen(request, timeout=30))
assert r["ok"], r
r = r["result"]
assert r["blocks"] == 17 and r["skipped_blocks_of_other_mods"] == 1 and r["parts"] == 1, r
assert {m["item"]: m["count"] for m in r["materials"]} == {"minecraft:cobblestone": 16, "minecraft:oak_stairs": 1}, r
for bad in ("../chests.txt", "x.nbt'; touch pwned; '"):
    request = urllib.request.Request("http://127.0.0.1:8090/call", json.dumps({"name": "schematic", "arguments": {"file": bad}}).encode(), {"X-Bot": "1"})
    assert not json.load(urllib.request.urlopen(request, timeout=30))["ok"], bad
print("schematic tool OK")
