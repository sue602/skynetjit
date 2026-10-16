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
local set = require "set"

-- A set answers a member list Redis promises no order for, and the two sides cannot
-- be asked for the same order: the payload here is walked in one way, the server's
-- deferred reply in another, and once a hashtable is involved the bucket order comes
-- from a hash seed that is random per process. So every member list this step hands
-- over is sorted, and interop.py sorts the server's reply the same way -- a set
-- compared as the set it is, with the conversion said out loud instead of hidden.
local function sorted(t)
	local out = {}
	for i = 1, #t do out[i] = t[i] end
	table.sort(out)
	return out
end

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

	-- 4. two sets built here, for the server to import and re-run the set operations
	--    on. This is where SUNIONCARD / SINTERCARD / SDIFFCARD are compared: their
	--    wire form is `SINTERCARD numkeys key [key ...] [LIMIT limit]`, and an oplog
	--    record has nowhere to put a count in front of the names -- replay_redis.py
	--    always sends `CMD key args...` -- so the differential run carries only their
	--    shared failure text (see gen.lua's note above the set pools) and the answers
	--    themselves are checked here, on identical content.
	local sa = set.new()
	sa:sadd("1", "2", "3", "4", "5", "x\ty")
	local sb = set.new()
	sb:sadd("4", "5", "6", "7", "not-an-integer")

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
		-- a set travels the same way -- uint32 count, then uint32 length and that
		-- many member bytes, the members in ascending byte order -- and the python
		-- step replays it with SADD, which is spec 4's promise for this family too
		set_a_dump = hex(sa:dump()),
		set_b_dump = hex(sb:dump()),
		set_a_scard = sa:scard(),
		set_b_scard = sb:scard(),
		set_a_members = sorted(sa:smembers()),
		set_union = sorted(set.sunion({sa, sb})),
		set_inter = sorted(set.sinter({sa, sb})),
		set_diff = sorted(set.sdiff({sa, sb})),
		set_rdiff = sorted(set.sdiff({sb, sa})),
		set_unioncard = set.sunioncard({sa, sb}),
		set_intercard = set.sintercard({sa, sb}),
		set_diffcard = set.sdiffcard({sa, sb}),
		set_intercard_l1 = set.sintercard({sa, sb}, "limit", 1),
		set_diffcard_l2 = set.sdiffcard({sb, sa}, "LIMIT", 2),
	}))
	o:close()

	local rep = io.open(report_path, "wb")
	rep:write(cjson.encode(r) .. "\n")
	rep:close()
	io.stdout:write("interop_lib.lua: loaded the server's values and wrote its own\n")
	os.exit(0)
end)
