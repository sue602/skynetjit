#!/usr/bin/env bash
set -euo pipefail
export PATH="/bin:/usr/bin:$PATH"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)

case "${1:-}" in
	windows) EXE_SUFFIX=.exe; DEFAULT_OUT="$ROOT_DIR/build/out" ;;
	linux) EXE_SUFFIX=; DEFAULT_OUT="$ROOT_DIR/build/linux-out" ;;
	*) echo "Usage: $0 windows|linux [output-directory] [epoll|uring]" >&2; exit 2 ;;
esac
PLATFORM=$1
OUT_DIR=${2:-$DEFAULT_OUT}
BACKEND=${3:-epoll}
case "$BACKEND" in epoll|uring) ;; *) echo "Unknown backend: $BACKEND" >&2; exit 2 ;; esac
cd "$OUT_DIR"
# Both supported output directories live two levels below the project root. A
# relative path works for native Windows binaries as well as Linux/WSL, while
# an MSYS /e/... absolute path does not.
TEST_DIR=../../tests

run_skynet_exit_test() {
	local config=$1
	local log=$2
	local input=${3-}
	local timeout_tenths=${4:-150}
	local pid
	local ticks=0

	if [ -n "$input" ]; then
		printf '%b' "$input" | "./skynet$EXE_SUFFIX" "$config" > "$log" 2>&1 &
	else
		"./skynet$EXE_SUFFIX" "$config" > "$log" 2>&1 &
	fi
	pid=$!
	while kill -0 "$pid" 2>/dev/null; do
		if [ "$ticks" -ge "$timeout_tenths" ]; then
			kill "$pid" 2>/dev/null || true
			sleep 0.2
			kill -KILL "$pid" 2>/dev/null || true
			wait "$pid" 2>/dev/null || true
			cat "$log" >&2
			echo "Skynet exit test timed out: $config" >&2
			return 1
		fi
		sleep 0.1
		ticks=$((ticks + 1))
	done
	if ! wait "$pid"; then
		cat "$log" >&2
		return 1
	fi
	cat "$log"
}

echo "Running LuaJIT x64 and module smoke tests ($PLATFORM)..."
"./luajit$EXE_SUFFIX" "$ROOT_DIR/tests/smoke.lua"
(
	export LUA_PATH="./lualib/?.lua"
	find lualib service -type f -name '*.lua' ! -path 'lualib/jit/*' \
		-exec "./luajit$EXE_SUFFIX" -b '{}' /dev/null \;
)

echo "Running Skynet runtime smoke test..."
rm -f runtime-smoke.ok
run_skynet_exit_test "$TEST_DIR/runtime-config.lua" runtime-smoke.log
test -f runtime-smoke.ok
grep -q "runtime-smoke: Skynet socket loop succeeded" runtime-smoke.log
if [ "$BACKEND" = uring ]; then
	grep -q "io_uring completion backend enabled" runtime-smoke.log
	echo "Running completion-driven io_uring TCP and UDP round-trip tests..."
	rm -f uring-socket.ok
	run_skynet_exit_test "$TEST_DIR/uring-config.lua" uring-socket.log
	test -f uring-socket.ok
	grep -q "io_uring completion backend enabled" uring-socket.log
	grep -Eq "io_uring multishot accept (active|unavailable; using regular accept)" uring-socket.log
	grep -q "uring-socket: completion-driven TCP and UDP round trips succeeded" uring-socket.log
fi

echo "Running graceful abort smoke test..."
rm -f abort-smoke.ok
run_skynet_exit_test "$TEST_DIR/abort-config.lua" abort-smoke.log
test -f abort-smoke.ok
grep -q "abort-smoke: graceful shutdown requested" abort-smoke.log

if [ "$PLATFORM" = windows ]; then
	echo "Running Windows console stdin bridge smoke test..."
	rm -f abort-smoke.ok
	run_skynet_exit_test "$TEST_DIR/console-config.lua" \
		console-smoke.log 'abort_smoke\n'
	test -f abort-smoke.ok
	grep -q "LAUNCH snlua console" console-smoke.log
	grep -q "abort-smoke: graceful shutdown requested" console-smoke.log

fi

if [ "$PLATFORM" = windows ] || [ "$BACKEND" = uring ]; then
	if [ "$PLATFORM" = windows ]; then
		capacity_backend=wepoll
	else
		capacity_backend=io_uring
	fi
	echo "Running $capacity_backend 8192-socket capacity smoke test..."
	rm -f socket-capacity.ok
	run_skynet_exit_test "$TEST_DIR/socket-capacity-config.lua" \
		socket-capacity.log '' 600
	test -f socket-capacity.ok
	grep -q "socket-capacity: 8192 sockets registered" socket-capacity.log
fi
echo "Tests passed ($PLATFORM)."
