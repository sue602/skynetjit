-- geo.lua -- business-facing module for the redislib geoset: Redis' GEOADD
-- GEODIST GEOHASH GEOPOS GEORADIUS[_RO] GEORADIUSBYMEMBER[_RO] GEOSEARCH
-- GEOSEARCHSTORE over a sorted set whose scores are 52-bit geohashes. The C
-- module is `redis.geo`; this file adds the uppercase command names and the
-- table form of GEOADD's elements (spec §3.1's dual-form input).

local c = require "redis.geo"

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

-- GEOADD key [CH] [NX|XX] lon lat name [lon lat name ...], in either shape
-- (spec §3.1): the flat varargs, or one list of {lon, lat, member} triples.
-- The C layer implements the first, so a list is flattened here and the option
-- words that precede it keep their positions.
local function flatten_triples(list)
	local out = {}
	for i = 1, #list do
		local t = list[i]
		if type(t) ~= "table" or #t ~= 3 then
			error("geoadd element "..i.." must be {longitude, latitude, member}", 2)
		end
		out[#out+1] = t[1]
		out[#out+1] = t[2]
		out[#out+1] = t[3]
	end
	return out
end

local raw_geoadd = class.geoadd
local function geoadd(o, ...)
	local args = table.pack(...)
	local flat, seen_list = {}, false
	for i = 1, args.n do
		local a = args[i]
		-- only a table whose first item is a triple is the element list; a stray
		-- table in a member position is the C layer's own type error to report
		if type(a) == "table" and type(a[1]) == "table" then
			if seen_list then error("geoadd accepts one element list", 2) end
			seen_list = true
			local t = flatten_triples(a)
			for j = 1, #t do flat[#flat+1] = t[j] end
		elseif seen_list then
			error("geoadd takes the options before the element list", 2)
		else
			flat[#flat+1] = a
		end
	end
	return raw_geoadd(o, unpack(flat))
end

class.geoadd = geoadd

alias_all(class)
alias_all(c)

return c
