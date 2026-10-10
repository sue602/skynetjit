-- interop_lib.lua -- redis-diff: the middle step of the spec 4 round trip. It
-- loads the bytes a real redis-server produced, reports what they count as, and
-- writes the bytes this library produced for the server to import.
--
--   cd build/linux-out && ./skynet ../../tools/redis-diff/interop-config.lua
--
-- Paths come from the config (redisdiff_in / redisdiff_out / redisdiff_report).
local skynet = require "skynet"
local cjson = require "cjson"

local bitmap = require "bitmap"
local hll = require "hll"
local sortedset = require "sortedset"
local geo = require "geo"

local function unhex(s)
	s = s:gsub("%s", "")
	local out = {}
	for i = 1, #s, 2 do
		out[#out+1] = string.char(tonumber(s:sub(i, i+1), 16))
	end
	return table.concat(out)
end

local function read_hex(path)
	local f = assert(io.open(path, "rb"))
	local s = f:read("a")
	f:close()
	return s
end

skynet.start(function()
	local in_path = assert(skynet.getenv("redisdiff_in"))
	local out_path = assert(skynet.getenv("redisdiff_out"))
	local report_path = assert(skynet.getenv("redisdiff_report"))

	local ino = cjson.decode(read_hex(in_path))
	local r = {}

	-- The exchange files carry hex, so a binary value survives a text file and a JSON
	-- field: spec 4's claim is about the bytes themselves.
	local function hex(s)
		local out = {}
		for i = 1, #s do out[i] = string.format("%02x", s:byte(i)) end
		return table.concat(out)
	end

	-- 1. a sketch made by the server, loaded here
	local h = hll.load(unhex(ino.hll_hex))
	-- load() alone does not touch a byte, which is what a caller migrating values
	-- needs -- and it is checked before anything else runs, because PFCOUNT updates
	-- the cached cardinality in the value (its own, and the server's).
	r.hll_reparse_same = (hex(h:dump()) == ino.hll_hex)
	r.hll_loaded_count = h:pfcount()
	r.hll_counted_hex = hex(h:dump())
	r.hll_loaded_readds = h:pfadd("interop-x", "interop-y")
	r.hll_after_readds = h:pfcount()

	-- 2. a string value made by the server, loaded as a bitmap
	local b = bitmap.load(unhex(ino.bitmap_hex))
	r.bitmap_len = b:len()
	r.bitmap_bitcount = b:bitcount()
	r.bitmap_bitpos = b:bitpos(1)
	r.bitmap_bitpos0 = b:bitpos(0)
	r.bitmap_reparse_same = (hex(b:dump()) == ino.bitmap_hex)

	-- 3. the other direction: values produced here, for the server to import
	local mine = hll.new()
	for i = 1, 400 do mine:pfadd("e" .. i) end
	-- count first: the sketch that travels is the one with a warm cardinality
	-- cache, which is what the server will hold after its own PFCOUNT
	local mine_count = mine:pfcount()
	local mb = bitmap.new()
	mb:setbit(500, 1)
	mb:setbit(7, 1)
	local mz = sortedset.new()
	mz:zadd(1.5, "a", -2, "b", 7, "c")
	local mg = geo.new()
	mg:geoadd(13.361389, 38.115556, "Palermo")
	mg:geoadd(15.0872689, 37.502669, "Catania")

	local o = io.open(out_path, "wb")
	o:write(cjson.encode({
		hll_hex = hex(mine:dump()),
		hll_count = mine_count,
		bitmap_hex = hex(mb:dump()),
		bitmap_bitcount = mb:bitcount(),
		-- a sorted set and a geoset travel as spec 4's records; the python step
		-- parses them with plain byte arithmetic and replays ZADD / GEOADD
		zset_dump = hex(mz:dump()),
		geo_dump = hex(mg:dump()),
		geo_dist = mg:geodist("Palermo", "Catania", "km"),
	}))
	o:close()

	local rep = io.open(report_path, "wb")
	rep:write(cjson.encode(r) .. "\n")
	rep:close()
	io.stdout:write("interop_lib.lua: loaded the server's values and wrote its own\n")
	os.exit(0)
end)
