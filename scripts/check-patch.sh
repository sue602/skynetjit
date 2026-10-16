#!/usr/bin/env bash
# 检查 patches/skynet-luajit.patch 在两个嵌套 skynet 修订上都还能应用：
# 一个是本仓库 pin 住的（CI 检出的就是它），一个是工作树里当前检出的
# （本机拉过 skynet-mingw/skynet 之后常常比 pin 的新）。这两者一旦分叉，
# 只在本地验证过的补丁会在 CI 上炸，而这条检查比构建快两个数量级。
set -uo pipefail

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
PATCH="$ROOT_DIR/patches/skynet-luajit.patch"
MINGW="$ROOT_DIR/third_party/skynet-mingw"
SKY="$MINGW/skynet"

[ -f "$PATCH" ] || { echo "no patch at $PATCH" >&2; exit 2; }
[ -d "$SKY" ] || { echo "nested skynet is not checked out; run: git submodule update --init --recursive" >&2; exit 2; }

pinned=$(git -C "$MINGW" ls-tree HEAD skynet | awk '{print $3}')
[ -n "$pinned" ] || { echo "cannot read the pinned skynet gitlink" >&2; exit 2; }
current=$(git -C "$SKY" rev-parse HEAD)

rc=0
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

for rev in "$current" "$pinned"; do
	short=$(git -C "$SKY" rev-parse --short "$rev" 2>/dev/null) || {
		echo "$rev MISSING (not fetched locally)"
		rc=1
		continue
	}
	if [ "$rev" = "$current" ]; then
		label="checked out"
	elif [ "$rev" = "$pinned" ]; then
		label="pinned (what CI builds)"
	else
		label="other"
	fi
	rm -rf "$tmp/tree"
	mkdir -p "$tmp/tree"
	if ! (cd "$SKY" && git archive --format=tar "$rev") | tar -xf - -C "$tmp/tree"; then
		echo "$short [$label] ARCHIVE-FAIL"
		rc=1
		continue
	fi
	if (cd "$tmp/tree" && git apply --check --ignore-space-change "$PATCH"); then
		echo "$short [$label] APPLY-OK"
	else
		echo "$short [$label] APPLY-FAIL"
		rc=1
	fi
done

exit "$rc"
