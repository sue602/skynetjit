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

local tests = {
	{ "bitmap", test_bitmap },
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
