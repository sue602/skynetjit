#!/usr/bin/env bash
set -euo pipefail

export PATH="/bin:/usr/bin:$PATH"

ROOT_DIR=$(CDPATH= cd -- "$(dirname -- "$0")/.." && pwd -P)
cd "$ROOT_DIR"

git submodule sync -- third_party/skynet-mingw third_party/luajit2
git submodule update --init --checkout -- \
	third_party/skynet-mingw third_party/luajit2

echo "Updating skynet-mingw/master and luajit2/v2.1-agentzh..."
git submodule update --remote --checkout -- \
	third_party/skynet-mingw third_party/luajit2

git -C third_party/skynet-mingw submodule sync -- skynet
git -C third_party/skynet-mingw submodule update --init --checkout -- skynet

echo "Submodule revisions:"
git submodule status --recursive
