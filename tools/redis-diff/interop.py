#!/usr/bin/env python3
"""interop.py -- redis-diff: spec 4's round trip, in both directions.

    python3 interop.py export --out work/interop-from-redis.json
    python3 interop.py import --redis work/interop-from-redis.json \
                              --lib work/interop-from-lib.json \
                              --report work/interop-lib-report.json

`export` builds an HLL and a string value on a real server and writes the bytes
`GET` returns; the library then loads them (interop_lib.lua). `import` takes the
bytes the library produced, hands them to the server -- SET for a string, a parsed
ZADD replay for the sorted-set and geoset blobs, which is exactly what spec 4
promises a foreign reader can do -- and checks that both sides count the same
things. Every check is printed; any failure makes the exit code nonzero.
"""
import argparse
import json
import struct
import sys

from resp2 import Resp, as_hex

HLL_KEY = "interop:hll"
STR_KEY = "interop:str"
ZSET_KEY = "interop:zset"
GEO_KEY = "interop:geo"


def blob_records(blob_hex):
    """Parse spec 4's sorted-set blob: uint32 count, then float64 score, uint32
    length and that many member bytes, little-endian. struct is enough -- which is
    the point: nothing Redis-specific is needed to read it."""
    blob = bytes.fromhex(blob_hex)
    (count,) = struct.unpack_from("<I", blob, 0)
    off, out = 4, []
    for _ in range(count):
        (score,) = struct.unpack_from("<d", blob, off)
        (mlen,) = struct.unpack_from("<I", blob, off + 8)
        member = blob[off + 12: off + 12 + mlen].decode("utf-8")
        out.append((score, member))
        off += 12 + mlen
    assert off == len(blob), "blob has trailing bytes"
    return out


def phase_export(a):
    r = Resp(a.host, a.port)
    r.cmd("FLUSHDB")
    for i in range(1, 401):
        r.cmd("PFADD", HLL_KEY, "e%d" % i)
    r.cmd("SETBIT", STR_KEY, 500, 1)
    r.cmd("SETBIT", STR_KEY, 7, 1)
    # PFCOUNT caches its estimate in the value, so read the bytes after it: that is
    # the shape a sketch has once anybody has asked what is in it.
    hll_count = r.cmd("PFCOUNT", HLL_KEY)
    out = {
        "hll_count": hll_count,
        "hll_hex": as_hex(r.cmd("GET", HLL_KEY)),
        "bitmap_hex": as_hex(r.cmd("GET", STR_KEY)),
        "bitmap_bitcount": r.cmd("BITCOUNT", STR_KEY),
        "bitmap_bitpos": r.cmd("BITPOS", STR_KEY, 1),
    }
    with open(a.out, "w") as f:
        json.dump(out, f)
    print("interop.py export: server values written to %s" % a.out)
    return 0


def phase_import(a):
    lib = json.load(open(a.lib))
    redis_side = json.load(open(a.redis))
    report = json.load(open(a.report))
    r = Resp(a.host, a.port)
    r.cmd("FLUSHDB")

    checks = []

    def check(name, got, want, extra=""):
        checks.append((name, got == want, got, want, extra))

    # 1. a blob made here, imported by the server: the sketch counts the same
    r.cmd("SET", HLL_KEY, bytes.fromhex(lib["hll_hex"]), "KEEPTTL")
    check("server PFCOUNT of a library sketch", r.cmd("PFCOUNT", HLL_KEY),
          lib["hll_count"])
    # and reading it back gives the very bytes the library wrote
    # PFCOUNT above may have refreshed the cache; the bytes still have to be the
    # ones the library wrote, because both sides warm their cache the same way
    check("server GET returns those bytes", as_hex(r.cmd("GET", HLL_KEY)),
          lib["hll_hex"])

    # 2. the string value, both directions
    r.cmd("SET", STR_KEY, bytes.fromhex(lib["bitmap_hex"]))
    check("server BITCOUNT of a library string", r.cmd("BITCOUNT", STR_KEY),
          lib["bitmap_bitcount"])
    check("server GET == library dump", as_hex(r.cmd("GET", STR_KEY)),
          lib["bitmap_hex"])

    # 3. the sorted-set blob: parse it and replay with ZADD, the documented route
    records = blob_records(lib["zset_dump"])
    for score, member in records:
        r.cmd("ZADD", ZSET_KEY, repr(score), member)
    check("server ZCARD after replaying a library blob", r.cmd("ZCARD", ZSET_KEY),
          len(records))
    got = r.cmd("ZRANGE", ZSET_KEY, 0, -1, "WITHSCORES")
    check("server ZRANGE WITHSCORES == the blob's records",
          [(float(got[i + 1]), got[i]) for i in range(0, len(got), 2)], records)

    # 4. the geoset blob replays the same way, and the positions still answer
    for score, member in blob_records(lib["geo_dump"]):
        r.cmd("ZADD", GEO_KEY, "%d" % int(score), member)
    check("server ZCARD after replaying a geoset blob", r.cmd("ZCARD", GEO_KEY),
          len(blob_records(lib["geo_dump"])))
    check("server GEODIST of the replayed geoset",
          r.cmd("GEODIST", GEO_KEY, "Palermo", "Catania", "km"),
          "%.4f" % lib["geo_dist"])

    # 5. what the library made of the server's bytes (interop_lib.lua's report)
    check("library PFCOUNT of a server sketch", report["hll_loaded_count"],
          redis_side["hll_count"])
    check("library load() keeps the server's bytes", report["hll_reparse_same"],
          True)
    check("both sides cache the same bytes", report["hll_counted_hex"],
          redis_side["hll_hex"])
    check("library BITCOUNT of a server string", report["bitmap_bitcount"],
          redis_side["bitmap_bitcount"])
    check("library BITPOS 1 of a server string", report["bitmap_bitpos"],
          redis_side["bitmap_bitpos"])
    check("library BITPOS 0 of a server string", report["bitmap_bitpos0"], 0)
    check("library load() keeps the string bytes", report["bitmap_reparse_same"],
          True)

    bad = [c for c in checks if not c[1]]
    for name, ok, got, want, extra in checks:
        print("%-4s %-46s %s" % ("PASS" if ok else "FAIL", name,
                                 "" if ok else "got %r want %r %s" % (got, want, extra)))
    print("interop: %d checks, %d failed" % (len(checks), len(bad)))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6399)
    sub = ap.add_subparsers(dest="phase", required=True)

    e = sub.add_parser("export")
    e.add_argument("--out", required=True)
    i = sub.add_parser("import")
    i.add_argument("--redis", required=True)
    i.add_argument("--lib", required=True)
    i.add_argument("--report", required=True)

    a = ap.parse_args()
    return phase_export(a) if a.phase == "export" else phase_import(a)


if __name__ == "__main__":
    sys.exit(main())
