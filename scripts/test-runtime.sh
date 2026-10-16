#!/usr/bin/env bash
set -euo pipefail
export PATH="/bin:/usr/bin:$PATH"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)

case "${1:-}" in
	windows) EXE_SUFFIX=.exe; DEFAULT_OUT="$ROOT_DIR/build/out" ;;
	linux) EXE_SUFFIX=; DEFAULT_OUT="$ROOT_DIR/build/linux-out" ;;
	*) echo "Usage: $0 windows|linux [output-directory]" >&2; exit 2 ;;
esac
PLATFORM=$1
OUT_DIR=${2:-$DEFAULT_OUT}
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

if [ -f luaclib/cjson.so ] && [ -f luaclib/luacurl.so ] && [ -f luaclib/skiplist/c.so ]; then
	echo "Running NetBull luaclib smoke test..."
	rm -f clibs-smoke.ok
	run_skynet_exit_test "$TEST_DIR/clibs-config.lua" clibs-smoke.log
	test -f clibs-smoke.ok
	grep -q "clibs-smoke: NetBull luaclib modules succeeded" clibs-smoke.log
else
	echo "NetBull luaclib set not built; skipping its smoke test."
fi
if [ -f luaclib/redis/bitmap.so ] && [ -f luaclib/redis/hll.so ] &&
   [ -f luaclib/redis/sortedset.so ] && [ -f luaclib/redis/geo.so ] &&
   [ -f luaclib/redis/set.so ]; then
	echo "Running redislib smoke test..."
	rm -f redislib-smoke.ok
	run_skynet_exit_test "$TEST_DIR/redislib-config.lua" redislib-smoke.log
	test -f redislib-smoke.ok
	grep -q "redislib-smoke: redislib modules succeeded" redislib-smoke.log
else
	echo "redislib set not built; skipping its smoke test."
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

if [ "$PLATFORM" = windows ]; then
	# This regression exists for the Windows side: its socket capacity is a wepoll
	# shim over WSAPoll with an FD_SETSIZE ceiling of its own, so "can the socket
	# server actually hold 65535 of them" is a Windows-specific question. The Linux
	# epoll path never ran it and does not now.
	capacity_count=${SKYNETJIT_SOCKET_CAPACITY:-65535}
	case "$capacity_count" in
		''|*[!0-9]*) echo "SKYNETJIT_SOCKET_CAPACITY must be numeric" >&2; exit 2 ;;
	esac
	if [ "$capacity_count" -lt 1 ] || [ "$capacity_count" -gt 65535 ]; then
		echo "SKYNETJIT_SOCKET_CAPACITY must be between 1 and 65535" >&2
		exit 2
	fi
	export SKYNETJIT_SOCKET_CAPACITY=$capacity_count
	echo "Running wepoll $capacity_count-socket capacity smoke test..."
	rm -f socket-capacity.ok
	run_skynet_exit_test "$TEST_DIR/socket-capacity-config.lua" \
		socket-capacity.log '' 2400
	test -f socket-capacity.ok
	grep -q "socket-capacity: $capacity_count sockets registered" socket-capacity.log
fi
echo "Tests passed ($PLATFORM)."
