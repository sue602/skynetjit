#!/usr/bin/env bash
set -euo pipefail

export PATH="/bin:/usr/bin:$PATH"

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
WORK_DIR="$ROOT_DIR/build/work"
OUT_DIR="$ROOT_DIR/build/out"
source "$ROOT_DIR/scripts/build-common.sh"

usage() {
	cat <<'USAGE'
Usage: ./build.sh [options]

USAGE
	common_usage
	cat <<'USAGE'

Environment:
  MINGW64_ROOT    MinGW-w64 root, for example C:/mingw64 or /c/mingw64.
  CC, AR, RANLIB  Optional x86_64-w64-mingw32 tool overrides.
USAGE
}

parse_build_options "$@"

if [ "$SOCKET_BACKEND" != epoll ]; then
	echo "The io_uring backend is available only to build-linux.sh." >&2
	exit 2
fi

to_msys_path() {
	case "$1" in
		[A-Za-z]:/*|[A-Za-z]:\\*)
			if command -v cygpath >/dev/null 2>&1; then
				cygpath -u "$1"
			else
				echo "$1"
			fi
			;;
		*) echo "$1" ;;
	esac
}

if [ -n "${MINGW64_ROOT:-}" ]; then
	MINGW_ROOT=$(to_msys_path "$MINGW64_ROOT")
elif [ -d /c/mingw64/bin ]; then
	MINGW_ROOT=/c/mingw64
elif [ -d /mingw64/bin ]; then
	MINGW_ROOT=/mingw64
else
	echo "Cannot find MinGW-w64 x64. Set MINGW64_ROOT." >&2
	exit 1
fi

export PATH="$MINGW_ROOT/bin:$PATH"
CC=${CC:-gcc}
AR=${AR:-ar}
RANLIB=${RANLIB:-ranlib}

TARGET=$($CC -dumpmachine)
if [ "$TARGET" != "x86_64-w64-mingw32" ]; then
	echo "Expected x86_64-w64-mingw32 compiler, got: $TARGET" >&2
	exit 1
fi

prepare_sources

echo "Building LuaJIT2 for x64..."
make -C "$WORK_DIR/luajit2" -j"$JOBS" \
	CC="$CC" HOST_CC="$CC" \
	XCFLAGS="-DLUAJIT_ENABLE_LUA52COMPAT"

echo "Building Skynet with the LuaJIT2 backend..."
make -f "$ROOT_DIR/integration/Makefile" -j"$JOBS" \
	CC="$CC" AR="$AR" RANLIB="$RANLIB" \
	SKYNET_DIR="$WORK_DIR/skynet" \
	LUAJIT_DIR="$WORK_DIR/luajit2" \
	OUT="$OUT_DIR" INTEGRATION_DIR="$ROOT_DIR" all

cp "$WORK_DIR/luajit2/src/lua51.dll" "$OUT_DIR/lua51.dll"
cp "$WORK_DIR/luajit2/src/luajit.exe" "$OUT_DIR/luajit.exe"
copy_runtime_lua

if [ -f "$MINGW_ROOT/bin/libwinpthread-1.dll" ]; then
	cp "$MINGW_ROOT/bin/libwinpthread-1.dll" "$OUT_DIR/"
fi

if [ "$RUN_TESTS" -eq 1 ]; then
	bash "$ROOT_DIR/scripts/test-runtime.sh" windows "$OUT_DIR"
fi

echo
echo "Build complete: $OUT_DIR"
echo "LuaJIT: $OUT_DIR/luajit.exe"
echo "Skynet: $OUT_DIR/skynet.exe"
