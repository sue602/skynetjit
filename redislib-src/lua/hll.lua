-- hll.lua -- business-facing module for the redislib HyperLogLog: Redis'
-- PFADD PFCOUNT PFMERGE PFDEBUG PFSELFTEST over an in-memory HLL string. The C
-- module is `redis.hll`; this file adds the uppercase command names (spec §3.1).
-- dump()/load()/free() keep their lower-case names, and dump() is the Redis
-- string value itself -- byte for byte what GET of such a key returns, not
-- Redis' DUMP payload.

local c = require "redis.hll"

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
