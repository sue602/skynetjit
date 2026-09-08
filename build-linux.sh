#!/usr/bin/env bash
set -euo pipefail

export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
BUILD_DIR="$ROOT_DIR/build"
WORK_DIR="$BUILD_DIR/linux-work"
OUT_DIR="$BUILD_DIR/linux-out"
UPDATE=1
KEEP_REVISIONS=0
RUN_TESTS=1
BACKEND=auto

usage() {
	cat <<'USAGE'
Usage: ./build-linux.sh [options]

Options:
  --backend NAME  auto (default), epoll, or uring.
  --offline       Do not fetch newer submodule revisions.
  --no-sync       Use the already checked-out submodule revisions as-is.
  --no-test       Build only; skip smoke and Lua syntax tests.
  --jobs N        Parallel build jobs (defaults to CPU count).
  --help          Show this help.

Requirements:
  A native Linux GCC/make toolchain is required. io_uring mode additionally
  needs liburing development files discoverable through pkg-config.
USAGE
}

JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)
while [ "$#" -gt 0 ]; do
	case "$1" in
		--backend)
			shift
			[ "$#" -gt 0 ] || { echo "--backend needs a value" >&2; exit 2; }
			BACKEND=$1
			;;
		--offline) UPDATE=0 ;;
		--no-sync) KEEP_REVISIONS=1 ;;
		--no-test) RUN_TESTS=0 ;;
		--jobs)
			shift
			[ "$#" -gt 0 ] || { echo "--jobs needs a value" >&2; exit 2; }
			JOBS=$1
			;;
		--help|-h) usage; exit 0 ;;
		*) echo "Unknown option: $1" >&2; usage >&2; exit 2 ;;
	esac
	shift
done

	case "$BACKEND" in
		auto)
			# The poll-only io_uring adapter is intentionally opt-in. A full
			# completion backend will replace it; stable builds use epoll.
			BACKEND=epoll
		;;
	epoll|uring) ;;
	*) echo "Unknown socket backend: $BACKEND" >&2; exit 2 ;;
esac

if [ "$BACKEND" = uring ]; then
	command -v pkg-config >/dev/null 2>&1 && pkg-config --exists liburing || {
		echo "io_uring backend needs liburing development files (pkg-config liburing)." >&2
		exit 1
	}
fi

command -v gcc >/dev/null 2>&1 || { echo "Cannot find gcc." >&2; exit 1; }
command -v make >/dev/null 2>&1 || { echo "Cannot find make." >&2; exit 1; }

if [ "$KEEP_REVISIONS" -eq 1 ]; then
	:
elif [ "$UPDATE" -eq 1 ]; then
	"$ROOT_DIR/scripts/update-submodules.sh"
else
	git -C "$ROOT_DIR" submodule update --init --checkout -- \
		third_party/skynet-mingw third_party/luajit2
	git -C "$ROOT_DIR/third_party/skynet-mingw" submodule update \
		--init --checkout -- skynet
fi

case "$BUILD_DIR" in
	"$ROOT_DIR"/build) ;;
	*) echo "Unsafe build directory: $BUILD_DIR" >&2; exit 1 ;;
esac

rm -rf "$WORK_DIR" "$OUT_DIR"
mkdir -p "$WORK_DIR/skynet" "$WORK_DIR/luajit2" "$OUT_DIR"

git -C "$ROOT_DIR/third_party/skynet-mingw/skynet" archive HEAD |
	tar -xf - -C "$WORK_DIR/skynet"
git -C "$ROOT_DIR/third_party/luajit2" archive HEAD |
	tar -xf - -C "$WORK_DIR/luajit2"

(
	cd "$WORK_DIR/skynet"
	GIT_CEILING_DIRECTORIES="$WORK_DIR" \
		git apply --ignore-space-change "$ROOT_DIR/patches/skynet-luajit.patch"
	grep -q 'require "skynetjit.compat"' lualib/loader.lua || {
		echo "Skynet LuaJIT compatibility patch was not applied" >&2
		exit 1
	}
)
mkdir -p "$WORK_DIR/skynet/lualib/skynetjit"
cp "$ROOT_DIR/compat/lua/skynetjit/compat.lua" \
	"$WORK_DIR/skynet/lualib/skynetjit/compat.lua"
cp "$ROOT_DIR/compat/lua/skynet/sharetable.lua" \
	"$WORK_DIR/skynet/lualib/skynet/sharetable.lua"
mkdir -p "$WORK_DIR/skynet/lualib/skynet/sharetable"
cp "$ROOT_DIR/compat/lua/skynet/sharetable/codec.lua" \
	"$WORK_DIR/skynet/lualib/skynet/sharetable/codec.lua"
if [ "$BACKEND" = uring ]; then
	cp "$ROOT_DIR/compat/linux/socket_uring.h" \
		"$WORK_DIR/skynet/skynet-src/socket_uring.h"
fi

echo "Building LuaJIT2 for Linux..."
make -C "$WORK_DIR/luajit2" -j"$JOBS" \
	CC="${CC:-gcc}" HOST_CC="${HOST_CC:-gcc}" \
	XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT"

echo "Building Skynet for Linux with socket backend: $BACKEND..."
make -f "$ROOT_DIR/integration/linux.mk" -j"$JOBS" \
	CC="${CC:-gcc}" AR="${AR:-ar}" RANLIB="${RANLIB:-ranlib}" \
	SKYNET_DIR="$WORK_DIR/skynet" LUAJIT_DIR="$WORK_DIR/luajit2" \
	OUT="$OUT_DIR" INTEGRATION_DIR="$ROOT_DIR" SOCKET_BACKEND="$BACKEND" all

cp -a "$WORK_DIR/luajit2/src/libluajit.so" "$OUT_DIR/"
ln -sf libluajit.so "$OUT_DIR/libluajit-5.1.so.2"
cp "$WORK_DIR/luajit2/src/luajit" "$OUT_DIR/luajit"
cp -a "$WORK_DIR/skynet/lualib" "$OUT_DIR/lualib"
cp -a "$WORK_DIR/skynet/service" "$OUT_DIR/service"
cp -a "$WORK_DIR/skynet/examples" "$OUT_DIR/examples"
mkdir -p "$OUT_DIR/lualib/jit"
cp -a "$WORK_DIR/luajit2/src/jit/." "$OUT_DIR/lualib/jit/"

run_skynet_exit_test() {
	local config=$1
	local log=$2
	local timeout_tenths=${3:-150}
	local pid
	local done=0

	rm -f "$log"
	./skynet "$config" > "$log" 2>&1 &
	pid=$!
	for _ in $(seq 1 "$timeout_tenths"); do
		if ! kill -0 "$pid" 2>/dev/null; then
			done=1
			break
		fi
		sleep 0.1
	done
	if [ "$done" -eq 0 ]; then
		kill "$pid" 2>/dev/null || true
		wait "$pid" 2>/dev/null || true
		cat "$log" >&2
		echo "Skynet exit test timed out: $config" >&2
		return 1
	fi
	if ! wait "$pid"; then
		cat "$log" >&2
		return 1
	fi
	cat "$log"
}

if [ "$RUN_TESTS" -eq 1 ]; then
	echo "Running Linux LuaJIT compatibility and module smoke tests..."
	(
		cd "$OUT_DIR"
		./luajit "$ROOT_DIR/tests/smoke.lua"
		export LUA_PATH="./lualib/?.lua"
		find lualib service -type f -name '*.lua' ! -path 'lualib/jit/*' \
			-exec ./luajit -b '{}' /dev/null \;
	)
	echo "Running Skynet runtime smoke test ($BACKEND)..."
	(
		cd "$OUT_DIR"
		rm -f runtime-smoke.ok
		run_skynet_exit_test ../../tests/runtime-config.lua runtime-smoke.log
		test -f runtime-smoke.ok
		grep -q "runtime-smoke: Skynet socket loop succeeded" runtime-smoke.log
	)
	echo "Running graceful abort smoke test..."
	(
		cd "$OUT_DIR"
		rm -f abort-smoke.ok
		run_skynet_exit_test ../../tests/abort-config.lua abort-smoke.log
		test -f abort-smoke.ok
		grep -q "abort-smoke: graceful shutdown requested" abort-smoke.log
	)
fi

echo
echo "Build complete: $OUT_DIR"
echo "Socket backend requested: $BACKEND"
echo "LuaJIT: $OUT_DIR/luajit"
echo "Skynet: $OUT_DIR/skynet"
