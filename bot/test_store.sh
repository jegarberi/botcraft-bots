#!/bin/bash
# The stores, the same checks on each: SQLite in a temporary folder, a throwaway PostgreSQL cluster and a throwaway
# Valkey (or Redis) server, both on spare ports, removed afterwards. A server whose binaries are missing is skipped.
# Build first: cmake --build build --target test_store
set -u
cd "$(dirname "$0")"
tmp=$(mktemp -d)
trap 'pg_ctl -D "$tmp/pg" -m immediate stop >/dev/null 2>&1; [ -n "${kv:-}" ] && kill "$kv" 2>/dev/null; rm -rf "$tmp"' EXIT
fail=0

echo "== sqlite"
BOTCRAFT_DB= BOTCRAFT_MEMORY_DIR="$tmp/memory" BOTCRAFT_SHARED_DB="$tmp/shared.sqlite" bin/test_store || fail=1

pgbin=$(ls -d /usr/lib*/postgresql*/bin 2>/dev/null | sort -V | tail -1)
if [ -x "${pgbin:-}/initdb" ]; then
    echo "== postgresql"
    export PATH="$pgbin:$PATH"
    initdb -D "$tmp/pg" -U bots --auth=trust >/dev/null && \
        pg_ctl -D "$tmp/pg" -o "-p 55432 -k $tmp -c listen_addresses=''" -l "$tmp/pg.log" -w start >/dev/null && \
        createdb -h "$tmp" -p 55432 -U bots bots && \
        BOTCRAFT_DB="postgresql://bots@/bots?host=$tmp&port=55432" bin/test_store || { fail=1; tail -5 "$tmp/pg.log"; }
else
    echo "== postgresql: initdb not found, skipped"
fi

server=$(command -v valkey-server || command -v redis-server)
if [ -n "$server" ]; then
    echo "== valkey ($(basename "$server"))"
    "$server" --port 56379 --save '' --appendonly no --daemonize no >"$tmp/kv.log" 2>&1 &
    kv=$!
    until (exec 3<>/dev/tcp/127.0.0.1/56379) 2>/dev/null; do sleep 0.1; done
    BOTCRAFT_DB="valkey://127.0.0.1:56379/0" bin/test_store || fail=1
else
    echo "== valkey: no valkey-server or redis-server, skipped"
fi
exit $fail
