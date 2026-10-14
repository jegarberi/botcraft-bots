#!/usr/bin/env bash
# Check that a kicked bot and a bot whose process died both come back after 60 seconds.
cd "$(dirname "$0")"
until ! pgrep -f "[b]ot_mcp.*Tester" >/dev/null; do sleep 2; done
sleep 8   # the server throttles joins that follow a leave too closely
state() { curl -s -m 5 http://127.0.0.1:8087/state | python3 -c "
import json,sys
for b in json.load(sys.stdin)['bots']: print('  ', b['name'], '|', b['state'], '|', b.get('position'))"; }
(python3 test_mcp.py --login Tester --bots 2 --delay 5 --web 8087 -- sleep:105 bots > test_out.txt 2>&1 &)
sleep 16
pkill -9 -f "[b]ot_mcp --child.*--login Tester2"; echo "killed Tester2's process: $?"
timeout 6 ./bin/bot 192.168.100.78:25565 Tester1 >/dev/null 2>&1   # same name from elsewhere: Tester1 gets kicked
sleep 6;  echo "after ~12s:"; state
sleep 66; echo "after ~80s:"; state
sleep 16; tail -1 test_out.txt | cut -c1-200
grep -E "joined again|was dropped|ended, starting|could not join" test_mcp.log | cut -c28-220
rm -rf memory test_out.txt test_mcp.log
