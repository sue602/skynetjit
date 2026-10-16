-- set.lua -- business-facing module for the redislib set: Redis 8.10.0's SADD
-- SREM SCARD SMEMBERS SISMEMBER SMISMEMBER SMOVE SPOP SRANDMEMBER SUNION SINTER
-- SDIFF SUNIONCARD SINTERCARD SDIFFCARD over an in-memory set. SSCAN is not here
-- for the same reason ZSCAN is not in sortedset.lua: scanning is a keyspace
-- operation and a redislib object is a handle, not a key (spec §3.3, NOTICE §6).
-- The C module is `redis.set`; this file adds the uppercase command names
-- (spec §3.1). dump()/load()/free() keep their lower-case names.
--
-- The operand list of the set operations is a table of handles where Redis has
-- `numkeys key [key ...]`, so `set.sunion({a, b})`, `set.sintercard({a, b},
-- "limit", 3)` and `a:smove(b, "member")`. SMEMBERS and friends reply a member as
-- a string even when Redis stored it as an integer, which is what the wire does.

local c = require "redis.set"

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

alias_all(class)
alias_all(c)

return c
