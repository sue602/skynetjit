#!/usr/bin/env python3
"""replay_redis.py -- redis-diff step 3: replay the oplog against a real
redis-server (RESP2 via resp2.py), recording one JSON line per op in the shape the
library replay produces.

    python3 replay_redis.py --port 6399 --oplog oplog.jsonl --out redis-results.jsonl

Nothing here interprets the results; diff.py decides what counts as equal, and its
rules are the documented RESP-vs-Lua mappings (spec 3.2), not per-case leniency.
"""
import argparse
import json
import sys


from resp2 import Resp, as_hex


# oplog command -> wire command. The log's `args` is the library's own argument
# list (after the receiver), so the multi-key commands are the only ones that need
# re-ordering: Redis wants the key first and a numkeys count before its list.
def run_one(r, op):
    cmd, key, args = op["cmd"].upper(), op["key"], op["args"]
    if cmd == "BITOP":
        opname, dest, srcs = args[0].upper(), args[1], args[2]
        return r.cmd("BITOP", opname, dest, *srcs)
    if cmd == "PFMERGE":
        dest, srcs = args[0], args[1]
        return r.cmd("PFMERGE", dest, *srcs)
    if cmd in ("ZUNIONSTORE", "ZINTERSTORE", "ZDIFFSTORE"):
        dest, srcs = args[0], args[1]
        rest, i = [], 2
        while i < len(args):
            a = args[i]
            if isinstance(a, str) and a.lower() == "weights":
                rest.append("WEIGHTS")
                rest.extend(args[i + 1])
                i += 2
            elif isinstance(a, str) and a.lower() == "aggregate":
                rest.append("AGGREGATE")
                rest.append(args[i + 1])
                i += 2
            else:
                rest.append(a)
                i += 1
        return r.cmd(cmd, dest, len(srcs), *srcs, *rest)
    if cmd == "GEOSEARCHSTORE":
        # the log's key is the source geoset; the destination is inside args, the
        # way it is for BITOP, and the wire wants them the other way round
        return r.cmd(cmd, args[0], key, *args[1:])
    if cmd == "ZRANGESTORE":
        # args[0] is the source key's name (a single name, not a list), the rest of
        # the arguments keep their order, and the wire wants the destination first
        return r.cmd(cmd, key, *args)
    if cmd in ("ZUNION", "ZINTER", "ZDIFF", "ZINTERCARD"):
        # the read forms take a source list; Redis wants numkeys in front of it
        srcs, rest, i = args[0], [], 1
        while i < len(args):
            if args[i] == "limit":
                rest += ["LIMIT", args[i + 1]]
                i += 2
            else:
                rest.append(args[i])
                i += 1
        return r.cmd(cmd, len(srcs), *srcs, *rest)
    if cmd == "ZMPOP":
        srcs = args[0]
        return r.cmd(cmd, len(srcs), *srcs, *args[1:])
    if cmd == "PFSELFTEST":
        return r.cmd(cmd)
    if cmd == "PFDEBUG":
        # PFDEBUG puts the subcommand before the key, while the library calls it
        # on the object: h:pfdebug("todense") is `PFDEBUG todense key` on the wire.
        return r.cmd(cmd, *args, key)
    # gen.lua does not generate the STORE / STOREDIST forms of GEORADIUS or
    # GEOSEARCHSTORE: their destination is a handle here and a key on the wire,
    # which is a mapping the smoke test already covers (NOTICE 6).
    return r.cmd(cmd, key, *args)


def probe(r, op):
    """State snapshot with commands that exist identically on both sides."""
    key, cls = op["key"], op["class"]
    if cls == "bitmap":
        value = as_hex(r.cmd("GET", key))
        bits = r.cmd("BITCOUNT", key)
        return {"value": value, "bits": bits if value is not None else 0}
    if cls == "hll":
        value = as_hex(r.cmd("GET", key))
        return {"count": r.cmd("PFCOUNT", key), "value": value}
    # a geoset IS a sorted set, so both classes snapshot the same way
    return {"range": r.cmd("ZRANGE", key, 0, -1, "WITHSCORES"),
            "card": r.cmd("ZCARD", key)}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--host", default="127.0.0.1")
    ap.add_argument("--port", type=int, default=6399)
    ap.add_argument("--oplog", required=True)
    ap.add_argument("--out", required=True)
    args = ap.parse_args()

    r = Resp(args.host, args.port)
    r.cmd("FLUSHDB")
    if r.cmd("PING") != "PONG":
        sys.exit("redis is not answering on %s:%d" % (args.host, args.port))

    n = 0
    with open(args.oplog) as f, open(args.out, "w") as out:
        for line in f:
            if not line.strip():
                continue
            op = json.loads(line)
            rec = {"seq": op["seq"], "class": op["class"], "key": op["key"],
                   "cmd": op["cmd"]}
            try:
                rec["reply"] = run_one(r, op)
            except Exception as exc:              # a transport failure is a finding
                rec["reply"] = {"fatal": "%s: %s" % (type(exc).__name__, exc)}
            if op.get("probe"):
                try:
                    rec["state"] = probe(r, op)
                except Exception as exc:
                    rec["state"] = {"fatal": "%s: %s" % (type(exc).__name__, exc)}
            out.write(json.dumps(rec, ensure_ascii=False, sort_keys=True) + "\n")
            n += 1
    print("replay_redis.py: %d ops -> %s" % (n, args.out))


if __name__ == "__main__":
    main()
