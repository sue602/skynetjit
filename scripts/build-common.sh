#!/usr/bin/env bash
# Shared by the Windows and Linux entry points. They set ROOT_DIR/WORK_DIR/OUT_DIR.

parse_build_options() {
	SYNC_MODE=update
	RUN_TESTS=1
	JOBS=${NUMBER_OF_PROCESSORS:-2}
	if command -v nproc >/dev/null 2>&1; then
		JOBS=$(nproc)
	fi
	while [ "$#" -gt 0 ]; do
		case "$1" in
			--offline) SYNC_MODE=offline ;;
			--no-sync) SYNC_MODE=current ;;
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
	case "$JOBS" in
		''|*[!0-9]*|0) echo "--jobs must be a positive integer" >&2; exit 2 ;;
	esac
}

common_usage() {
	cat <<'USAGE'
Options:
  --offline       Use recorded submodule revisions without fetching updates.
  --no-sync       Use the already checked-out revisions without changing them.
  --no-test       Build only; skip smoke and Lua syntax tests.
  --jobs N        Parallel build jobs (defaults to CPU count).
  --help          Show this help.
USAGE
}

prepare_sources() {
	case "$SYNC_MODE" in
		update) bash "$ROOT_DIR/scripts/update-submodules.sh" ;;
		offline)
			git -C "$ROOT_DIR" submodule update --init --checkout -- \
				third_party/skynet-mingw third_party/luajit2
			git -C "$ROOT_DIR/third_party/skynet-mingw" submodule update \
				--init --checkout -- skynet
			;;
		current)
			# Check initialization before clearing any previous build output.
			test -f "$ROOT_DIR/third_party/skynet-mingw/skynet/skynet-src/socket_server.c"
			test -f "$ROOT_DIR/third_party/luajit2/src/luajit.c"
			;;
	esac

	# Only the two platform-specific build pairs may be recreated.
	case "$WORK_DIR:$OUT_DIR" in
		"$ROOT_DIR/build/work:$ROOT_DIR/build/out" | \
		"$ROOT_DIR/build/linux-work:$ROOT_DIR/build/linux-out") ;;
		*) echo "Unsafe build directories: $WORK_DIR / $OUT_DIR" >&2; exit 1 ;;
	esac
	mkdir -p "$ROOT_DIR/build"
	if [ "$(cd "$ROOT_DIR/build" && pwd -P)" != "$ROOT_DIR/build" ] ||
		[ -L "$WORK_DIR" ] || [ -L "$OUT_DIR" ]; then
		echo "Build directories must not redirect outside the workspace." >&2
		exit 1
	fi
	rm -rf "$WORK_DIR" "$OUT_DIR"
	mkdir -p "$WORK_DIR/skynet" "$WORK_DIR/luajit2" "$OUT_DIR"

	git -C "$ROOT_DIR/third_party/skynet-mingw/skynet" archive HEAD |
		tar -xf - -C "$WORK_DIR/skynet"
	git -C "$ROOT_DIR/third_party/luajit2" archive HEAD |
		tar -xf - -C "$WORK_DIR/luajit2"
	(
		cd "$WORK_DIR/skynet"
		# Isolate the archive from the parent Git repo; tolerate upstream CRLF.
		GIT_CEILING_DIRECTORIES="$WORK_DIR" \
			git apply --ignore-space-change "$ROOT_DIR/patches/skynet-luajit.patch"
		grep -q 'require "skynetjit.compat"' lualib/loader.lua
	)
	cp -a "$ROOT_DIR/compat/lua/." "$WORK_DIR/skynet/lualib/"
}

copy_runtime_lua() {
	cp -a "$WORK_DIR/skynet/lualib" "$OUT_DIR/lualib"
	cp -a "$WORK_DIR/skynet/service" "$OUT_DIR/service"
	cp -a "$WORK_DIR/skynet/examples" "$OUT_DIR/examples"
	mkdir -p "$OUT_DIR/lualib/jit"
	cp -a "$WORK_DIR/luajit2/src/jit/." "$OUT_DIR/lualib/jit/"
}
