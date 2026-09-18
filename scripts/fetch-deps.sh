#!/usr/bin/env bash
# Fetch and build the Windows static dependencies (zlib, libcurl) for the
# NetBull luaclib set. Results are cached under build/deps and never touch
# the submodules. Linux builds use the system curl/zlib instead.
set -euo pipefail

ROOT_DIR=$1
DEPS_DIR=$2
MINGW_ROOT=$3
SYNC_MODE=${4:-update}
JOBS=${5:-2}

ZLIB_VER=1.3.1
CURL_VER=8.22.0
CMAKE_VER=3.31.6

ZLIB_SHA256=9a93b2b7dfdac77ceba5a558a580e74667dd6fede4585b91eefb60f03b72df23
CURL_SHA256=d54dd598bf05927a726deb38df31c6a255ba83ff1de57c5d1464dac3ed8f44a1
CMAKE_SHA256=

STAMP="$DEPS_DIR/.built-zlib$ZLIB_VER-curl$CURL_VER"
if [ -f "$STAMP" ]; then
	exit 0
fi
if [ "$SYNC_MODE" = offline ]; then
	echo "Windows curl/zlib dependencies are not built and --offline forbids downloads." >&2
	echo "Run the build once without --offline, or pre-populate $DEPS_DIR." >&2
	exit 1
fi

case "$SYSTEMROOT" in
	[A-Za-z]:*) SYS_BIN="$SYSTEMROOT/System32" ;;
	*) SYS_BIN='C:/Windows/System32' ;;
esac
DL_EXE="$SYS_BIN/curl.exe"
TAR_EXE="$SYS_BIN/tar.exe"
CERT_EXE="$SYS_BIN/certutil.exe"
[ -x "$DL_EXE" ] || DL_EXE=$(command -v curl) || {
	echo "Need Windows curl.exe for dependency downloads." >&2; exit 1; }
[ -x "$TAR_EXE" ] || {
	echo "Need Windows bsdtar (System32\\tar.exe) for zip extraction." >&2; exit 1; }

mkdir -p "$DEPS_DIR/downloads"

sha256_of() { # file
	local file=$1
	if command -v sha256sum >/dev/null 2>&1; then
		sha256sum "$file" | cut -d" " -f1
	elif [ -x "$CERT_EXE" ]; then
		"$CERT_EXE" -hashfile "$file" SHA256 2>/dev/null | sed -n 2p | tr "A-Z" "a-z"
	else
		echo ""
	fi
}

fetch() { # file sha256 url...
	local file=$1 sum=$2 url have
	shift 2
	if [ -f "$DEPS_DIR/downloads/$file" ] && [ -n "$sum" ]; then
		have=$(sha256_of "downloads/$file")
		if [ -n "$have" ] && [ "$have" != "$sum" ]; then
			rm -f "$DEPS_DIR/downloads/$file"
		fi
	fi
	if [ ! -f "$DEPS_DIR/downloads/$file" ]; then
		for url in "$@"; do
			echo "Downloading $url"
			if "$DL_EXE" -sSL --retry 4 --retry-delay 3 -C - \
				-o "$DEPS_DIR/downloads/$file.part" "$url" &&
				mv "$DEPS_DIR/downloads/$file.part" "$DEPS_DIR/downloads/$file"; then
				break
			fi
			rm -f "$DEPS_DIR/downloads/$file.part"
		done
	fi
	[ -f "$DEPS_DIR/downloads/$file" ] || { echo "Failed to download $file" >&2; exit 1; }
	if [ -n "$sum" ]; then
		have=$(cd "$DEPS_DIR" && sha256_of "downloads/$file")
		if [ -z "$have" ]; then
			echo "No sha256 tool found; skipping integrity check for $file" >&2
		elif [ "$have" != "$sum" ]; then
			echo "sha256 mismatch for $file" >&2
			exit 1
		fi
	fi
}

fetch "zlib-$ZLIB_VER.tar.gz" "$ZLIB_SHA256" \
	"https://zlib.net/fossils/zlib-$ZLIB_VER.tar.gz" \
	"https://github.com/madler/zlib/releases/download/v$ZLIB_VER/zlib-$ZLIB_VER.tar.gz"
fetch "curl-$CURL_VER.tar.gz" "$CURL_SHA256" \
	"https://github.com/curl/curl/releases/download/curl-$(echo "$CURL_VER" | tr . _)/curl-$CURL_VER.tar.gz" \
	"https://curl.se/download/curl-$CURL_VER.tar.gz"
fetch "cmake-$CMAKE_VER-windows-x86_64.zip" "$CMAKE_SHA256" \
	"https://cmake.org/files/v${CMAKE_VER%.*}/cmake-$CMAKE_VER-windows-x86_64.zip" \
	"https://github.com/Kitware/CMake/releases/download/v$CMAKE_VER/cmake-$CMAKE_VER-windows-x86_64.zip"

CMAKE="$DEPS_DIR/cmake-$CMAKE_VER-windows-x86_64/bin/cmake.exe"
if [ ! -x "$CMAKE" ]; then
	(cd "$DEPS_DIR" && "$TAR_EXE" xf "downloads/cmake-$CMAKE_VER-windows-x86_64.zip")
fi
[ -x "$CMAKE" ] || { echo "CMake extraction failed." >&2; exit 1; }

win_path() { # /e/x or E:/x -> e:/x (forward slashes, drive-letter prefix)
	case "$1" in
		/[A-Za-z]/*)
			local d=${1:1:1} rest=${1:3}
			echo "$(echo "$d" | tr "a-z" "A-Z"):/$rest"
			;;
		*) echo "$1" ;;
	esac
}

if [ ! -d "$DEPS_DIR/zlib-$ZLIB_VER" ]; then
	(cd "$DEPS_DIR" && "$TAR_EXE" xf "downloads/zlib-$ZLIB_VER.tar.gz")
fi
if [ ! -d "$DEPS_DIR/curl-$CURL_VER" ]; then
	(cd "$DEPS_DIR" && "$TAR_EXE" xf "downloads/curl-$CURL_VER.tar.gz")
fi

GCC=$(win_path "$MINGW_ROOT/bin/gcc.exe")
AR=$(win_path "$MINGW_ROOT/bin/ar.exe")
RANLIB=$(win_path "$MINGW_ROOT/bin/ranlib.exe")
MAKE=$(win_path "$MINGW_ROOT/bin/mingw32-make.exe")
DEPS_WIN=$(win_path "$DEPS_DIR")

echo "Building static zlib $ZLIB_VER..."
"$CMAKE" -S "$DEPS_DIR/zlib-$ZLIB_VER" -B "$DEPS_DIR/zlib-build" \
	-G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER="$GCC" -DCMAKE_AR="$AR" -DCMAKE_RANLIB="$RANLIB" \
	-DCMAKE_MAKE_PROGRAM="$MAKE" >/dev/null
"$CMAKE" --build "$DEPS_DIR/zlib-build" --target zlibstatic -j"$JOBS" >/dev/null
mkdir -p "$DEPS_DIR/zlib-static/lib" "$DEPS_DIR/zlib-static/include"
cp "$DEPS_DIR/zlib-build/libzlibstatic.a" "$DEPS_DIR/zlib-static/lib/libz.a"
cp "$DEPS_DIR/zlib-$ZLIB_VER/zlib.h" "$DEPS_DIR/zlib-build/zconf.h" \
	"$DEPS_DIR/zlib-static/include/"

echo "Building static libcurl $CURL_VER (Schannel + zlib)..."
"$CMAKE" -S "$DEPS_DIR/curl-$CURL_VER" -B "$DEPS_DIR/curl-build" \
	-G "MinGW Makefiles" -DCMAKE_BUILD_TYPE=Release \
	-DCMAKE_C_COMPILER="$GCC" -DCMAKE_AR="$AR" -DCMAKE_RANLIB="$RANLIB" \
	-DCMAKE_MAKE_PROGRAM="$MAKE" \
	-DBUILD_SHARED_LIBS=OFF -DBUILD_STATIC_LIBS=ON -DBUILD_CURL_EXE=OFF \
	-DBUILD_EXAMPLES=OFF -DBUILD_TESTING=OFF -DENABLE_CURL_MANUAL=OFF \
	-DBUILD_LIBCURL_DOCS=OFF -DUSE_LIBIDN2=OFF -DUSE_NGHTTP2=OFF \
	-DCURL_USE_LIBPSL=OFF -DCURL_USE_LIBSSH2=OFF -DCURL_BROTLI=OFF \
	-DCURL_ZSTD=OFF -DCURL_USE_SCHANNEL=ON -DCURL_ZLIB=ON \
	-DZLIB_INCLUDE_DIR="$DEPS_WIN/zlib-static/include" \
	-DZLIB_LIBRARY="$DEPS_WIN/zlib-static/lib/libz.a" >/dev/null
"$CMAKE" --build "$DEPS_DIR/curl-build" --target libcurl_static -j"$JOBS" >/dev/null
mkdir -p "$DEPS_DIR/curl-static/lib"
cp "$DEPS_DIR/curl-build/lib/libcurl.a" "$DEPS_DIR/curl-static/lib/"
cp -a "$DEPS_DIR/curl-$CURL_VER/include/." "$DEPS_DIR/curl-static/include/"

rm -rf "$DEPS_DIR/zlib-build" "$DEPS_DIR/curl-build" \
	"$DEPS_DIR/zlib-$ZLIB_VER" "$DEPS_DIR/curl-$CURL_VER"
touch "$STAMP"
echo "Windows luaclib dependencies ready: $DEPS_DIR/{curl-static,zlib-static}"
