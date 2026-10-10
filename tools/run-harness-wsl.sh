#!/bin/sh
# redislib kernel harness runner -- native Linux gcc build + run.
#
# Invoke from the Windows/MSYS side as (paths must stay in Windows form for
# wsl.exe, hence the MSYS_NO_PATHCONV knobs; wsl output is UTF-16 so redirect
# to a file and read that):
#
#   MSYS_NO_PATHCONV=1 MSYS2_ARG_CONV_EXCL='*' \
#     wsl -d Ubuntu-24.04 -- bash /mnt/e/git/skynetjit/tools/run-harness-wsl.sh \
#     > tools/harness-out.txt 2>&1
#
# Success marker printed by the harness itself:
#   redislib harness OK: hll=<cardinality>
#
# Every .c under redislib-src/kernel/ is compiled as-is: the test mains of
# siphash.c and mt19937-64.c stay behind their upstream #ifdef guards
# (HASHKEY_TEST / MT19937_64_MAIN), so they cannot collide with the harness.
#
# Diagnostics deliberately silenced, both benign and documented in
# redislib-src/NOTICE:
#   -Wno-unused-variable      hyperloglog.c keeps the static error string
#                             `invalid_hll_err` after the PF*Command removal
#                             (Task 3 reuses that exact text).
#   -Wno-maybe-uninitialized  upstream false positive in dict.c rehash path.
#   -Wno-unused-parameter     robj/list stub signatures keep unused names.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
K="$ROOT/redislib-src/kernel"
OUT=${HARNESS_OUT:-/tmp/redislib-harness}
CC=${CC:-gcc}

# skynet_malloc.h lives in the checked-out skynet tree; pick whichever of the
# build staging dirs exists so the script works on both platform pipelines.
for d in "$ROOT/build/work/skynet/skynet-src" \
         "$ROOT/build/linux-work/skynet/skynet-src" \
         "$ROOT/third_party/skynet-mingw/skynet/skynet-src"; do
    if [ -f "$d/skynet_malloc.h" ]; then SKYNET_SRC=$d; break; fi
done
if [ -z "${SKYNET_SRC:-}" ]; then
    echo "run-harness-wsl.sh: skynet_malloc.h not found; run a build first" >&2
    exit 1
fi

SRC=""
for f in "$K"/*.c; do SRC="$SRC $f"; done

$CC -std=gnu11 -g -O1 -Wall -Wno-unused-variable -Wno-unused-parameter \
    -Wno-maybe-uninitialized \
    -I"$K" -I"$SKYNET_SRC" \
    -o "$OUT" "$ROOT/tools/redislib-harness.c" $SRC -lm

echo "== running $OUT =="
"$OUT"
echo "== harness exit status: 0 =="
