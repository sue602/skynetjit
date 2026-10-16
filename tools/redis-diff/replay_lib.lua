-- replay_lib.lua -- redis-diff step 4: replay the oplog through the redislib
-- modules and record one JSON line per op, in the shape replay_redis.py records.
--
-- Run it as a skynet service from the built tree:
--
--     cd build/linux-out && ./skynet ../../tools/redis-diff/lib-config.lua
--
-- The oplog and output paths come from the config (redisdiff_oplog /
-- redisdiff_out), so a runner controls them without touching this file.
--
-- A record maps to the library's own API: `key` names the handle, created on first
-- use the way a key comes into existence on a server, and the multi-key commands go
-- through the module functions, which take a list of handles where Redis takes
-- numkeys plus names.
--
-- Three conversions are applied while recording, and diff.py states the same rules
-- rather than hiding them: a `false` reply (Redis' nil, spec 3.2) is written as
-- JSON null; BITOP's handle reply is written as the length of the string it
-- produced, which is what Redis answers; and probe values are hex of the raw
-- bytes, so a bitmap or HLL value is compared with exactly what `GET` of that key
-- returns on the server.
local skynet = require "skynet"
local cjson = require "cjson"

local bitmap = require "bitmap"
local hll = require "hll"
local sortedset = require "sortedset"
local geo = require "geo"
local set = require "set"

-- An empty Lua table encodes as `{}` here (this cjson build has no
-- encode_empty_table_as_object), while the server's empty ZRANGE is `[]`. diff.py
-- treats `{}` and `[]` as the same empty sequence, which is the only reason the
-- two encodings ever have to meet.
local NEW = { bitmap = bitmap.new, hll = hll.new, zset = sortedset.new, geo = geo.new,
              set = set.new }

local handles = {}
local names = {}          -- handle -> logical name, for replies that carry one

local function handle_for(class, key)
	local h = handles[key]
	if h == nil then
		h = NEW[class]()
		handles[key] = h
		names[h] = key
	end
	return h
end

local function hex(s)
	if s == nil then return nil end
	local out = {}
	for i = 1, #s do out[i] = string.format("%02x", s:byte(i)) end
	return table.concat(out)
end

-- An empty set is not a key on the server, and an HLL nobody wrote to is not a key
-- either, while a fresh handle holds Redis' just-created HLL header. Both sides
-- then read as "nothing here", so record the untouched object as null: the same
-- conversion the server's missing key already gets.
local FRESH_HLL = hll.new():dump()

local function normalize(v)
	if v == false then return cjson.null end
	if type(v) ~= "table" then return v end
	local out = {}
	for i = 1, #v do out[i] = normalize(v[i]) end
	return out
end

local function args_of(args, from)
	local out = {}
	for i = from or 1, #args do out[#out+1] = args[i] end
	return out
end

local function handle_list(class, names)
	local out = {}
	for i, k in ipairs(names) do out[i] = handle_for(class, k) end
	return out
end

local CALL = {}

function CALL.bitmap(op)
	local args, cmd = op.args, op.cmd
	if cmd == "bitop" then
		local opname, dest, srcs = args[1], args[2], handle_list("bitmap", args[3])
		local h = bitmap.bitop(opname, srcs)
		handles[dest] = h
		-- Redis answers the length of the destination string
		return #h:dump()
	end
	local h = handle_for("bitmap", op.key)
	if cmd == "setbit" then return h:setbit(args[1], args[2]) end
	if cmd == "bitcount" or cmd == "bitpos" or cmd == "bitfield"
		or cmd == "bitfield_ro" then
		return h[cmd](h, unpack(args_of(args)))
	end
	return h[cmd](h, args[1])          -- getbit
end

function CALL.hll(op)
	local args, cmd = op.args, op.cmd
	if cmd == "pfmerge" then
		return hll.pfmerge(handle_for("hll", args[1]), handle_list("hll", args[2]))
	end
	if cmd == "pfselftest" then return hll.pfselftest() end
	local h = handle_for("hll", op.key)
	if cmd == "pfadd" then return h:pfadd(unpack(args_of(args))) end
	if cmd == "pfdebug" then return h:pfdebug(args[1]) end
	return h:pfcount()                  -- pfcount, no arguments in the log
end

function CALL.zset(op)
	local args, cmd = op.args, op.cmd
	if cmd == "zunion" or cmd == "zinter" or cmd == "zdiff" or cmd == "zintercard" then
		-- read forms: the source list is the first argument, no destination
		return sortedset[cmd](handle_list("zset", args[1]), unpack(args_of(args), 2, #args))
	end
	if cmd == "zmpop" then
		local reply = sortedset.zmpop(handle_list("zset", args[1]), unpack(args_of(args), 2, #args))
		if reply ~= false and reply ~= nil then
			-- the server answers the key name where this answers the handle
			reply[1] = names[reply[1]] or reply[1]
		end
		return reply
	end
	if cmd == "zrangestore" then
		-- `key` is the destination (the class object the command writes), args[1]
		-- is the source, which is also how the wire form reads
		return sortedset.zrangestore(handle_for("zset", op.key),
			handle_for("zset", args[1]), unpack(args_of(args), 2, #args))
	end
	if cmd == "zunionstore" or cmd == "zinterstore" or cmd == "zdiffstore" then
		local dest = handle_for("zset", args[1])
		local rest, i = {}, 3
		while i <= #args do
			if args[i] == "weights" then
				rest[#rest+1] = "weights"
				rest[#rest+1] = args_of(args[i+1])
				i = i + 2
			else
				rest[#rest+1] = args[i]
				i = i + 1
			end
		end
		return sortedset[cmd](dest, handle_list("zset", args[2]), unpack(rest))
	end
	local h = handle_for("zset", op.key)
	return h[cmd](h, unpack(args_of(args)))
end

function CALL.geo(op)
	local h = handle_for("geo", op.key)
	if op.cmd == "geosearchstore" then
		-- args[1] is the destination handle, `key` the source, and the destination
		-- may be a geoset or a plain sorted set (Redis writes a zset there)
		local dst = handles[op.args[1]] or handle_for("zset", op.args[1])
		local rest = args_of(op.args, 2)
		return geo.geosearchstore(dst, h, unpack(rest))
	end
	return h[op.cmd](h, unpack(args_of(op.args)))
end

-- The class metatable, taken off a throwaway handle: a method reached through it
-- checks its receiver's kind and answers Redis' WRONGTYPE. Reaching the same
-- function off the handle (`h[cmd]`) cannot work for the records whose `key` names a
-- key of another type -- a bitmap userdata has no `sadd` field to find, so the call
-- would die in Lua instead of answering what a server answers. Same shape as
-- tests/redislib_smoke.lua's set section.
local set_class = getmetatable(set.new())

function CALL.set(op)
	local args, cmd = op.args, op.cmd
	if cmd == "sunion" or cmd == "sinter" or cmd == "sdiff" then
		-- the read forms. Their operand list is flat, not nested the way ZUNION's is,
		-- because the wire form of these three is the original `SINTER key [key ...]`
		-- with no numkeys to insert: `key` is the first operand and `args` the rest.
		local srcs = handle_list("set", args)
		table.insert(srcs, 1, handle_for("set", op.key))
		return set[cmd](srcs)
	end
	if cmd == "sintercard" or cmd == "sunioncard" or cmd == "sdiffcard" then
		-- The only shape the log can carry for these three is the operand list that
		-- is empty on both sides of the call; gen.lua's BAD table says why their
		-- answers are compared by interop.sh instead.
		return set[cmd](handle_list("set", args[1] or {}))
	end
	if cmd == "smove" then
		-- the receiver is the source and the destination is argument 1, which is how
		-- the wire form reads too; the reply is 0 or 1, never a name, so unlike
		-- zmpop's there is nothing to substitute here
		return set.smove(handle_for("set", op.key), handle_for("set", args[1]), args[2])
	end
	return set_class[cmd](handle_for("set", op.key), unpack(args_of(args)))
end

-- The state snapshot uses only commands that exist on both sides. A geoset is a
-- sorted set, so its probe loads its own blob into a sorted-set handle and reads
-- that: the same flat member/score list as the server's ZRANGE ... WITHSCORES, and
-- it exercises spec 4's load() on the way.
--
-- The set family has no branch here on purpose: no set record asks for a probe.
-- `GET`/`BITCOUNT`, `PFCOUNT` and `ZRANGE`/`ZCARD` are what the other side of that
-- comparison answers for a set key, and all three are a WRONGTYPE; the set's own
-- reads (SCARD, SMEMBERS, SISMEMBER, SMISMEMBER) are generated as ordinary records
-- instead, which is the same information through a command both sides have.
local function probe(op)
	local class, key = op.class, op.key
	local h = handles[key]
	if class == "bitmap" then
		if h == nil then return { value = cjson.null, bits = 0 } end
		local d = h:dump()
		if d == "" then return { value = cjson.null, bits = 0 } end
		return { value = hex(d), bits = h:bitcount() }
	end
	if class == "hll" then
		if h == nil then return { count = 0, value = cjson.null } end
		local d = h:dump()
		if d == FRESH_HLL then return { count = h:pfcount(), value = cjson.null } end
		return { count = h:pfcount(), value = hex(d) }
	end
	if h == nil then return { range = {}, card = 0 } end
	if class == "geo" then
		local z = sortedset.load(h:dump())
		return { range = normalize(z:zrange(0, -1, "withscores")), card = z:zcard() }
	end
	return { range = normalize(h:zrange(0, -1, "withscores")), card = h:zcard() }
end

skynet.start(function()
	local ino = assert(io.open(assert(skynet.getenv("redisdiff_oplog")), "rb"))
	local outo = assert(io.open(assert(skynet.getenv("redisdiff_out")), "wb"))

	local n, errs = 0, 0
	for line in ino:lines() do
		if line:match("%S") then
			local ok, op = pcall(cjson.decode, line)
			assert(ok, "bad oplog line: " .. line)
			n = n + 1
			local rec = { seq = op.seq, class = op.class, key = op.key, cmd = op.cmd }
			local called, reply = pcall(function() return CALL[op.class](op) end)
			if called then
				rec.reply = normalize(reply)
			else
				-- strip the position prefix LuaJIT puts on errors from C, the same
				-- way tests/redislib_smoke.lua does, leaving Redis' own text
				local msg = tostring(reply):gsub("^.-:%d+: ", "", 1)
				rec.reply = { error = msg }
				errs = errs + 1
			end
			if op.probe then
				local pok, state = pcall(probe, op)
				rec.state = pok and state or { fatal = tostring(state) }
			end
			outo:write(cjson.encode(rec) .. "\n")
		end
	end
	ino:close()
	outo:close()
	io.stdout:write("replay_lib.lua: ", n, " ops replayed, ", errs,
		" raised an error reply -> ", skynet.getenv("redisdiff_out"), "\n")
	os.exit(0)
end)
