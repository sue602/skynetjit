#!/usr/bin/env bash
set -euo pipefail

export PATH="/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin:$PATH"
ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
WORK_DIR="$ROOT_DIR/build/linux-work"
OUT_DIR="$ROOT_DIR/build/linux-out"
source "$ROOT_DIR/scripts/build-common.sh"

usage() {
	echo "Usage: ./build-linux.sh [options]"
	echo
	common_usage
	echo
	echo "Requires Linux x64, a native GCC/make toolchain and Git."
}

parse_build_options "$@"
[ "$(uname -s)" = Linux ] || { echo "Run this script on Linux or in WSL." >&2; exit 1; }
CC=${CC:-gcc}
command -v "$CC" >/dev/null 2>&1 || { echo "Cannot find compiler: $CC" >&2; exit 1; }
command -v make >/dev/null 2>&1 || { echo "Cannot find make." >&2; exit 1; }
if [ "$SOCKET_BACKEND" = uring ]; then
	command -v pkg-config >/dev/null 2>&1 && pkg-config --exists liburing || {
		echo "The io_uring backend needs the liburing development package." >&2
		exit 1
	}
fi
case "$("$CC" -dumpmachine)" in
	x86_64*linux*) ;;
	*) echo "A native Linux x64 compiler is required." >&2; exit 1 ;;
esac
prepare_sources

echo "Building LuaJIT2 for Linux..."
make -C "$WORK_DIR/luajit2" -j"$JOBS" \
	CC="$CC" HOST_CC="${HOST_CC:-$CC}" \
	XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT"

echo "Building Skynet for Linux ($SOCKET_BACKEND)..."
make -f "$ROOT_DIR/integration/linux.mk" -j"$JOBS" \
	CC="$CC" SKYNET_DIR="$WORK_DIR/skynet" LUAJIT_DIR="$WORK_DIR/luajit2" \
	OUT="$OUT_DIR" INTEGRATION_DIR="$ROOT_DIR" SOCKET_BACKEND="$SOCKET_BACKEND" all

cp -a "$WORK_DIR/luajit2/src/libluajit.so" "$OUT_DIR/"
ln -sf libluajit.so "$OUT_DIR/libluajit-5.1.so.2"
cp "$WORK_DIR/luajit2/src/luajit" "$OUT_DIR/luajit"
copy_runtime_lua

if [ "$RUN_TESTS" -eq 1 ]; then
	bash "$ROOT_DIR/scripts/test-runtime.sh" linux "$OUT_DIR" "$SOCKET_BACKEND"
fi

echo
echo "Build complete: $OUT_DIR"
echo "LuaJIT: $OUT_DIR/luajit"
echo "Skynet: $OUT_DIR/skynet"
