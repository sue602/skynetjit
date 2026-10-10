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

local CLASSES = { "bitmap", "hll", "zset", "geo" }

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
}
local pool = {}
for _, c in ipairs(CLASSES) do
	pool[c] = {}
	for i = 1, 6 do pool[c][i] = c .. "_" .. i end
end
local key_of = function(c) return one_of(pool[c]) end

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
}

local fh = assert(io.open(out, "wb"))
local seq = 0

local function emit(class, key, cmd, args, probe)
	seq = seq + 1
	fh:write(string.format(
		'{"seq":%d,"class":"%s","key":"%s","cmd":"%s","args":%s,"probe":%s}\n',
		seq, class, key, cmd, jargs(args), probe and "true" or "false"))
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
	local g = pool.geo[1]
	for _, p in ipairs(SICILY) do emit("geo", g, "geoadd", { p[1], p[2], p[3] }, true) end
end
warm()
-- the warm-up above is what first brings these keys into existence on the server
for _, k in ipairs({ pool.bitmap[1], pool.bitmap[2], pool.bitmap[3],
	pool.hll[1], pool.hll[2], pool.zset[1], pool.zset[2], pool.geo[1] }) do
	written[k] = true
end

while seq < count do
	local r = pick(100)
	if r <= 4 then
		local bad = BAD[pick(#BAD)]
		emit(bad[1], key_of(bad[1]), bad[2], bad[3], bad[4])
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
