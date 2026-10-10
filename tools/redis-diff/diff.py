#!/usr/bin/env python3
"""diff.py -- redis-diff step 5: compare the two result logs and explain every
difference.

    python3 diff.py redis-results.jsonl lib-results.jsonl [--max 40] [--show N]

Exit code is the number of differing records, so a run is a check, not a printout.

Equivalence rules, in one place, each with the reason it exists:

  * a number and a numeric string are compared with a tolerance (1e-9 relative).
    Redis sends a double as a bulk string on the wire; spec 3.2 maps it to a Lua
    number, so the two files legitimately hold different JSON types for one value.
  * null and "" are the same state for a bitmap: an untouched key does not exist
    on the server, and an untouched handle holds the empty string here (NOTICE 6,
    "an object that empties out stays addressable instead of being deleted").
  * `{}` and `[]` are the same empty sequence: an empty Lua table encodes as an
    object with this cjson build, and the server's empty array decodes as one.
  * error texts are compared exactly, including the `ERR `/`WRONGTYPE ` code,
    because that is the contract rl_error() reproduces (NOTICE 7).
  * everything else must match byte for byte.
"""
import argparse
import json
import math
import sys

TOL = 1e-9


def numeric(v):
    if isinstance(v, bool):
        return None
    if isinstance(v, (int, float)):
        return float(v)
    if isinstance(v, str):
        try:
            return float(v)
        except ValueError:
            return None
    return None


def equal(a, b, path, out):
    """Compare one value pair; append a reason to `out` when they differ."""
    if a is None and (b is None or b == ""):
        return
    if b is None and (a is None or a == ""):
        return
    if a == {} or b == {}:
        # empty sequence: the server replies an empty array, an empty Lua table
        # encodes as an object (see replay_lib.lua). Same "nothing".
        if (not a) or (not b):
            return
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            out.append("%s: length %d vs %d" % (path, len(a), len(b)))
            return
        for i, (x, y) in enumerate(zip(a, b)):
            equal(x, y, "%s[%d]" % (path, i), out)
        return
    if isinstance(a, dict) and isinstance(b, dict):
        if set(a) != set(b):
            out.append("%s: keys %s vs %s" % (path, sorted(a), sorted(b)))
            return
        for k in a:
            equal(a[k], b[k], "%s.%s" % (path, k), out)
        return
    na, nb = numeric(a), numeric(b)
    if isinstance(a, bool) or isinstance(b, bool):
        na = nb = None                      # booleans are values, not numbers
    if na is not None and nb is not None:
        # Both sides hold a number, in whatever JSON shape they got it: the wire
        # formats a double as text and an integer reply as an int, while the
        # library answers a Lua number (spec 3.2), and LuaJIT's number IS a double
        # -- so an i64 BITFIELD answer cannot be exact there (README, known
        # boundaries). Compared with a relative tolerance, 1e-9.
        if math.isclose(na, nb, rel_tol=TOL, abs_tol=TOL):
            return
        out.append("%s: %r != %r" % (path, a, b))
        return
    if type(a) is not type(b) and not (a is None or b is None):
        out.append("%s: type %s vs %s (%r / %r)" %
                   (path, type(a).__name__, type(b).__name__, a, b))
        return
    if a != b:
        out.append("%s: %r != %r" % (path, a, b))


def load(path):
    recs = {}
    with open(path) as f:
        for line in f:
            if line.strip():
                r = json.loads(line)
                recs[r["seq"]] = r
    return recs


def classify(a, b, reasons):
    """Name the rule that makes a whole record's difference *expected*, or None.

    Two interface facts are documented in NOTICE 6 and can never line up with a
    server, so they are reported separately instead of being compared away:

      no-keyspace        -- PFDEBUG reads its key with lookupKeyWrite, and a key
                            that is not there is an error there. Here the object is
                            the receiver and always exists, so the command runs.
      no-command-table   -- Redis rejects a too-short command from the arity field
                            of its command table, before the handler; this port has
                            no command table, so the handler's own complaint is what
                            comes back. Both are errors, and the texts differ.

    Anything else, and any difference in the state probe, is unexplained.
    """
    if len(reasons) != 1 or not reasons[0].startswith("reply"):
        return None
    ra = a.get("reply")
    if isinstance(ra, dict) and "error" in ra:
        msg = ra["error"]
        if msg == "ERR The specified key does not exist":
            return "no-keyspace"
        if msg.startswith("ERR wrong number of arguments for"):
            return "no-command-table"
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("redis_log")
    ap.add_argument("lib_log")
    ap.add_argument("--max", type=int, default=40,
                    help="how many differing records to print in full")
    ap.add_argument("--show", type=int, default=6,
                    help="reasons printed per differing record")
    args = ap.parse_args()

    red, lib = load(args.redis_log), load(args.lib_log)
    if set(red) != set(lib):
        print("record sets differ: %d server / %d library" % (len(red), len(lib)))
        missing = sorted(set(red) - set(lib))[:5]
        extra = sorted(set(lib) - set(red))[:5]
        if missing:
            print("  only on the server at seq", missing)
        if extra:
            print("  only in the library at seq", extra)
        return 1

    by_cmd = {}
    diffs = []
    documented = {}
    for seq in sorted(red):
        a, b = red[seq], lib[seq]
        reasons = []
        # ZRANDMEMBER is Redis' own random sample: which members come back is a
        # set, not a sequence, and no reply may be demanded of it, so its reply is
        # not compared. The state probe below still is.
        if a["cmd"] != "zrandmember":
            equal(a.get("reply"), b.get("reply"), "reply", reasons)
        equal(a.get("state"), b.get("state"), "state", reasons)
        if not reasons:
            continue
        rule = classify(a, b, reasons)
        if rule:
            documented[rule] = documented.get(rule, 0) + 1
            continue
        key = "%s.%s" % (a["class"], a["cmd"])
        by_cmd[key] = by_cmd.get(key, 0) + 1
        diffs.append((seq, a, b, reasons))

    print("compared %d records" % len(red))
    print("unexplained differences: %d" % len(diffs))
    print("documented interface deviations: %d" % sum(documented.values()))
    for rule in sorted(documented):
        print("  %-20s %d" % (rule, documented[rule]))
    error_pairs = sum(1 for _, a, b, _ in diffs
                      if isinstance(a.get("reply"), dict) and "error" in a["reply"])
    print("of the unexplained, the server errored: %d" % error_pairs)
    if by_cmd:
        print("\nby command:")
        for k in sorted(by_cmd, key=lambda x: -by_cmd[x]):
            print("  %-24s %d" % (k, by_cmd[k]))
    print()
    for seq, a, b, reasons in diffs[:args.max]:
        print("seq %d  %s %s(%s)" % (seq, a["cmd"], a["key"],
                                     json.dumps(a.get("args", []), ensure_ascii=False)[:60]))
        print("   server: %s" % json.dumps(a.get("reply"), ensure_ascii=False)[:200])
        print("   library: %s" % json.dumps(b.get("reply"), ensure_ascii=False)[:200])
        for r in reasons[:args.show]:
            print("     - %s" % r[:200])
        if len(reasons) > args.show:
            print("     - (%d more)" % (len(reasons) - args.show))
    return len(diffs)


if __name__ == "__main__":
    sys.exit(main())
