#!/usr/bin/env luajit
-- gen.lua -- redis-diff step 2: a deterministic operation log for replaying the
-- redislib modules and a real redis-server against each other.
--
--     luajit gen.lua <seed> <count> <out.jsonl>
--
-- One JSON object per line. Both replay programs read the SAME file, so what is
-- compared is one fixed sequence of calls replayed twice, not two random
-- sessions that happen to look similar.
--
-- Record shape:
--   {"seq":N,"class":"zset","key":"zset_3","cmd":"zadd","args":[1,"a"],"probe":true}
-- `key` is a logical name that keeps its class for the whole run: on the server it
-- IS the redis key, in the library it names the handle. `args` is the argument list
-- as the *library* method takes it (after the receiver), which is the shape with
-- the least ceremony; replay_redis.py adds what the wire form needs (a key first,
-- a numkeys count for the multi-key commands). A nested array in `args` is a list
-- of logical names.
--
-- `probe` says the replay must snapshot the state this op can have changed, with
-- read-only commands that exist identically on both sides -- see each replay's
-- probe section. That is how commands whose replies are deliberately different
-- (BITOP answers a handle here and a length there, ZMPOP a handle and a key name)
-- still get compared: through the state they left behind.

require "bit"
local bit = require "bit"
local bxor, blshift, brshift, band = bit.bxor, bit.lshift, bit.rshift, bit.band

local seed = tonumber(arg[1]) or 1
local count = tonumber(arg[2]) or 2000
local out = arg[3] or "oplog.jsonl"

-- xorshift32, so the log can be regenerated and audited in any language.
local st = seed ~= 0 and (seed % 0xFFFFFFFF) or 1
local function rnd()
	st = bxor(st, blshift(st, 13)) % 0x100000000
	st = bxor(st, brshift(st, 17))
	st = bxor(st, blshift(st, 5)) % 0x100000000
	return st
end
local function pick(n) return rnd() % n + 1 end
local function one_of(t) return t[pick(#t)] end

local function esc(s)
	return (s:gsub('[%c"\\]', function(c)
		if c == '"' then return '\\"' end
		if c == '\\' then return '\\\\' end
		if c == '\n' then return '\\n' end
		if c == '\t' then return '\\t' end
		if c == '\r' then return '\\r' end
		return string.format('\\u%04x', c:byte())
	end))
end

local function jval(v)
	if type(v) == "number" then
		if v ~= v then return '"nan"' end          -- never emitted; a guard, not a case
		if v == math.huge then return '"inf"' end
		if v == -math.huge then return '"-inf"' end
		if v == math.floor(v) and math.abs(v) < 1e15 then return string.format("%d", v) end
		return string.format("%.17g", v)
	elseif type(v) == "table" then
		local p = {}
		for _, k in ipairs(v) do p[#p+1] = '"' .. k .. '"' end
		return "[" .. table.concat(p, ",") .. "]"
	end
	return '"' .. esc(tostring(v)) .. '"'
end

local function jargs(a)
	local p = {}
	for _, v in ipairs(a) do
		if v ~= nil then p[#p+1] = jval(v) end
	end
	return "[" .. table.concat(p, ",") .. "]"
end

local CLASSES = { "bitmap", "hll", "zset", "geo", "set" }

-- Which keys exist ON THE SERVER. Everything else about the two sides is
-- symmetric, but a key no write has touched does not exist there while the handle
-- exists here, and PFDEBUG reads the key itself (it has no empty-key answer).
-- Running it on such a key makes the server refuse while the library converts its
-- object -- and that difference then cascades into every later probe of that key,
-- which would bury any real finding. So PFDEBUG is generated only for keys the log
-- has already written on both sides; the deviation itself stays documented in
-- NOTICE 6 and asserted in the smoke test.
local written = {}
local WRITES = {
	bitmap = { setbit = true, bitfield = true, bitop = true },
	hll = { pfadd = true, pfmerge = true },
	zset = { zadd = true, zincrby = true, zpopmin = true, zpopmax = true,
	         zmpop = true, zrangestore = true, zunionstore = true,
	         zinterstore = true, zdiffstore = true },
	geo = { geoadd = true, geosearchstore = true },
	set = { sadd = true, srem = true, spop = true, smove = true },
}
local pool = {}
for _, c in ipairs(CLASSES) do
	pool[c] = {}
	for i = 1, 6 do pool[c][i] = c .. "_" .. i end
end
local key_of = function(c) return one_of(pool[c]) end

-- The set pool, split by what its members do to the payload, and the rules that
-- split is there to keep.
--
-- SPOP takes members out at random, and the two sides cannot draw the same ones:
-- the kernel samples through random(), which Redis seeds per process and this port
-- seeds from getRandomBytes() (NOTICE 6's rand() note), so which members leave is
-- a correct answer twice over and never the same answer. Every later reply about
-- that key would then differ -- and a cascade of legitimate differences is exactly
-- how a real finding gets buried, the same reason PFDEBUG stays off untouched keys.
-- A set that was emptied by a draw therefore never happens here: SPOP and
-- SRANDMEMBER are generated with count 0, with a count above any size these keys
-- can reach (Redis' whole-payload case, which walks the payload instead of
-- drawing), or on a set the log has just put in a known state with exactly one
-- member, where a random draw has one answer. See set_forge() and set_scripted().
--
-- The other half is order. SMEMBERS and the whole-payload cases answer the
-- payload's own order, which is reproducible for an intset (ascending value) and
-- for a listpack (insertion order), because both sides walk it the same way from
-- the same history -- but not for a hashtable, whose bucket order comes from a
-- hash seed that is random per process. SUNION and SDIFF build a *new* payload and
-- pick a hashtable as soon as one operand is not an intset, so they only ever take
-- the integer keys (set_1..set_3, intset by construction). SINTER answers by
-- walking its smallest operand and builds nothing, so both encodings are
-- comparable there.
--
-- The three CARDINALITY forms answer an integer, so order is not their problem --
-- their wire form is. `SINTERCARD numkeys key [key ...]` puts a count where
-- replay_redis.py's default arm can only put the record's key, and that file is
-- not part of this change, so the oplog carries only the failure text the two
-- shapes share and tools/redis-diff/interop.sh compares what those three commands
-- actually answer.
--
-- One consequence of the same reading: a set record never sets `probe`. The state
-- snapshot both replays share is GET/BITCOUNT, PFCOUNT and ZRANGE/ZCARD, and every
-- one of those is a WRONGTYPE against a set key. The set family is compared
-- through its replies instead, which is why SCARD/SMEMBERS/SISMEMBER/SMISMEMBER
-- are generated often: they *are* the snapshot.
local SET_INT = { pool.set[1], pool.set[2], pool.set[3] }
local SET_LP = { pool.set[4], pool.set[5], pool.set[6] }
local IS_SET_INT = { [pool.set[1]] = true, [pool.set[2]] = true, [pool.set[3]] = true }
-- integer members only, so the payload stays an intset; "-5" checks the negative
-- side of the intset's int64 range and "100" its ordering (100 after 12, not after
-- "7" as a byte compare would put it)
local SET_INT_MEMBERS = { "1", "2", "3", "7", "12", "42", "100", "-5" }
-- no member of either list parses as an integer, or the key it lands in would
-- start out an intset and the split above would be a lie
local SET_LP_MEMBERS = { "a", "b", "member with space", "Omega", "x\ty", "z\nw",
                         "quote\"x", "back\\slash" }
-- A count no key of this log can reach, so SPOP and SRANDMEMBER always take their
-- "the whole payload" branch and never their drawing one. Both sides parse it the
-- same way: Redis with getPositiveLongFromObjectOrReply() / getRangeLongFromObject-
-- OrReply(), the facade with the two same-named readers of redislib-src/facade/set.c.
local SET_ALL = "1000000"

-- members stay stable across the run so ordering is comparable; the odd ones
-- check binary-safety through JSON, the server and the library.
local MEMBERS = { "a", "b", "c", "d", "e", "member with space", "17", "-3.5",
                  "Omega", "x\ty", "z\nw", "quote\"x", "back\\slash" }
local member = function() return one_of(MEMBERS) end

local SICILY = { { 13.361389, 38.115556, "Palermo" },
                 { 15.0872689, 37.502669, "Catania" },
                 { 15.087269, 37.502669, "Catania2" },
                 { -179.99, 0, "west" }, { 179.99, 0, "east" },
                 { 0, 0, "greenwich" }, { 116.4898586, 39.9167148, "SF" },
                 { 2.191, 41.9, "skopje" } }

-- Failures with a text both sides must produce; the error contract is the part a
-- migration notices first, so it is asserted as a first-class case, not skipped.
-- Only failures that the two call shapes can both express are here: the log
-- always names a receiver key, so "too few arguments" on the library side is a
-- wire-valid command on the server. Wrong type, wrong value and bad option
-- combinations are the failures that mean the same thing on either side.
--
-- The fifth field, when present, names the key to put in the record instead of one
-- drawn from the entry's own class pool. It is how the set family's type check gets
-- compared: `SADD geo_1 x` is a WRONGTYPE on the server because geo_1 holds a
-- sorted set, and the same WRONGTYPE here because handles are keyed by name, so the
-- set function reaches a geoset object. Such a row needs a key that exists on both
-- sides for the whole run, which is what the rows below say how to pick: a name that
-- was not there yet would make the library create a set and the server create a key,
-- and both sides would then be wrong about every later record that touches it.
local BAD = {
	{ "bitmap", "setbit",    { "notanumber", 1 }, false },
	{ "bitmap", "bitcount",  { "0", "-1", "BYTE|BIT" }, false },
	{ "bitmap", "bitfield",  { "get", "u64", "notanumber" }, false },
	{ "bitmap", "bitfield_ro", { "get", "u8", "notanumber" }, false },
	{ "hll",    "pfdebug",   { "nosuch" }, false },
	{ "zset",   "zadd",      { "notanumber", "a" }, false },
	{ "zset",   "zadd",      { "nx", "xx", 1, "a" }, false },
	{ "zset",   "zrange",    { 0, -1, "bylex", "withscores" }, false },
	{ "zset",   "zrangebyscore", { 1, 3, "limit", 0 }, false },
	{ "zset",   "zrange",    { "notarange", 1 }, false },
	{ "geo",    "geoadd",    { 200, 37, "bad" }, false },
	{ "geo",    "georadius", { 15, 37, -1, "km" }, false },
	{ "geo",    "georadius", { 15, 37, 1, "furlong" }, false },
	{ "geo",    "geosearch", { "fromlonlat", 15, 37 }, false },
	{ "geo",    "geopos",    { "nosuchmember" }, true },
	{ "set",    "sadd",      {}, false },                   -- arity -3 on both sides
	{ "set",    "srem",      {}, false },
	{ "set",    "smismember", {}, false },
	{ "set",    "sismember", { "x", "y" }, false },          -- arity 3, exactly
	{ "set",    "smembers",  { "x" }, false },
	{ "set",    "scard",     { "x" }, false },
	{ "set",    "smove",     { "set_5" }, false },           -- the member is missing
	{ "set",    "spop",      { "1", "2" }, false },          -- too many arguments
	{ "set",    "spop",      { "notanumber" }, false },      -- one text for both failures
	{ "set",    "spop",      { "-1" }, false },
	{ "set",    "srandmember", { "notanumber" }, false },
	{ "set",    "srandmember", { "1", "2" }, false },
	-- the CARDINALITY family: the only failure a record can express for it on both
	-- sides, an operand list with nothing in it. The nested empty list is the whole
	-- argument: here it is `setnum == 0` ("numkeys should be greater than 0"), on the
	-- wire it is the count slot holding a key name, which Redis refuses with that same
	-- sentence, and the extra element keeps the command past its own arity check.
	{ "set",    "sintercard", { {} }, false },
	{ "set",    "sunioncard", { {} }, false },
	{ "set",    "sdiffcard",  { {} }, false },
	-- the wrong kind of handle, at the three places a set command reads a key. Only
	-- geo_1 and hll_1 serve here, because they are the two foreign keys this log can
	-- promise exists for the whole run: GEOADD never empties a geoset and a merged
	-- sketch is never an empty string, while bitmap_1 can be deleted under a BITOP
	-- that produces an empty string and zset_1 under a store command whose result is
	-- empty -- and a key that is simply absent answers 0 or the empty array instead
	-- of the WRONGTYPE this row is here to compare.
	{ "set",    "sadd",      { "x" }, false, "geo_1" },
	{ "set",    "smembers",  {}, false, "geo_1" },
	{ "set",    "scard",     {}, false, "hll_1" },
}

local fh = assert(io.open(out, "wb"))
local seq = 0

local function emit(class, key, cmd, args, probe)
	seq = seq + 1
	fh:write(string.format(
		'{"seq":%d,"class":"%s","key":"%s","cmd":"%s","args":%s,"probe":%s}\n',
		seq, class, key, cmd, jargs(args), probe and "true" or "false"))
end

-- ---- the set family's own generators ------------------------------------
-- They name more than one key, so they call emit() directly the way the multi-key
-- writers above do. `key` is the first operand and `args` the rest, flat: the wire
-- form of SINTER / SUNION / SDIFF is the original `SINTER key [key ...]` with no
-- numkeys in front of it, so unlike ZUNION's operand list it needs no nesting and
-- replay_redis.py's default arm sends it as written.
local function slice(t, i, j)
	local out = {}
	for k = i, j do out[#out+1] = t[k] end
	return out
end
local set_pool_of = function(key)
	return IS_SET_INT[key] and SET_INT_MEMBERS or SET_LP_MEMBERS
end

-- n distinct keys of one pool, in the order the record wants them
local function set_srcs(nums, n)
	local g = nums and SET_INT or SET_LP
	local out, used = {}, {}
	while #out < n do
		local k = g[pick(#g)]
		if not used[k] then
			used[k] = true
			out[#out+1] = k
		end
	end
	return out
end

local function emit_setop(cmd, nums, n)
	local srcs = set_srcs(nums, n)
	local args = {}
	for i = 2, #srcs do args[i - 1] = srcs[i] end
	emit("set", srcs[1], cmd, args, false)
end

-- A key in a state the log knows: clear every member this file can ever add to it,
-- then bring back the ones named. The clearing answer is a count both sides reach
-- from the same content, so nothing can diverge, and Redis' delete of an emptied
-- key is the same state as an emptied handle (NOTICE 6's object model). After this
-- the key holds exactly `ms`, which is what lets the sequence below ask for the
-- draws that have exactly one answer.
local function set_forge(key, ms)
	local all = set_pool_of(key)
	emit("set", key, "srem", slice(all, 1, #all), false)
	if ms and #ms > 0 then
		emit("set", key, "sadd", ms, false)
		written[key] = true
	end
end

-- The scripted exchange: forge two keys, then read and combine them. SPOP's
-- whole-payload case, its "nothing to pop" answer and the draw with one possible
-- result are all reached here, because the state in front of each of them is a fact
-- of the log rather than a history the two sides happen to share.
local function set_scripted(nums)
	local m = nums and SET_INT_MEMBERS or SET_LP_MEMBERS
	local srcs = set_srcs(nums, 2)
	local a, b = srcs[1], srcs[2]
	set_forge(a, slice(m, 1, 5))
	set_forge(b, slice(m, 3, 7))
	emit("set", a, "smembers", {}, false)
	emit("set", a, "sinter", { b }, false)
	if nums then
		-- intset operands leave an intset result, so SUNION and SDIFF answer in
		-- ascending value order on either side, and SPOP's whole-payload case --
		-- which Redis serves by running its own union of one key -- walks the same
		-- payload the facade iterates directly.
		emit("set", a, "sunion", { b }, false)
		emit("set", a, "sdiff", { b }, false)
		emit("set", b, "sdiff", { a }, false)
		emit("set", a, "spop", { SET_ALL }, false)
		emit("set", a, "spop", {}, false)              -- nothing left: nil on both sides
		emit("set", a, "sadd", { m[8] }, false)        -- one member: a draw with one answer
		emit("set", a, "spop", {}, false)
	else
		-- A listpack payload is walked in insertion order, which SMEMBERS and
		-- SRANDMEMBER's whole-payload case both show. SPOP's CASE 1 is deliberately
		-- not here: Redis answers it through sunionDiffGenericCommand(), and the
		-- payload that builds is a hashtable as soon as its operand is not an intset,
		-- so the two sides would order the same members differently by design.
		set_forge(a)                                   -- and now it holds nothing
		emit("set", a, "sadd", { m[7] }, false)
		emit("set", a, "spop", {}, false)              -- one member: a draw with one answer
		emit("set", a, "srandmember", {}, false)       -- nothing left: nil on both sides
		emit("set", a, "sadd", { m[1], m[2], m[3] }, false)
		emit("set", a, "srandmember", { SET_ALL }, false)
		emit("set", a, "smembers", {}, false)
		emit("set", a, "smismember", { m[1], m[7], m[4] }, false)
		emit("set", b, "smove", { a, m[3] }, false)    -- across two listpack payloads
	end
	emit("set", a, "sismember", { m[1] }, false)
	emit("set", a, "scard", {}, false)
end

local GEN = {
	bitmap = function()
		local c = pick(9)
		if c == 1 then return "setbit", { tostring(rnd() % 2000), pick(2) - 1 }, true
		elseif c == 2 then return "getbit", { tostring(rnd() % 2000) }, false
		elseif c == 3 then return "bitcount", {}, true
		elseif c == 4 then return "bitcount", { tostring(rnd() % 500), tostring(rnd() % 1000) }, false
		elseif c == 5 then return "bitcount", { "0", "64", one_of({ "BYTE", "BIT" }) }, false
		elseif c == 6 then return "bitpos", { pick(2) - 1 }, false
		elseif c == 7 then return "bitpos", { 1, "0", "8" }, false
		elseif c == 8 then return "bitfield", { "set", "u8", tostring(rnd() % 200), tostring(rnd() % 256) }, true
		else return "bitfield", { "incrby", one_of({ "i8", "u16", "i64", "u63" }),
		                          tostring(rnd() % 40), tostring(rnd() % 700 - 350) }, true end
	end,
	hll = function(key)
		local c = pick(6)
		if c <= 3 then
			local e = {}
			for i = 1, pick(5) do e[#e+1] = "e" .. tostring(rnd() % 3000) end
			return "pfadd", e, true
		elseif c == 4 then return "pfcount", {}, false
		elseif not written[key] then
			return "pfadd", { "warm" .. tostring(rnd() % 999) }, true
		elseif c == 5 then return "pfdebug", { "encoding" }, false
		else return "pfdebug", { "todense" }, true end
	end,
	zset = function()
		local c = pick(28)
		if c == 1 then
			local a = {}
			for i = 1, pick(4) do a[#a+1] = (rnd() % 2000) / 4 - 500; a[#a+1] = member() end
			return "zadd", a, true
		elseif c == 2 then return "zadd", { "nx", tostring(rnd() % 10), member() }, true
		elseif c == 3 then return "zrem", { member(), member() }, true
		elseif c == 4 then return "zscore", { member() }, false
		elseif c == 5 then return "zcard", {}, false
		elseif c == 6 then return "zrange", { "-5", "5", "withscores" }, false
		elseif c == 7 then return "zrange", { "0", "-1", "byscore", "limit", "0", "3" }, false
		elseif c == 8 then return "zrangebyscore", { tostring(rnd() % 100 - 50), tostring(rnd() % 100), "limit", 0, tostring(rnd() % 5) }, false
		elseif c == 9 then return "zrevrangebyscore", { "+inf", "-inf", "withscores" }, false
		elseif c == 10 then return "zincrby", { tostring(rnd() % 50 - 25), member() }, true
		elseif c == 11 then return "zrank", { member() }, false
		elseif c == 12 then return "zcount", { "(1", "(3" }, false
		elseif c == 13 then return "zremrangebyrank", { 0, tostring(rnd() % 3) }, true
		elseif c == 14 then return "zremrangebyscore", { tostring(rnd() % 20 - 10), tostring(rnd() % 30) }, true
		elseif c == 15 then return "zpopmin", {}, true
		elseif c == 16 then return "zpopmax", { "2" }, true
		elseif c == 17 then return "zrandmember", { "3", "withscores" }, false
		-- the rest of the family: the other reading forms, the lex bounds, the
		-- store/read set operations, and the replies that name a key
		elseif c == 18 then return "zrevrange", { "0", "-1", "withscores" }, false
		elseif c == 19 then return "zrevrank", { member() }, false
		elseif c == 20 then return "zmscore", { member(), member() }, false
		elseif c == 21 then return "zlexcount", { "[b", "[d" }, false
		elseif c == 22 then return "zrangebylex", { "-", "+" }, false
		elseif c == 23 then return "zrevrangebylex", { "[c", "(a", "limit", "0", "2" }, false
		elseif c == 24 then return "zremrangebylex", { "(a", "[c" }, true
		elseif c == 25 then return "zrangebyscore", { "-inf", "+inf", "withscores" }, false
		elseif c == 26 then return "zrange", { "0", "-1", "withscores" }, false
		elseif c == 27 then return "zcount", { "-inf", "+inf" }, false
		else return "zcard", {}, false end
	end,
	geo = function()
		local c = pick(14)
		if c <= 3 then
			local p = SICILY[pick(#SICILY)]
			return "geoadd", { p[1], p[2], p[3] }, true
		elseif c == 4 then return "geodist", { "Palermo", "Catania", one_of({ "m", "km", "mi", "ft" }) }, false
		elseif c == 5 then return "geodist", { "Palermo", "nosuch" }, false
		elseif c == 6 then return "geopos", { "Palermo", "Catania", "nosuch" }, false
		elseif c == 7 then return "geohash", { "Palermo", "SF" }, false
		elseif c == 8 then return "georadius", { "15", "37", tostring(rnd() % 200), one_of({ "km", "m", "mi", "ft" }), "withdist", "withcoord", "asc", "count", "3" }, false
		elseif c == 9 then return "georadiusbymember", { "Palermo", "100", "km", "desc", "count", "2" }, false
		elseif c == 10 then return "geosearch", { "fromlonlat", "15", "37", "byradius", tostring(rnd() % 300), "km", "withcoord" }, false
		elseif c == 11 then return "geosearch", { "frommember", "Catania", "bybox", "100", "100", "km", "asc", "count", "2" }, false
		elseif c == 12 then return "georadius_ro", { "15", "37", "150", "km", "withdist", "asc" }, false
		elseif c == 13 then return "geosearch", { "frommember", "Palermo", "byradius", "200", "km", "withhash" }, false
		else return "georadiusbymember_ro", { "Catania", "200", "mi", "desc" }, false end
	end,
	-- Every set record answers false for the probe: see the note above the set pools.
	-- The reads are generated often because they are the snapshot this family has.
	set = function(key)
		local nums = IS_SET_INT[key]
		local m = function() return one_of(nums and SET_INT_MEMBERS or SET_LP_MEMBERS) end
		local c = pick(10)
		if c == 1 then
			local a = {}
			for i = 1, pick(4) do a[#a+1] = m() end
			return "sadd", a, false
		elseif c == 2 then
			local a = {}
			for i = 1, pick(3) do a[#a+1] = m() end
			return "srem", a, false
		elseif c == 3 then return "scard", {}, false
		elseif c == 4 then return "smembers", {}, false
		elseif c == 5 then return "sismember", { m() }, false
		elseif c == 6 then
			local a = {}
			for i = 1, pick(4) do a[#a+1] = m() end
			return "smismember", a, false
		elseif c == 7 then
			-- SMOVE names its destination as an argument on both sides, and it stays
			-- inside this key's own pool: a member that only one encoding can hold
			-- would make the two payloads differ from that record on.
			local g = nums and SET_INT or SET_LP
			return "smove", { one_of(g), m() }, false
		elseif c == 8 then
			-- a count above anything these keys reach: Redis' "return the whole set"
			-- case, which walks the payload instead of drawing from it
			return "srandmember", { SET_ALL }, false
		elseif c == 9 then return "spop", { "0" }, false     -- the count-0 fast return
		else return "scard", {}, false end
	end,
}

-- Multi-key commands: the destination first, then the source list as a nested
-- array, then the optional WEIGHTS / AGGREGATE words. Each replay knows where the
-- wire form wants the numkeys.
local function emit_multi(class, dest, cmd, args, probe)
	emit(class, dest, cmd, args, probe)
end

local function warm()
	emit("bitmap", pool.bitmap[1], "setbit", { 1000, 1 }, true)
	emit("bitmap", pool.bitmap[1], "setbit", { 7, 1 }, true)
	emit("bitmap", pool.bitmap[2], "setbit", { 3, 1 }, true)
	emit("bitmap", pool.bitmap[3], "setbit", { 3, 1 }, true)
	emit("bitmap", pool.bitmap[3], "setbit", { 90, 1 }, true)
	emit("hll", pool.hll[1], "pfadd", { "k1", "k2", "k3" }, true)
	emit("hll", pool.hll[2], "pfadd", { "k2", "k4" }, true)
	emit("zset", pool.zset[1], "zadd", { 1, "a", 2, "b", 3, "c" }, true)
	emit("zset", pool.zset[2], "zadd", { 1, "a", 20, "z" }, true)
	-- one key of each set pool, so both payloads exist from the first record on:
	-- the integer members make an intset, the others a listpack
	emit("set", pool.set[1], "sadd", { "1", "2", "3" }, false)
	emit("set", pool.set[4], "sadd", { "a", "member with space" }, false)
	local g = pool.geo[1]
	for _, p in ipairs(SICILY) do emit("geo", g, "geoadd", { p[1], p[2], p[3] }, true) end
end
warm()
-- the warm-up above is what first brings these keys into existence on the server
for _, k in ipairs({ pool.bitmap[1], pool.bitmap[2], pool.bitmap[3],
	pool.hll[1], pool.hll[2], pool.zset[1], pool.zset[2], pool.geo[1],
	pool.set[1], pool.set[4] }) do
	written[k] = true
end

while seq < count do
	local r = pick(100)
	if r <= 4 then
		local bad = BAD[pick(#BAD)]
		emit(bad[1], bad[5] or key_of(bad[1]), bad[2], bad[3], bad[4])
	elseif r <= 10 then
		local which = pick(3)
		if which == 1 then
			local dest = key_of("bitmap")
			written[dest] = true
			emit_multi("bitmap", dest, "bitop",
				{ one_of({ "and", "or", "xor", "not", "diff", "andor", "one" }),
				  dest, { key_of("bitmap"), key_of("bitmap") } }, true)
		elseif which == 2 then
			local dest = key_of("hll")
			written[dest] = true
			emit_multi("hll", dest, "pfmerge", { dest, { key_of("hll"), key_of("hll") } }, true)
		else
			local dest = key_of("zset")
			written[dest] = true
			local args = { dest, { key_of("zset"), key_of("zset"), key_of("zset") } }
			if pick(2) == 1 then
				args[#args+1] = "weights"
				args[#args+1] = { tostring(rnd() % 4 + 1), tostring(rnd() % 4 + 1),
				                  tostring(rnd() % 4 + 1) }
			end
			if pick(2) == 1 then
				args[#args+1] = "aggregate"
				args[#args+1] = one_of({ "sum", "min", "max" })
			end
			emit_multi("zset", dest, one_of({ "zunionstore", "zinterstore", "zdiffstore" }),
				args, true)
		end
	elseif r <= 16 then
		-- the commands whose receiver is not the object they read or write: the
		-- set operations in their read form, ZMPOP, ZRANGESTORE, GEOSEARCHSTORE
		local which = pick(9)
		local zs = { key_of("zset"), key_of("zset") }
		local numarg = { zs[1], zs[2] }
		if which <= 3 then
			local a = { numarg }
			if pick(2) == 1 then
				a[#a+1] = "withscores"
			else
				a[#a+1] = "limit"
				a[#a+1] = "2"
			end
			emit("zset", zs[1], one_of({ "zunion", "zinter", "zdiff" }), a, false)
		elseif which == 4 then
			emit("zset", zs[1], "zintercard", { numarg, "limit", "3" }, false)
		elseif which == 5 then
			emit("zset", zs[1], "zmpop", { zs, one_of({ "min", "max" }), "count", "2" }, true)
		elseif which == 6 then
			emit("zset", key_of("zset"), "zrangestore",
				{ key_of("zset"), "0", "-1" }, true)
		elseif which == 7 then
			emit("zset", key_of("zset"), "zrangestore",
				{ key_of("zset"), "1", "100", "byscore", "rev", "limit", "0", "2" }, true)
		elseif which == 8 then
			emit("geo", key_of("geo"), "geosearchstore",
				{ key_of("zset"), "fromlonlat", "15", "37", "byradius", "200", "km",
				  "count", "2" }, false)
		else
			emit("hll", key_of("hll"), "pfselftest", {}, false)
		end
	elseif r <= 26 then
		-- the set forms: the operations whose receiver is one of the operands rather
		-- than the object being written, and the scripted exchanges that put a key in
		-- a state the log knows. SINTER takes either pool because it answers by
		-- walking its smallest operand and builds no new payload; SUNION and SDIFF
		-- only the integer one, because the payload they build is a hashtable as soon
		-- as an operand is not an intset, and a hashtable has no reproducible order
		-- for either side (see the note above the set pools).
		local which = pick(12)
		if which <= 3 then
			emit_setop("sinter", pick(2) == 1, pick(3))
		elseif which == 4 then
			emit_setop("sunion", true, pick(3))
		elseif which == 5 then
			emit_setop("sdiff", true, pick(3))
		elseif which == 6 then
			emit_setop("sinter", true, 2)
		elseif which <= 9 then
			set_scripted(true)
		else
			set_scripted(false)
		end
	else
		local class = one_of(CLASSES)
		local key = key_of(class)
		local cmd, args, probe = GEN[class](key)
		if WRITES[class][cmd] then written[key] = true end
		emit(class, key, cmd, args, probe)
	end
end

fh:close()
io.write("gen.lua: wrote ", seq, " ops to ", out, " (seed ", seed, ")\n")
