-- bitmap.lua -- business-facing module for the redislib bitmap: Redis'
-- SETBIT GETBIT BITCOUNT BITPOS BITOP BITFIELD BITFIELD_RO over an in-memory
-- string value. The C module is `redis.bitmap` (luaclib/redis/bitmap.so); this
-- file only adds the uppercase command names a caller migrating from a Redis
-- client expects (spec §3.1). dump()/load()/len()/free() keep their lower-case
-- names -- they are the library's own API, not Redis commands -- and dump() in
-- particular is NOT Redis' DUMP, which is an RDB payload.

local c = require "redis.bitmap"

-- The C module keeps the methods in the class metatable, whose __index is that
-- same table, so a key added here becomes a method on every bitmap. A throwaway
-- handle is the cheapest way to reach it from Lua.
local class = getmetatable(c.new())

-- Not a hand-written alias table on purpose: the names come from the module
-- itself, so an alias set cannot drift when the facade gains a command. The
-- exceptions are the members that are not Redis commands.
local NOT_COMMAND = { new = true, load = true, dump = true, free = true,
                      len = true }

local function alias_all(t)
	local names = {}
	for name, f in pairs(t) do
		if type(f) == "function" and not NOT_COMMAND[name] and
			name:sub(1, 2) ~= "__" then
			names[#names+1] = name
		end
	end
	-- a second pass: assigning into t while pairs() walks it is undefined
	for _, name in ipairs(names) do
		t[name:upper()] = t[name]
	end
end

alias_all(class)
alias_all(c)

return c
