#!/usr/bin/env bash
# run.sh -- redis-diff: replay the same operation log through a real
# redis-server and through the redislib modules, then diff the two recordings.
#
#	Run it from inside Linux (this project builds and tests redislib in WSL
#	Ubuntu):
#
#	    ./tools/redis-diff/run.sh [seed] [ops]
#
# Requirements, checked rather than assumed:
#   - a redis-server binary, from the source tree this port was made against.
#     Build it in Linux's own filesystem (WSL's /tmp does not survive the VM
#     stopping):
#	         mkdir -p ~/src && cp -r /mnt/d/download/redis-8.10.0 ~/src/ &&
#	         make -C ~/src/redis-8.10.0 -j"$(nproc)" MALLOC=libc all
#     or point REDIS_SERVER at any redis-server 8.10.0.
#   - a built redislib tree in build/linux-out (./build-linux.sh --no-sync)
#
# Nothing is written outside work/ (git-ignored) and the server that is started
# here is stopped here, by the PID this script recorded -- never by process name.
set -euo pipefail

ROOT=$(CDPATH= cd -- "$(dirname -- "$0")/../.." && pwd -P)
DIFF_DIR="$ROOT/tools/redis-diff"
WORK="$DIFF_DIR/work"
SEED=${1:-1}
OPS=${2:-3000}
PORT=${PORT:-6399}
mkdir -p "$WORK"

SERVER=${REDIS_SERVER:-$HOME/src/redis-8.10.0/src/redis-server}
[ -x "$SERVER" ] || { echo "no redis-server at $SERVER (see the header)"; exit 2; }
[ -x "$ROOT/build/linux-out/skynet" ] || { echo "build/linux-out is not built"; exit 2; }

echo "== 1. generate the oplog =="
(
	cd "$ROOT/build/linux-out"
	LD_LIBRARY_PATH="$ROOT/build/linux-out" \
		./luajit "$DIFF_DIR/gen.lua" "$SEED" "$OPS" "$WORK/oplog.jsonl"
)

echo "== 2. start redis-server on 127.0.0.1:$PORT =="
"$SERVER" --port "$PORT" --bind 127.0.0.1 --save '' --appendonly no \
	--daemonize no --loglevel notice > "$WORK/redis.log" 2>&1 &
REDIS_PID=$!
echo "   pid $REDIS_PID"

stop_redis() {
	# only ever this run's pid, and only if it is still the server we started
	if kill -0 "$REDIS_PID" 2>/dev/null &&
	   tr -d '\0' < "/proc/$REDIS_PID/cmdline" 2>/dev/null | grep -q redis-server; then
		kill -TERM "$REDIS_PID" 2>/dev/null || true
		wait "$REDIS_PID" 2>/dev/null || true
	fi
}
trap stop_redis EXIT

for _ in $(seq 1 50); do
	if python3 - "$PORT" <<'PY' 2>/dev/null
import socket, sys
s = socket.create_connection(("127.0.0.1", int(sys.argv[1])), timeout=2)
s.sendall(b"*1\r\n$4\r\nPING\r\n")
sys.exit(0 if b"+PONG" in s.recv(64) else 1)
PY
	then
		echo "   PING ok"
		break
	fi
	sleep 0.2
done

echo "== 3. replay against the server =="
python3 "$DIFF_DIR/replay_redis.py" --port "$PORT" \
	--oplog "$WORK/oplog.jsonl" --out "$WORK/redis-results.jsonl"

echo "== 4. replay against the modules =="
sed -e "s|redisdiff_oplog = .*|redisdiff_oplog = \"$WORK/oplog.jsonl\"|" \
	-e "s|redisdiff_out = .*|redisdiff_out = \"$WORK/lib-results.jsonl\"|" \
	-e "s|root = \"./\"|root = \"$ROOT/build/linux-out/\"|" \
	"$DIFF_DIR/lib-config.lua" > "$WORK/lib-config.run.lua"
# `timeout` because a service that fails to load leaves skynet running with
# nothing to do; the log goes to a file so a startup error stays readable.
(
	cd "$ROOT/build/linux-out"
	LD_LIBRARY_PATH="$ROOT/build/linux-out" \
		timeout 600 ./skynet "$WORK/lib-config.run.lua" > "$WORK/lib-replay.log" 2>&1
) || echo "   skynet exited nonzero (see $WORK/lib-replay.log)"
tail -5 "$WORK/lib-replay.log" 2>/dev/null || true

echo "== 5. diff =="
set +e
python3 "$DIFF_DIR/diff.py" "$WORK/redis-results.jsonl" "$WORK/lib-results.jsonl" \
	| tee "$WORK/diff.txt" | head -60
RC=${PIPESTATUS[0]}
set -e
echo "diff exit code (differing records): $RC"
echo "full output: $WORK/diff.txt"
exit "$RC"
