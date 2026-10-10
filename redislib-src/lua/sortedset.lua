-- sortedset.lua -- business-facing module for the redislib sorted set: all 31
-- Z* commands of Redis 8.10.0 except ZSCAN (which needs a keyspace). The C module
-- is `redis.sortedset`; this file adds the uppercase command names, the table
-- form of ZADD's score/member pairs, and the options-table form of the range
-- family with the `pairs` sugar (spec §3.1 and §3.2).

local c = require "redis.sortedset"

local class = getmetatable(c.new())

local NOT_COMMAND = { new = true, load = true, dump = true, free = true }

local function alias_all(t)
	local names = {}
	for name, f in pairs(t) do
		if type(f) == "function" and not NOT_COMMAND[name] and
			name:sub(1, 2) ~= "__" then
			names[#names+1] = name
		end
	end
	for _, name in ipairs(names) do
		t[name:upper()] = t[name]
	end
end

-- ZADD key [NX|XX] [GT|LT] [CH] [INCR] score member [score member ...], also
-- writable as one list of {score, member} pairs (spec §3.1's dual-form input).
-- The option words keep their positions in front of the flattened pairs, so both
-- shapes reach the same C parser. A table only counts as the element list when
-- its first item is a pair table: a stray table in a member position is a type
-- error the C layer reports for itself, not an empty list.
local raw_zadd = class.zadd
local function zadd_shape(o, ...)
	local args = table.pack(...)
	local out, seen_list = {}, false
	for i = 1, args.n do
		local a = args[i]
		if type(a) == "table" and type(a[1]) == "table" then
			if seen_list then error("zadd accepts one element list", 2) end
			seen_list = true
			for j = 1, #a do
				local p = a[j]
				if type(p) ~= "table" or #p ~= 2 then
					error("zadd element "..j.." must be {score, member}", 2)
				end
				out[#out+1] = p[1]
				out[#out+1] = p[2]
			end
		elseif seen_list then
			error("zadd takes the options before the element list", 2)
		else
			out[#out+1] = a
		end
	end
	return raw_zadd(o, unpack(out))
end

-- The range family: `{byscore=true, rev=true, withscores=true, limit={o,c}}`
-- instead of the option words, in the order the server's own grammar uses. Any
-- word the command does not accept is still refused by the C layer, so the two
-- shapes fail and succeed exactly where Redis does. `pairs` is the wrapper's own:
-- it asks for WITHSCORES and re-shapes the flat reply into records.
local OPT_ORDER = { "byscore", "bylex", "rev", "withscores", "limit" }
local OPT_KNOWN = { byscore = true, bylex = true, rev = true,
                    withscores = true, limit = true, pairs = true }

local function shape_options(fn, name)
	return function(...)
		local args = table.pack(...)
		local opts = args.n >= 2 and type(args[args.n]) == "table"
			and args[args.n] or nil

		if opts == nil then
			return fn(...)
		end

		local out = {}
		for i = 1, args.n - 1 do out[#out+1] = args[i] end
		for _, k in ipairs(OPT_ORDER) do
			local v = opts[k]
			if k == "limit" then
				if v then
					if type(v) ~= "table" or #v ~= 2 then
						error(name .. ": limit must be {offset, count}", 2)
					end
					out[#out+1] = "limit"
					out[#out+1] = v[1]
					out[#out+1] = v[2]
				end
			elseif v then
				out[#out+1] = k
			end
		end
		for k in pairs(opts) do
			if not OPT_KNOWN[k] then
				error(name .. ": unknown option " .. tostring(k), 2)
			end
		end
		if opts.pairs and not opts.withscores then
			error(name .. ": pairs needs withscores", 2)
		end

		local reply = fn(unpack(out))
		if not opts.pairs then return reply end

		local records = {}
		for i = 1, #reply, 2 do
			records[#records+1] = { member = reply[i], score = reply[i+1] }
		end
		return records
	end
end

class.zadd = zadd_shape
for _, name in ipairs({ "zrange", "zrevrange", "zrangebyscore",
	"zrevrangebyscore", "zrangebylex", "zrevrangebylex", "zrangestore" }) do
	class[name] = shape_options(class[name], name)
end
-- the function shape of ZRANGESTORE takes the same options, so it gets the same
-- normalization; the other module-level commands already read their key list as
-- one table at the C layer.
c.zrangestore = shape_options(c.zrangestore, "zrangestore")

alias_all(class)
alias_all(c)

return c
