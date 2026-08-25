#!/usr/bin/env bash
set -euo pipefail

export PATH="/bin:/usr/bin:$PATH"

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
BUILD_DIR="$ROOT_DIR/build"
WORK_DIR="$BUILD_DIR/work"
OUT_DIR="$BUILD_DIR/out"
UPDATE=1
RUN_TESTS=1

usage() {
	cat <<'USAGE'
Usage: ./build.sh [options]

Options:
  --offline       Do not fetch newer submodule revisions.
  --no-test       Build only; skip smoke and Lua syntax tests.
  --jobs N        Parallel build jobs (defaults to CPU count).
  --help          Show this help.

Environment:
  MINGW64_ROOT    MinGW-w64 root, for example C:/mingw64 or /c/mingw64.
  CC, AR, RANLIB  Optional x86_64-w64-mingw32 tool overrides.
USAGE
}

JOBS="${NUMBER_OF_PROCESSORS:-}"
if command -v nproc >/dev/null 2>&1; then
	JOBS=$(nproc)
fi
JOBS=${JOBS:-2}

while [ "$#" -gt 0 ]; do
	case "$1" in
		--offline) UPDATE=0 ;;
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

if [ "$UPDATE" -eq 1 ]; then
	"$ROOT_DIR/scripts/update-submodules.sh"
else
	git -C "$ROOT_DIR" submodule update --init --checkout -- \
		third_party/skynet-mingw third_party/luajit2
	git -C "$ROOT_DIR/third_party/skynet-mingw" submodule update \
		--init --checkout -- skynet
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
	# Keep git from treating this ignored work copy as part of the outer repo.
	GIT_CEILING_DIRECTORIES="$WORK_DIR" \
		git apply "$ROOT_DIR/patches/skynet-luajit.patch"
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
cp -a "$WORK_DIR/skynet/lualib" "$OUT_DIR/lualib"
cp -a "$WORK_DIR/skynet/service" "$OUT_DIR/service"
cp -a "$WORK_DIR/skynet/examples" "$OUT_DIR/examples"
mkdir -p "$OUT_DIR/lualib/jit"
cp -a "$WORK_DIR/luajit2/src/jit/." "$OUT_DIR/lualib/jit/"

if [ -f "$MINGW_ROOT/bin/libwinpthread-1.dll" ]; then
	cp "$MINGW_ROOT/bin/libwinpthread-1.dll" "$OUT_DIR/"
fi

if [ "$RUN_TESTS" -eq 1 ]; then
	echo "Running x64, Lua compatibility, and module smoke tests..."
	(
		cd "$OUT_DIR"
		./luajit.exe "$ROOT_DIR/tests/smoke.lua"
	)
	if command -v find >/dev/null 2>&1; then
		(
			cd "$OUT_DIR"
			export LUA_PATH="./lualib/?.lua"
			find lualib service -type f -name '*.lua' \
				! -path 'lualib/jit/*' -print |
			while IFS= read -r file; do
				./luajit.exe -b "$file" /dev/null
			done
		)
	fi
	echo "Running Skynet socket-loop runtime smoke test..."
	(
		cd "$OUT_DIR"
		rm -f runtime-smoke.log runtime-smoke.ok
		./skynet.exe ../../tests/runtime-config.lua > runtime-smoke.log 2>&1 &
		SKYNET_PID=$!
		SKYNET_DONE=0
		for _ in $(seq 1 150); do
			if [ -f runtime-smoke.ok ]; then
				SKYNET_DONE=1
				break
			fi
			if ! kill -0 "$SKYNET_PID" 2>/dev/null; then
				SKYNET_DONE=1
				break
			fi
			sleep 0.1
		done
		if [ "$SKYNET_DONE" -eq 0 ] || [ ! -f runtime-smoke.ok ]; then
			kill "$SKYNET_PID" 2>/dev/null || true
			wait "$SKYNET_PID" 2>/dev/null || true
			cat runtime-smoke.log >&2
			echo "Skynet runtime smoke test failed or timed out" >&2
			exit 1
		fi
		if ! wait "$SKYNET_PID"; then
			cat runtime-smoke.log >&2
			exit 1
		fi
		cat runtime-smoke.log
		grep -q "runtime-smoke: Skynet x64 socket loop succeeded" runtime-smoke.log
	)
fi

echo
echo "Build complete: $OUT_DIR"
echo "LuaJIT: $OUT_DIR/luajit.exe"
echo "Skynet: $OUT_DIR/skynet.exe"
