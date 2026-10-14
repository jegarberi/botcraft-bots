#!/usr/bin/env bash
# Two bots are walked onto the same spot, then put on autopilot: they must end up at least --spacing blocks apart.
cd "$(dirname "$0")"
until ! pgrep -f "[b]ot_mcp.*Space" >/dev/null; do sleep 2; done
sleep 8
(python3 test_mcp.py --login Space --bots 2 --delay 5 --spacing 4 --web 8087 -- sleep:75 > /dev/null 2>&1 &)
sleep 18
call() { curl -s -m 60 -X POST -H "X-Bot: 1" -d "{\"name\":\"$1\",\"arguments\":$2}" http://127.0.0.1:8087/call; }
where() { call status "{\"bot\":\"$1\"}" | python3 -c "
import json,sys
p=json.load(sys.stdin)['result']['position']; print(p['x'], p['y'], p['z'])"; }
gap() { python3 -c "
import math,sys
a=list(map(float,sys.argv[1:4])); b=list(map(float,sys.argv[4:7])); print('%.1f' % math.dist(a,b))" $(where Space1) $(where Space2); }
read x y z <<< "$(where Space1)"
call goto "{\"bot\":\"Space2\",\"x\":$x,\"y\":$y,\"z\":$z,\"tolerance\":0}" | cut -c1-80; echo
echo "distance once together: $(gap)"
call autopilot '{"bot":"Space1","enabled":true,"gear":false}' >/dev/null
call autopilot '{"bot":"Space2","enabled":true,"gear":false}' >/dev/null
sleep 5;  echo "distance 5 s after autopilot on: $(gap)"
sleep 10; echo "distance 15 s after: $(gap)"
for b in Space1 Space2; do call status "{\"bot\":\"$b\"}" | python3 -c "
import json,sys
d=json.load(sys.stdin)['result']; print('  ', d['name'], '|', d['state'], '| flow', d['flow'][-5:])"; done
sleep 45; rm -rf memory test_mcp.log
