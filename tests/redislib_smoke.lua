local skynet = require "skynet"

-- Capture a Redis-shaped error raised by a facade call and strip the position
-- LuaJIT prepends to errors coming out of C code ("file:line: <message>"), so
-- assertions compare against the upstream error text itself.
local function errtext(f, ...)
	local ok, message = pcall(f, ...)
	assert(not ok, "expected an error")
	return ((tostring(message)):gsub("^.-:%d+: ", "", 1))
end

local function test_bitmap()
	local bitmap = require "bitmap"

	-- SETBIT / GETBIT (bit 7 = LSB of byte 0, bit 0 = MSB)
	local b = bitmap.new()
	assert(b:setbit(7, 1) == 0)
	assert(b:getbit(7) == 1)
	assert(b:getbit(0) == 0)
	assert(b:getbit(100) == 0)
	assert(b:bitcount() == 1)
	assert(b:setbit(7, 1) == 1) -- returns the ORIGINAL bit
	assert(b:setbit(7, 0) == 1)
	assert(b:getbit(7) == 0)

	-- second bitmap spans TWO bytes: bit 8 -> byte 1 = 0x80, byte 0 = 0x00
	local c = bitmap.new()
	c:setbit(8, 1)

	-- BITOP over b=[0x80] and c=[0x00,0x80]
	assert(b:setbit(7, 1) == 0) -- restore bit cleared by the lifecycle check above
	local d = bitmap.bitop("and", { b, c })
	assert(d:bitcount() == 0 and d:len() == 2) -- Redis returns maxlen bytes
	local e = bitmap.bitop("or", { b, c })
	assert(e:bitcount() == 2)
	local xr = bitmap.bitop("xor", { b, c })
	assert(xr:bitcount() == 2)
	local notr = bitmap.bitop("not", { b })
	assert(notr:bitcount() == 7) -- byte 0xFE (b=[0x01]: setbit(7) = LSB, Redis bit0 = MSB)
	assert(notr:getbit(0) == 1 and notr:getbit(7) == 0)
	local diff = bitmap.bitop("diff", { e, c })  -- e & ~c
	assert(diff:bitcount() == 1 and diff:getbit(7) == 1)
	local diff1 = bitmap.bitop("diff1", { c, e }) -- ~c & e = byte0 bit7 only
	assert(diff1:bitcount() == 1 and diff1:getbit(7) == 1)
	local andor = bitmap.bitop("andor", { e, c }) -- e & (c...)
	assert(andor:bitcount() == 1 and andor:getbit(8) == 1)
	local one = bitmap.bitop("one", { e, c })     -- bits set exactly once
	assert(one:bitcount() == 1 and one:getbit(7) == 1)
	assert(pcall(function() bitmap.bitop("not", { b, c }) end) == false)
	assert(pcall(function() bitmap.bitop("andnot", { b, c }) end) == false)

	-- BITPOS
	local bp = bitmap.new()
	bp:setbit(3, 1)
	assert(bp:bitpos(1) == 3)
	assert(bp:bitpos(0) == 0)
	assert(bp:bitpos(1, 4) == false)     -- start byte beyond range -> empty -> false
	assert(bp:bitpos(1, 0, 0) == 3)      -- byte 0 only
	local full = bitmap.new()
	for i = 0, 7 do full:setbit(i, 1) end
	assert(full:bitpos(0) == 8)          -- implicit zeros right after the string
	assert(full:bitpos(0, 0, 7) == false) -- explicit end inside range: no clear bit

	-- error texts
	assert(pcall(function() b:setbit(0, 2) end) == false)
	assert(pcall(function() b:setbit(-1, 1) end) == false)
	local e1 = select(2, pcall(function() b:bitcount(0, 0, "nonsense") end))
	assert(type(e1) == "string" and e1:lower():find("syntax"))

	-- BITFIELD basics: u8@0 = 200
	local g = bitmap.new()
	local r0 = g:bitfield("SET", "u8", 0, 200) -- replies are per-op arrays
	assert(r0[1] == 0)                         -- SET returns OLD value
	local gg = g:bitfield("GET", "u8", 0)
	assert((type(gg) == "table" and gg[1] == 200) or gg == 200)
	-- WRAP: i8@0 currently 200 == -56; +100 => 44 no wrap
	local w = g:bitfield("INCRBY", "i8", 0, 100)
	local wv = type(w) == "table" and w[1] or w
	assert(wv == 44)
	-- SAT clamp at u4: set 9 then incr 9 => 15
	local h = bitmap.new()
	h:bitfield("SET", "u4", 0, 9)
	local s = h:bitfield("OVERFLOW", "SAT", "INCRBY", "u4", 0, 9)
	assert((type(s) == "table" and s[1] == 15) or s == 15)
	assert(h:bitfield_ro("GET", "u4", 0)[1] == 15) -- SAT clamped 9+9 -> 15
	-- RO rejects write ops and non-GET subcommands:
	assert(pcall(function() h:bitfield_ro("SET", "u4", 0, 1) end) == false)
	assert(pcall(function() h:bitfield("OVERFLOW", "NOPE", "GET", "u4", 0) end) == false)
	-- FAIL: the overflowing op is not executed and replies null(false);
	-- per Redis 8.10 implementation the REMAINING ops still run.
	local k = bitmap.new()
	k:bitfield("SET", "u4", 0, 9)
	local fr = k:bitfield("OVERFLOW", "FAIL", "INCRBY", "u4", 0, 9, "GET", "u4", 0)
	assert(fr[1] == false and fr[2] == 9)
	assert(k:bitfield("GET", "u4", 0)[1] == 9) -- unchanged (FAIL op was first)
	assert(k:bitfield("GET", "u4", 4)[1] == 0)
	-- offset limits: u64 not supported, i64 ok; offset capped at proto_max_bulk_len bytes
	assert(pcall(function() k:bitfield("SET", "u64", 0, 1) end) == false)
	assert(pcall(function() k:bitfield("SET", "i64", 0, 1) end) == true)
	assert(pcall(function() k:bitfield("GET", "u8", 4*1024*1024*1024) end) == false)
	assert(pcall(function() k:bitfield("GET", "u8", 70000000) end) == true) -- 8.75MB < 512MB cap

	-- BITCOUNT ranges with byte/bit units
	local m = bitmap.new()
	m:setbit(0, 1)  -- byte0 MSB
	m:setbit(9, 1)  -- byte1 bit1
	assert(m:bitcount() == 2)
	assert(m:bitcount(0, 0) == 1)
	assert(m:bitcount(1, -1) == 1)
	assert(m:bitcount(0, 8, "BIT") == 1)  -- bits 0..8 => only bit0
	assert(m:bitcount(9, 9, "BIT") == 1)
	assert(m:bitcount(-1, -1) == 1)       -- last byte only
	assert(m:bitcount(5, 1) == 0)         -- start > end
end

local function test_hll()
	local hll = require "hll"
	local bitmap = require "bitmap"

	-- PFADD replies :1 when the cardinality changed, :0 otherwise
	local a = hll.new()
	assert(a:pfadd("x", "y", "z") == 1)
	assert(a:pfadd("x") == 0)
	assert(a:pfcount() == 3)

	-- a fresh value is an 18 byte sparse HLL covering all 16384 registers
	local z = hll.new()
	assert(z:pfcount() == 0)
	assert(z:pfdebug("encoding") == "sparse")
	assert(z:pfdebug("decode") == "Z:16384")

	-- elements are Redis bulk strings: numbers take their decimal form and
	-- embedded NUL bytes are kept
	assert(a:pfadd(123) == 1)
	assert(a:pfadd("123") == 0)
	assert(a:pfadd("a\0b") == 1)
	assert(a:pfcount() == 5)

	-- PFCOUNT with several operands is the union, computed in a throwaway
	-- value: nothing is written back to the operands
	local b = hll.new()
	assert(b:pfadd("z", "w") == 1)
	assert(b:pfcount() == 2)
	-- "z" is shared, so a(5) + b(2) = 6 distinct elements
	assert(a:pfcount(b) == 6)
	assert(hll.pfcount({ a, b }) == 6)
	assert(hll.pfcount(a, b) == 6)
	assert(a:pfcount({ b }) == 6)
	assert(a:pfcount() == 5 and b:pfcount() == 2)
	-- merging is the same value as adding every element to one HLL
	local u = hll.new()
	assert(u:pfadd("x", "y", "z", "123", "a\0b", "w") == 1)
	assert(u:pfcount() == a:pfcount(b))

	-- PFMERGE replies OK and is callable both as a module function (destination
	-- first) and as a method (destination is self)
	local m = hll.new()
	assert(hll.pfmerge(m, { a, b }) == "OK")
	assert(m:pfcount() == 6)
	assert(m:pfmerge(a, b) == "OK")
	assert(m:pfcount() == 6)
	assert(m:pfadd("q") == 1)
	assert(m:pfcount() == 7)

	-- PFDEBUG ENCODING / TODENSE / GETREG
	local d = hll.new()
	assert(d:pfdebug("todense") == 1) -- sparse -> dense, replies :1
	assert(d:pfdebug("encoding") == "dense")
	assert(d:pfdebug("todense") == 0) -- already dense
	local regs = d:pfdebug("getreg")
	assert(type(regs) == "table" and #regs == 16384)
	assert(regs[1] == 0 and regs[16384] == 0)
	d:pfadd("promote-me")
	assert(d:pfdebug("encoding") == "dense")
	assert(errtext(function() d:pfdebug("decode") end) == "HLL encoding is not sparse")

	-- sparse stays sparse while it is smaller than hll-sparse-max-bytes
	local s = hll.new()
	local added = 0
	for i = 1, 800 do
		local r = s:pfadd("sparse"..i)
		-- PFADD replies 1 only when a register actually moved, so a distinct
		-- element that lands on an already higher register replies 0
		assert(r == 0 or r == 1, "PFADD must reply 0 or 1")
		if r == 1 then added = added + 1 end
	end
	assert(added > 700, "registers updated: "..tostring(added))
	assert(s:pfdebug("encoding") == "sparse")
	assert(s:pfdebug("decode"):match("^[zZv]:%d") ~= nil)

	-- and promotes to dense past the threshold (Redis: ~1500 unique elements)
	for i = 801, 6000 do
		local r = s:pfadd("sparse"..i)
		assert(r == 0 or r == 1, "PFADD must reply 0 or 1")
		if r == 1 then added = added + 1 end
	end
	assert(s:pfdebug("encoding") == "dense")
	-- PFCOUNT stays within the documented error bounds for 6000 elements
	local card = s:pfcount()
	assert(card >= 5400 and card <= 6600, "cardinality out of bounds: "..tostring(card))
	local regs2 = s:pfdebug("getreg")
	assert(#regs2 == 16384)
	local maxreg = 0
	for _, v in ipairs(regs2) do assert(v >= 0 and v <= 63) if v > maxreg then maxreg = v end end
	assert(maxreg >= 1)

	-- PFDEBUG SIMD is answered by the kernel's runtime switch; this port builds
	-- neither AVX2 nor NEON, so the reply is always "disabled"
	assert(a:pfdebug("simd", "on") == "disabled")
	assert(a:pfdebug("simd", "off") == "disabled")
	assert(errtext(function() a:pfdebug("simd", "bogus") end) == "Argument must be ON or OFF")
	assert(errtext(function() a:pfdebug("simd") end) ==
		"Wrong number of arguments for the 'simd' subcommand")

	-- PFSELFTEST is the upstream kernel self-test, replies OK
	assert(a:pfselftest() == "OK")

	-- error texts are the Redis ones
	assert(errtext(function() a:pfdebug("bogus") end) ==
		"Unknown PFDEBUG subcommand 'bogus'")
	assert(errtext(function() a:pfdebug("getreg", 1) end) ==
		"Wrong number of arguments for the 'getreg' subcommand")
	assert(errtext(function() hll.pfcount() end) ==
		"wrong number of arguments for 'pfcount' command")
	local bm = bitmap.new()
	assert(errtext(function() hll.pfcount(bm) end) ==
		"WRONGTYPE Operation against a key holding the wrong kind of value")
	assert(errtext(function() hll.pfmerge(bm, a) end) ==
		"WRONGTYPE Operation against a key holding the wrong kind of value")
	assert(errtext(function() a:pfadd({}) end) == "string or number expected, got table")

	-- an element error mid-loop must not strand the object on the sds the
	-- kernel already replaced: PFADD writes each result back before the next
	-- argument can raise, so the value stays readable and freeable
	local partial = hll.new()
	assert(pcall(function() partial:pfadd("kept", {}) end) == false)
	collectgarbage("collect")
	assert(partial:pfcount() == 1)
	assert(partial:pfadd("second") == 1)
	assert(partial:pfcount() == 2)
	assert(errtext(function() partial:pfdebug("todense", 1) end) ==
		"Wrong number of arguments for the 'todense' subcommand")
	assert(partial:pfdebug("encoding") == "sparse")
	assert(partial:pfcount() == 2)

	-- free()/__gc: the value is released once
	local t = hll.new()
	t:pfadd("gc-me")
	t:free()
	assert(pcall(function() t:pfcount() end) == false)
	local g1 = hll.new()
	g1 = nil
	collectgarbage("collect")
end

local function test_sortedset()
	local sortedset = require "sortedset"
	local bitmap = require "bitmap"

	-- ZADD replies the number of elements ADDED, not the number processed
	local a = sortedset.new()
	assert(a:zcard() == 0)
	assert(a:zscore("nope") == false)
	assert(a:zadd(1, "a", 2, "b", 3, "c") == 3)
	assert(a:zadd(5, "a") == 0 and a:zscore("a") == 5)
	assert(a:zcard() == 3)
	assert(a:zincrby(2, "a") == 7)
	assert(a:zincrby(-7, "a") == 0)
	assert(a:zincrby(1, "new") == 1 and a:zcard() == 4)

	-- score input: the string forms Redis accepts, and the ones it refuses
	local b = sortedset.new()
	assert(b:zadd("1.5", "x") == 1 and b:zscore("x") == 1.5)
	assert(b:zadd("inf", "hi") == 1 and b:zscore("hi") == math.huge)
	assert(b:zadd("-inf", "lo") == 1 and b:zscore("lo") == -math.huge)
	assert(errtext(function() b:zadd("1e400", "q") end) == "value is not a valid float")
	assert(errtext(function() b:zadd(0 / 0, "q") end) == "value is not a valid float")
	assert(errtext(function() b:zadd("nan", "q") end) == "value is not a valid float")
	assert(errtext(function() b:zadd("abc", "q") end) == "value is not a valid float")
	-- inf + -inf is NaN, which zsetAdd() refuses with its own text
	assert(errtext(function() b:zincrby("-inf", "hi") end) ==
		"resulting score is not a number (NaN)")

	-- ZADD options, one by one and combined
	local c = sortedset.new()
	assert(c:zadd("nx", 1, "a") == 1)
	assert(c:zadd("nx", 2, "a") == 0 and c:zscore("a") == 1)
	assert(c:zadd("xx", 3, "a") == 0 and c:zscore("a") == 3)
	assert(c:zadd("ch", "xx", 4, "a") == 1)
	assert(c:zadd("xx", 5, "absent") == 0 and c:zscore("absent") == false)
	assert(c:zadd("gt", 1, "a") == 0 and c:zscore("a") == 4)
	assert(c:zadd("gt", 6, "a") == 0 and c:zscore("a") == 6)
	assert(c:zadd("lt", 7, "a") == 0 and c:zscore("a") == 6)
	assert(c:zadd("lt", 2, "a") == 0 and c:zscore("a") == 2)
	assert(c:zadd("incr", 3, "a") == 5)          -- 2 + 3
	assert(c:zadd("incr", "nx", 3, "a") == false) -- NX on an existing element: nil reply
	assert(c:zscore("a") == 5)
	assert(c:zadd("incr", "xx", 3, "a") == 8)     -- XX on an existing element: 5 + 3
	assert(errtext(function() c:zadd("nx", "xx", 1, "a") end) ==
		"XX and NX options at the same time are not compatible")
	assert(errtext(function() c:zadd("gt", "nx", 1, "a") end) ==
		"GT, LT, and/or NX options at the same time are not compatible")
	assert(errtext(function() c:zadd("incr", 1, "a", 2, "b") end) ==
		"INCR option supports a single increment-element pair")
	assert(errtext(function() c:zadd(1) end) == "syntax error")
	assert(errtext(function() c:zadd() end) == "syntax error")

	-- ZRANK / ZREVRANK, 0-based like the Redis reply, WITHSCORE 8.0 option
	local r = sortedset.new()
	r:zadd(1, "a", 2, "b", 3, "c")
	assert(r:zrank("a") == 0 and r:zrank("c") == 2)
	assert(r:zrevrank("a") == 2 and r:zrevrank("c") == 0)
	assert(r:zrank("nope") == false and r:zrevrank("nope") == false)
	local ws = r:zrank("b", "withscore")
	assert(type(ws) == "table" and ws[1] == 1 and ws[2] == 2)
	assert(r:zrevrank("b", "WITHSCORE")[2] == 2)
	assert(errtext(function() r:zrank("b", "bogus") end) == "syntax error")
	assert(errtext(function() r:zrank("b", "withscore", "x") end) ==
		"wrong number of arguments for 'zrank' command")

	-- ZMSCORE keeps one array slot per member, false where Redis replies nil
	local ms = r:zmscore("a", "nope", "c")
	assert(#ms == 3 and ms[1] == 1 and ms[2] == false and ms[3] == 3)

	-- ZCOUNT / ZLEXCOUNT, both bound grammars
	assert(r:zcount(1, 2) == 2)
	assert(r:zcount("(1", "2") == 1)
	assert(r:zcount("(2", "(3") == 0)
	assert(r:zcount("2", "1") == 0)
	assert(r:zcount("-inf", "+inf") == 3)
	assert(errtext(function() r:zcount("a", 1) end) == "min or max is not a float")
	assert(r:zlexcount("[a", "[c") == 3)
	assert(r:zlexcount("[a", "(c") == 2)
	assert(r:zlexcount("-", "+") == 3)
	assert(errtext(function() r:zlexcount("a", "b") end) ==
		"min or max not valid string range item")

	-- ZREM
	local d = sortedset.new()
	d:zadd(1, "a", 2, "b", 3, "c")
	assert(d:zrem("b", "zzz") == 1 and d:zcard() == 2)
	assert(d:zrem("a", "c") == 2 and d:zcard() == 0)
	assert(d:zscore("a") == false)

	-- ZREMRANGEBYRANK, including the negative index rules of the rank form
	local e = sortedset.new()
	e:zadd(1, "a", 2, "b", 3, "c", 4, "d")
	assert(e:zremrangebyrank(0, 0) == 1 and e:zscore("a") == false)
	assert(e:zremrangebyrank(-1, -1) == 1 and e:zscore("d") == false)
	assert(e:zcard() == 2)
	assert(e:zremrangebyrank(0, 10) == 2 and e:zcard() == 0)
	assert(e:zremrangebyrank(0, 1) == 0)          -- empty set
	assert(e:zremrangebyrank(5, 1) == 0)         -- start > end

	-- ZREMRANGEBYSCORE ("(N" opens the bound, so the edge score survives it)
	local f = sortedset.new()
	f:zadd(1, "a", 2, "b", 3, "c", 4, "d")
	assert(f:zremrangebyscore(2, 3) == 2 and f:zcard() == 2)     -- b, c
	assert(f:zremrangebyscore("(1", "(4") == 0)                  -- 1 < x < 4: empty
	assert(f:zremrangebyscore(1, 1) == 1)                        -- min == max, inclusive
	assert(f:zremrangebyscore("(0", "(5") == 1 and f:zcard() == 0)
	local fx = sortedset.new()
	fx:zadd(1, "a", 2, "b", 3, "c")
	assert(fx:zremrangebyscore("(1", "(3") == 1 and fx:zcard() == 2)
	assert(fx:zscore("b") == false and fx:zscore("a") == 1 and fx:zscore("c") == 3)

	-- ZREMRANGEBYLEX
	local g = sortedset.new()
	g:zadd(0, "aa", 0, "ab", 0, "bb", 0, "bc")
	assert(g:zremrangebylex("[aa", "[ab") == 2 and g:zcard() == 2)
	assert(g:zremrangebylex("[", "+") == 2 and g:zcard() == 0)
	assert(g:zremrangebylex("[zz", "[yy") == 0)

	-- the skiplist encoding takes over past zset-max-listpack-entries (128),
	-- so the range walks below run the skiplist branch
	local big = sortedset.new()
	for i = 1, 500 do big:zadd(i, "m" .. i) end
	assert(big:zcard() == 500)
	assert(big:zcount(100, 200) == 101)
	assert(big:zrank("m200") == 199)
	assert(big:zrevrank("m200") == 300)
	assert(big:zremrangebyrank(0, 99) == 100 and big:zcard() == 400)   -- m1..m100
	assert(big:zremrangebyscore(1, 200) == 100 and big:zcard() == 300) -- m101..m200
	assert(big:zremrangebylex("[m2", "[m3") == 99 and big:zcard() == 201) -- m201..m299
	assert(big:zscore("m201") == false and big:zscore("m299") == false)
	assert(big:zscore("m300") == 300 and big:zscore("m400") == 400)

	-- the range family: ZRANGE / ZREVRANGE reply flat arrays, WITHSCORES
	-- interleaves member and score like the RESP2 reply (spec §3.2)
	local q = sortedset.new()
	q:zadd(1, "a", 2, "b", 3, "c", 4, "d")
	local r1 = q:zrange(0, -1)
	assert(#r1 == 4 and r1[1] == "a" and r1[4] == "d")
	assert(#q:zrange(1, 2) == 2 and q:zrange(1, 2)[1] == "b")
	assert(q:zrange(-2, -1)[1] == "c")
	assert(#q:zrange(5, 10) == 0)                     -- start past the end: empty array
	local w = q:zrange(0, -1, "withscores")
	assert(#w == 8 and w[1] == "a" and w[2] == 1 and w[7] == "d" and w[8] == 4)
	local rv = q:zrevrange(0, -1, "withscores")
	assert(rv[1] == "d" and rv[2] == 4 and rv[7] == "a" and rv[8] == 1)
	assert(q:zrevrange(0, 0)[1] == "d")

	-- ZRANGEBYSCORE / ZREVRANGEBYSCORE (the reverse form takes max first)
	assert(#q:zrangebyscore(2, 3) == 2)
	local bs = q:zrangebyscore("(1", "4", "withscores")
	assert(#bs == 6 and bs[1] == "b" and bs[6] == 4)
	assert(q:zrangebyscore(2, "inf")[1] == "b")
	assert(#q:zrangebyscore(2, 3, "limit", 0, 1) == 1)
	assert(q:zrangebyscore(2, 3, "limit", 1, 1)[1] == "c")
	assert(#q:zrangebyscore(1, 4, "limit", -1, 2) == 0)  -- a negative offset replies empty
	local rev = q:zrevrangebyscore(4, 2)
	assert(rev[1] == "d" and rev[2] == "c" and rev[3] == "b")
	assert(#q:zrevrangebyscore("(4", "(2") == 1)   -- 2 < x < 4: only c, walked backwards
	assert(q:zrevrangebyscore("(4", "(2")[1] == "c")

	-- the unified 8.10 form: ZRANGE <min> <max> [BYSCORE|BYLEX] [REV] [...]
	local f1 = q:zrange("(1", "4", "byscore")
	assert(#f1 == 3 and f1[1] == "b")
	local f2 = q:zrange("[b", "[c", "bylex")
	assert(#f2 == 2 and f2[1] == "b" and f2[2] == "c")
	local f3 = q:zrange("4", "(1", "byscore", "rev", "withscores")
	assert(f3[1] == "d" and f3[2] == 4 and f3[3] == "c")
	assert(errtext(function() q:zrange(0, 1, "limit", 0, 1) end) ==
		"syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX")
	assert(errtext(function() q:zrange(0, -1, "bylex", "withscores") end) ==
		"syntax error, WITHSCORES not supported in combination with BYLEX")
	assert(errtext(function() q:zrange(0, -1, "bogus") end) == "syntax error")

	-- ZRANGEBYLEX / ZREVRANGEBYLEX never carry scores
	assert(#q:zrangebylex("[b", "[c") == 2)
	local lx = q:zrevrangebylex("[c", "[b")
	assert(lx[1] == "c" and lx[2] == "b")
	assert(q:zrangebylex("[b", "[c", "limit", 1, 1)[1] == "c")
	assert(errtext(function() q:zrangebylex("b", "c") end) ==
		"min or max not valid string range item")

	-- ZRANGESTORE replaces the destination and replies how many it stored
	local st = sortedset.new()
	assert(sortedset.zrangestore(st, q, 0, -1) == 4)
	assert(st:zcard() == 4 and st:zscore("d") == 4 and st:zrange(0, 0)[1] == "a")
	assert(sortedset.zrangestore(st, q, "(3", "inf", "byscore") == 1 and st:zcard() == 1)
	assert(st:zscore("d") == 4 and st:zscore("a") == false)
	-- an empty result leaves the destination empty (Redis deletes the key)
	assert(sortedset.zrangestore(st, q, 10, 20) == 0 and st:zcard() == 0)
	local st2 = sortedset.new()
	assert(st2:zrangestore(q, "(1", "4", "byscore") == 3 and st2:zcard() == 3)
	-- storing a set into itself keeps the same content
	assert(q:zrangestore(q, 0, -1) == 4 and q:zcard() == 4)
	assert(q:zrange(0, -1)[4] == "d")
	-- withscores is not a ZRANGESTORE option
	assert(errtext(function() q:zrangestore(q, 0, -1, "withscores") end) == "syntax error")

	-- the same walks over the skiplist encoding
	local bqr = sortedset.new()
	for i = 1, 300 do bqr:zadd(i * 0.5, "e" .. i) end
	assert(#bqr:zrange(0, 9) == 10 and bqr:zrange(0, 0)[1] == "e1")
	assert(#bqr:zrange(0, -1, "withscores") == 600)
	assert(bqr:zrevrange(0, 0)[1] == "e300")
	assert(#bqr:zrangebyscore(1, 2) == 3)            -- e2=1.0, e3=1.5, e4=2.0
	assert(bqr:zrangebyscore(149, 150, "limit", 1, 1)[1] == "e299")

	-- ZRANGEBYLEX is only meaningful when all the scores are equal (Redis
	-- documents it: the set is ordered by score first), which is also how the
	-- skiplist walk below finds its bounds
	local blx = sortedset.new()
	for i = 1, 300 do blx:zadd(0, "e" .. i) end
	assert(#blx:zrangebylex("[e1", "[e2") == 112)  -- e1, e10-e19, e100-e199 and e2
	assert(blx:zrangebylex("[e1", "[e2")[1] == "e1")
	assert(blx:zrevrangebylex("[e2", "[e1")[1] == "e2")
	assert(#blx:zrevrangebylex("[e2", "[e1") == 112)
	assert(#blx:zrangebylex("[e1", "[e2", "limit", 100, 200) == 12)

	-- ZPOPMIN / ZPOPMAX reply the RESP2 flat member/score array
	local p = sortedset.new()
	assert(#p:zpopmin() == 0)                       -- nothing in yet
	p:zadd(1, "a", 2, "b", 3, "c", 4, "d")
	local pm = p:zpopmin()
	assert(#pm == 2 and pm[1] == "a" and pm[2] == 1)
	local px = p:zpopmax(2)
	assert(#px == 4 and px[1] == "d" and px[2] == 4 and px[3] == "c" and px[4] == 3)
	assert(p:zcard() == 1)
	assert(#p:zpopmax(10) == 2)                     -- clamped to what is left (b)
	assert(p:zcard() == 0)
	assert(#p:zpopmin() == 0)                       -- empty again: an empty result
	assert(#p:zpopmax(0) == 0)                      -- ...must not free uninitialised memory
	assert(errtext(function() p:zpopmin(-1) end) == "value is out of range, must be positive")
	assert(errtext(function() p:zpopmin(1, 2) end) == "syntax error")

	-- ZMPOP picks the first operand that still holds elements; a handle stands
	-- for the key name Redis replies
	local e1 = sortedset.new()
	local e2 = sortedset.new()
	e2:zadd(5, "five", 10, "ten")
	local mp = sortedset.zmpop({e1, e2}, "min")
	assert(mp[1] == e2 and #mp[2] == 1)
	assert(mp[2][1][1] == "five" and mp[2][1][2] == 5)
	assert(e2:zcard() == 1)
	assert(sortedset.zmpop({e1}, "max") == false)   -- nothing to pop anywhere: nil
	local mpc = sortedset.zmpop({e2}, "max", "count", 5)
	assert(#mpc[2] == 1 and mpc[2][1][1] == "ten" and e2:zcard() == 0)
	assert(errtext(function() sortedset.zmpop({e2}, "bogus") end) == "syntax error")
	assert(errtext(function() sortedset.zmpop({e2}, "min", "count", 0) end) ==
		"count should be greater than 0")
	assert(errtext(function() sortedset.zmpop({}, "min") end) ==
		"numkeys should be greater than 0")
	assert(errtext(function() sortedset.zmpop({e2}, "min", "count") end) == "syntax error")

	-- ZRANDMEMBER: assert the shapes and the reachability, not the draw
	local rr = sortedset.new()
	assert(rr:zrandmember() == false)
	rr:zadd(1, "a", 2, "b", 3, "c")
	assert(type(rr:zrandmember()) == "string")
	local seen = {}
	for i = 1, 80 do seen[rr:zrandmember()] = true end
	assert(seen.a and seen.b and seen.c)
	local uniq = rr:zrandmember(2)
	assert(#uniq == 2 and uniq[1] ~= uniq[2])       -- positive count samples uniquely
	assert(#rr:zrandmember(0) == 0)
	assert(#rr:zrandmember(10) == 3)                -- count >= size replies the set
	local scored = rr:zrandmember(2, "withscores")
	assert(#scored == 4)
	for i = 1, #scored, 2 do
		local m, sc = scored[i], scored[i+1]
		assert((m == "a" and sc == 1) or (m == "b" and sc == 2) or (m == "c" and sc == 3),
			"unexpected pair "..tostring(m))
	end
	assert(#rr:zrandmember(-50) == 50)              -- negative count repeats
	assert(errtext(function() rr:zrandmember(1, "bogus") end) == "syntax error")
	assert(errtext(function() rr:zrandmember(1, "withscores", "x") end) == "syntax error")

	-- the same on the skiplist encoding, which covers CASE 3 (count*3 > size)
	-- and CASE 4 (the set is much bigger than the count)
	local bg = sortedset.new()
	for i = 1, 300 do bg:zadd(i, "s" .. i) end
	assert(#bg:zrandmember(10) == 10)
	for i = 1, 5 do
		local r = bg:zrandmember(290)
		assert(#r == 290)                            -- CASE 3 clamps to count
		local once = {}
		for k = 1, #r do
			assert(not once[r[k]], "CASE 3 repeated a member")
			once[r[k]] = true
		end
	end
	assert(#bg:zrandmember(1, "withscores") == 2)
	local many = bg:zrandmember(20)                  -- CASE 4: unique, big set
	assert(#many == 20)
	local dedup = {}
	for k = 1, #many do dedup[many[k]] = true end
	assert(#many == 20 and next(dedup))
	assert(errtext(function() bg:zrandmember(1.5) end) ==
		"value is not an integer or out of range")

	-- a member that looks like a number keeps the decimal string Redis replies
	local num = sortedset.new()
	assert(num:zadd(1, 42) == 1 and num:zscore("42") == 1)
	assert(num:zrange(0, -1)[1] == "42")
	assert(type(num:zrandmember()) == "string")

	-- ZUNION / ZINTER / ZDIFF: scores aggregate (SUM by default) and the reply
	-- is the result in ascending order, flat with the score interleaved
	local u1 = sortedset.new(); u1:zadd(1, "a", 2, "b")
	local u2 = sortedset.new(); u2:zadd(10, "a", 20, "c")
	local un = sortedset.zunion({u1, u2})
	assert(#un == 3 and un[1] == "b" and un[2] == "a" and un[3] == "c")
	local uns = sortedset.zunion({u1, u2}, "withscores")
	assert(#uns == 6 and uns[1] == "b" and uns[2] == 2 and uns[3] == "a" and uns[4] == 11
		and uns[5] == "c" and uns[6] == 20)
	assert(sortedset.zunion({u1, u2}, "aggregate", "min")[1] == "a")   -- a=1,b=2,c=20
	assert(sortedset.zunion({u1, u2}, "aggregate", "max")[1] == "b")   -- a=10,b=2,c=20
	local cnt = sortedset.zunion({u1, u2}, "aggregate", "count", "withscores")
	assert(cnt[1] == "b" and cnt[2] == 1 and cnt[5] == "a" and cnt[6] == 2)
	local wt = sortedset.zunion({u1, u2}, "weights", {2, 3}, "withscores")
	assert(wt[1] == "b" and wt[2] == 4 and wt[3] == "a" and wt[4] == 32 and wt[5] == "c" and wt[6] == 60)
	local inter = sortedset.zinter({u1, u2}, "withscores")
	assert(#inter == 2 and inter[1] == "a" and inter[2] == 11)
	assert(#sortedset.zinter({u1, sortedset.new()}) == 0)
	assert(#sortedset.zdiff({u1, u2}) == 1 and sortedset.zdiff({u1, u2})[1] == "b")
	assert(sortedset.zdiff({u2, u1})[1] == "c")
	assert(#sortedset.zdiff({u1, u1}) == 0)
	assert(sortedset.zintercard({u1, u2}) == 1)
	assert(sortedset.zintercard({u1, u2}, "limit", 1) == 1)
	assert(sortedset.zintercard({u1, u2}, "limit", 0) == 1)   -- 0 means unlimited
	assert(sortedset.zintercard({u1, sortedset.new()}) == 0)
	-- +inf and -inf of the same member sum to NaN, which Redis reports as 0
	local inf1 = sortedset.new(); inf1:zadd("inf", "i")
	local inf2 = sortedset.new(); inf2:zadd("-inf", "i")
	local infu = sortedset.zunion({inf1, inf2}, "withscores")
	assert(infu[1] == "i" and infu[2] == 0)

	-- the STORE family: the destination ends up holding the result, and an empty
	-- result leaves it empty (that is Redis deleting the destination key)
	local s1 = sortedset.new()
	assert(sortedset.zunionstore(s1, {u1, u2}) == 3)
	assert(s1:zcard() == 3 and s1:zscore("a") == 11 and s1:zrange(0, -1)[1] == "b")
	-- a repeated operand is a real second input: ZUNION sums it with itself
	assert(s1:zunionstore({u1, u1}) == 2 and s1:zscore("a") == 2)
	assert(s1:zinterstore({u1, u2}, "aggregate", "max") == 1 and s1:zcard() == 1)
	assert(s1:zscore("a") == 10)
	assert(s1:zdiffstore({u1, u2}) == 1 and s1:zscore("b") == 2)
	assert(s1:zdiffstore({u1, u1}) == 0 and s1:zcard() == 0)
	-- storing from a set that is also the destination
	local selfop = sortedset.new(); selfop:zadd(1, "x", 2, "y")
	local other = sortedset.new(); other:zadd(5, "y", 7, "z")
	assert(selfop:zunionstore({selfop, other}) == 3)
	assert(selfop:zscore("x") == 1 and selfop:zscore("y") == 7 and selfop:zscore("z") == 7)

	-- error texts are the Redis ones
	assert(errtext(function() sortedset.zunion({}) end) ==
		"at least 1 input key is needed for 'zunion' command")
	assert(errtext(function() sortedset.zdiff({u1, u2}, "aggregate", "max") end) ==
		"syntax error")
	assert(errtext(function() sortedset.zunion({u1, u2}, "weights", {1}) end) ==
		"syntax error")
	assert(errtext(function() sortedset.zunion({u1, u2}, "weights", {"x", 1}) end) ==
		"weight value is not a float")
	assert(errtext(function() sortedset.zunion({u1, bitmap.new()}) end) ==
		"WRONGTYPE Operation against a key holding the wrong kind of value")
	assert(errtext(function() s1:zunionstore({u1, u2}, "withscores") end) ==
		"syntax error")
	assert(errtext(function() sortedset.zintercard({u1, u2}, "withscores") end) ==
		"syntax error")
	assert(errtext(function() sortedset.zintercard({u1, u2}, "limit", -1) end) ==
		"LIMIT can't be negative")
	assert(errtext(function() sortedset.zintercard({u1, u2}, "aggregate", "sum") end) ==
		"syntax error")

	-- free() releases the object; a second use is an error, and __gc on a freed
	-- object must not double free
	local t = sortedset.new()
	t:zadd(1, "x")
	t:free()
	assert(pcall(function() t:zcard() end) == false)
	local t2 = sortedset.new()
	t2:zadd(1, "gc")
	t2 = nil
	collectgarbage("collect")
end

local tests = {
	{ "bitmap", test_bitmap },
	{ "hll", test_hll },
	{ "sortedset", test_sortedset },
}

local function run()
	for _, item in ipairs(tests) do
		local ok, message = pcall(item[2])
		if not ok then
			error(item[1] .. ": " .. tostring(message), 0)
		end
	end
	local success_file = assert(io.open("redislib-smoke.ok", "wb"))
	success_file:write("ok\n")
	success_file:close()
	io.stdout:write("redislib-smoke: redislib modules succeeded\n")
	io.stdout:flush()
	return true
end

skynet.start(function()
	local ok, message = xpcall(run, debug.traceback)
	if not ok then
		io.stderr:write("redislib-smoke: failed\n", tostring(message), "\n")
		io.stderr:flush()
		os.exit(1)
	end
	os.exit(0)
end)
