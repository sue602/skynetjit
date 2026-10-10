#!/usr/bin/env bash
# interop.sh -- spec 4's round trip between a real redis-server and the modules.
#
#     ./tools/redis-diff/interop.sh
#
# Same requirements as run.sh (a redis-server 8.10.0 binary and a built
# build/linux-out). The server is started and stopped by this script, by PID.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
DIFF_DIR="$ROOT/tools/redis-diff"
WORK="$DIFF_DIR/work"
PORT=${PORT:-6399}
mkdir -p "$WORK"

SERVER=${REDIS_SERVER:-$HOME/src/redis-8.10.0/src/redis-server}
[ -x "$SERVER" ] || { echo "no redis-server at $SERVER"; exit 2; }
[ -x "$ROOT/build/linux-out/skynet" ] || { echo "build/linux-out is not built"; exit 2; }

"$SERVER" --port "$PORT" --bind 127.0.0.1 --save '' --appendonly no \
	--daemonize no > "$WORK/interop-redis.log" 2>&1 &
REDIS_PID=$!
stop_redis() {
	if kill -0 "$REDIS_PID" 2>/dev/null &&
	   tr -d '\0' < "/proc/$REDIS_PID/cmdline" 2>/dev/null | grep -q redis-server; then
		kill -TERM "$REDIS_PID" 2>/dev/null || true
		wait "$REDIS_PID" 2>/dev/null || true
	fi
}
trap stop_redis EXIT
for _ in $(seq 1 50); do
	python3 -c "import socket,sys; s=socket.create_connection(('127.0.0.1',$PORT),2); s.sendall(b'*1\r\n\$4\r\nPING\r\n'); sys.exit(0 if b'+PONG' in s.recv(64) else 1)" 2>/dev/null && break
	sleep 0.2
done

echo "== 1. the server makes an HLL and a string value =="
python3 "$DIFF_DIR/interop.py" --port "$PORT" export \
	--out "$WORK/interop-from-redis.json"

echo "== 2. the modules load them and make their own =="
sed -e "s|root = \"./\"|root = \"$ROOT/build/linux-out/\"|" \
	-e "s|redisdiff_in = .*|redisdiff_in = \"$WORK/interop-from-redis.json\"|" \
	-e "s|redisdiff_out = .*|redisdiff_out = \"$WORK/interop-from-lib.json\"|" \
	-e "s|redisdiff_report = .*|redisdiff_report = \"$WORK/interop-lib-report.json\"|" \
	"$DIFF_DIR/interop-config.lua" > "$WORK/interop-config.run.lua"
(
	cd "$ROOT/build/linux-out"
	LD_LIBRARY_PATH="$ROOT/build/linux-out" \
		timeout 300 ./skynet "$WORK/interop-config.run.lua" 2>&1 | tail -3
)

echo "== 3. the server imports what the modules produced =="
python3 "$DIFF_DIR/interop.py" --port "$PORT" import \
	--redis "$WORK/interop-from-redis.json" \
	--lib "$WORK/interop-from-lib.json" \
	--report "$WORK/interop-lib-report.json"
