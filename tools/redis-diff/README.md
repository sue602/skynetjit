# redis-diff -- differential test of redislib against a real Redis

Replays one deterministic operation log twice: once against a `redis-server`
built from the very source tree this port was made against (8.10.0), once against
the `redislib` modules, and compares the two recordings.

It is **manual** on purpose. It needs a second build (the server) and a running
process, so it is not part of `build.sh`'s one-shot test -- that suite covers the
same commands against their own expectations (see `redislib-src/NOTICE` §5).

## Run it

Both scripts need Linux and are meant for WSL here (the port's Linux side):

```bash
./tools/redis-diff/run.sh [seed] [ops]      # command-by-command diff, default 1 3000
./tools/redis-diff/interop.sh               # spec §4 value round trip, both directions
```

Requirements they check rather than assume:

* a `redis-server` 8.10.0 binary. Build it in Linux's own filesystem -- WSL's
  `/tmp` disappears when the VM stops:

  ```bash
  mkdir -p ~/src && cp -r /mnt/d/download/redis-8.10.0 ~/src/
  make -C ~/src/redis-8.10.0 -j"$(nproc)" MALLOC=libc all
  ```

  or set `REDIS_SERVER=/path/to/redis-server`.
* a built `build/linux-out` (`./build-linux.sh --no-sync`).

Each script starts its own server on `127.0.0.1:6399` (`PORT=` to change) and
stops it by the PID it recorded, after checking `/proc/<pid>/cmdline`. Nothing is
written outside `work/`, which is git-ignored.

## What is compared

`gen.lua` writes ops over four key pools (bitmap / hll / zset / geo), mixing
writes, reads, deliberate failures, and the multi-key commands. A record is

```json
{"seq":41,"class":"zset","key":"zset_4","cmd":"zrange",
 "args":["0","-1","byscore","limit","0","3"],"probe":true}
```

`args` is the **library's** argument list after its receiver; `replay_redis.py`
adds what the wire form needs (the key first, a `numkeys` count for the
multi-key commands, `PFDEBUG subcommand key` order). So the two sides run the same
command through their own natural shape rather than a lowest common denominator.

Each replay records the reply and, when `probe` is set, a state snapshot taken
with commands that exist identically on both sides -- `GET`/`BITCOUNT` for a
string, `PFCOUNT`/`GET` for an HLL, `ZRANGE 0 -1 WITHSCORES`/`ZCARD` for a sorted
set and a geoset. Values are hex, so the bytes are compared, not a rendering.
The state probe is what covers commands whose replies are deliberately different
(`BITOP` answers a handle here and a length there).

`diff.py` compares with these rules, each tied to a documented mapping and nothing
else:

| rule | why |
|---|---|
| numbers compare with a 1e-9 relative tolerance | Redis sends a double as text on the wire; spec §3.2 maps it to a Lua number, and LuaJIT's number is a double |
| `[]` and `{}` are the same empty sequence | an empty Lua table encodes as an object with this cjson build |
| a missing/`null` and an empty string are the same bitmap state | an object that emptied out stays addressable here (NOTICE §6) |
| `ZRANDMEMBER`'s reply is not compared | it is Redis' own random sample; its *state* still is |
| error texts compare exactly, code included | `rl_error()` reproduces the client-visible text (NOTICE §7) |

Two record shapes are classified as **documented interface deviations** rather than
failures, and counted separately so a third kind can never hide in the bucket:

* `no-keyspace` -- `PFDEBUG` on a key that was never created: a server has no such
  key to read and errors; a handle exists, so the command runs. Deliberately **not
  generated**, because it changes the value on one side only and the difference then
  cascades into every later probe of that key; `gen.lua` restricts `PFDEBUG` to keys
  the log has written on both sides, and the smoke test asserts the deviation.
* `no-command-table` -- a command too short for Redis' `arity` field: the server
  rejects it before the handler, and this port has no command table, so the
  handler's own (different) error appears. Both sides error, and neither is wrong.

Anything else is an unexplained difference and makes the exit code nonzero.

## First run

`REPORT-2026-10-10.md` -- what the first 3000-op run found, and the five port bugs
it found with it.
