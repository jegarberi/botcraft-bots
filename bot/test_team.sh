#!/usr/bin/env bash
# Two bots team up on coal: expect "[team]" finds, orders and status reports in the chat,
# and no new "found" once the places are in memory (the team is set a second time to check).
cd "$(dirname "$0")"
until ! pgrep -f "[b]ot_mcp.*Tester" >/dev/null; do sleep 2; done
sleep 8
python3 test_mcp.py --login Tester --bots 2 --delay 5 -- sleep:14 \
  'autopilot:{"bot":"Tester1","enabled":false,"gear":false}' 'autopilot:{"bot":"Tester2","enabled":false,"gear":false}' \
  'team:{"objective":"gather","block":"coal_ore","count":4}' sleep:45 \
  'read_chat:{"bot":"Tester1"}' 'team:{"objective":"gather","block":"coal_ore","count":4}' sleep:40 \
  'read_chat:{"bot":"Tester1"}' 'status:{"bot":"Tester1"}' 2>&1 | python3 -c "
import sys,json
n=0
for l in sys.stdin:
    if l.startswith('status'):
        d=json.loads(l[l.index('{',8):]); print('status', d['name'], '|', d['state'], '| reports', d['team_reports'], '| order', d['team_order'], '| memory', d['memory'])
    elif l.startswith('read_chat'):
        n+=1; lines=[m for m in json.loads(l[l.index('s] ')+3:]) if '[team] Tester' in m]
        print('team chat after team call #%d: %d found, %d orders, %d other' % (n, sum(' found ' in m for m in lines), sum(' mine ' in m for m in lines), sum(' found ' not in m and ' mine ' not in m for m in lines)))
        for m in lines[:6]: print('   ', m[:140])
    else: print(l[:130].rstrip())"
grep -E "ERROR|FATAL" test_mcp.log | cut -c28-200 | head -5
rm -rf memory test_mcp.log
