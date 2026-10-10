local skynet = require "skynet"

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

	local function errtext(f, ...)
		local ok, message = pcall(f, ...)
		assert(not ok, "expected an error")
		-- LuaJIT prepends the raising position to errors that come out of C
		-- code ("file:line: <message>"); the Redis text is what matters here.
		return ((tostring(message)):gsub("^.-:%d+: ", "", 1))
	end

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

local tests = {
	{ "bitmap", test_bitmap },
	{ "hll", test_hll },
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
