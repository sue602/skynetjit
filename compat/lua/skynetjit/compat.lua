local ffi = require "ffi"

table.unpack = table.unpack or unpack
table.pack = table.pack or function(...)
	return { n = select("#", ...), ... }
end

-- Lua-level raw operations need an escape hatch for immutable external table
-- proxies. Native tables still use the original builtins unchanged.
local native_next = next
local native_rawget = rawget
local native_rawset = rawset
local native_table_insert = table.insert
local native_table_remove = table.remove
local native_table_sort = table.sort
local native_table_move = table.move
local proxy_operations = setmetatable({}, { __mode = "k" })
local compatibility = {}
local install_table_guards

function compatibility.register_table_proxy(value, operations)
	assert(type(value) == "table")
	proxy_operations[value] = operations
	install_table_guards()
end

local function proxy_hook(value, name)
	if type(value) ~= "table" then
		return nil
	end
	local operations = proxy_operations[value]
	return operations and operations[name]
end

function _G.next(value, key)
	local hook = proxy_hook(value, "next")
	if hook then
		return hook(value, key)
	end
	return native_next(value, key)
end

function _G.rawget(value, key)
	local hook = proxy_hook(value, "rawget")
	if hook then
		return hook(value, key)
	end
	return native_rawget(value, key)
end

function _G.rawset(value, key, entry)
	local hook = proxy_hook(value, "rawset")
	if hook then
		return hook(value, key, entry)
	end
	return native_rawset(value, key, entry)
end

local table_guards_installed = false
install_table_guards = function()
	if table_guards_installed then return end
	table_guards_installed = true

	function table.insert(value, ...)
		local hook = proxy_hook(value, "rawset")
		if hook then return hook(value, "table.insert") end
		return native_table_insert(value, ...)
	end

	function table.remove(value, ...)
		local hook = proxy_hook(value, "rawset")
		if hook then return hook(value, "table.remove") end
		return native_table_remove(value, ...)
	end

	function table.sort(value, ...)
		local hook = proxy_hook(value, "rawset")
		if hook then return hook(value, "table.sort") end
		return native_table_sort(value, ...)
	end

	if native_table_move then
		function table.move(source, first, last, target, destination)
			destination = destination or source
			local hook = proxy_hook(destination, "rawset")
			if hook then return hook(destination, "table.move") end
			if proxy_hook(source, "rawget") then
				error("table.move cannot read a shared table proxy; use sharetable.copy", 2)
			end
			return native_table_move(source, first, last, target, destination)
		end
	end
end

math.maxinteger = math.maxinteger or 9007199254740991
math.mininteger = math.mininteger or -9007199254740991
math.tointeger = math.tointeger or function(value)
	if type(value) ~= "number" or value ~= value or
		value < math.mininteger or value > math.maxinteger or
		value % 1 ~= 0 then
		return nil
	end
	return value
end

-- LuaJIT has no to-be-closed variables, so releasing a suspended coroutine
-- only needs to make it unreachable; Skynet already drops its references.
coroutine.close = coroutine.close or function(co)
	local status = coroutine.status(co)
	if status == "running" or status == "normal" then
		return false, "cannot close a " .. status .. " coroutine"
	end
	return true
end

if string.pack and string.unpack and string.packsize then
	return compatibility
end

local native_little_endian = ffi.abi("le")
local uint64_t = ffi.typeof("uint64_t")
local int64_t = ffi.typeof("int64_t")
local u256 = uint64_t(256)

local function fail(message, level)
	error(message, (level or 1) + 1)
end

local function read_number(format, position, default)
	local start = position
	while position <= #format do
		local byte = format:byte(position)
		if byte < 48 or byte > 57 then
			break
		end
		position = position + 1
	end
	if position == start then
		return default, position
	end
	local value = tonumber(format:sub(start, position - 1))
	if not value or value <= 0 or value > 16 then
		fail("invalid format size", 2)
	end
	return value, position
end

local function padding(offset, alignment, maximum_alignment)
	alignment = math.min(alignment, maximum_alignment)
	if alignment <= 1 then
		return 0
	end
	return (alignment - (offset % alignment)) % alignment
end

local function option_info(option, format, position)
	if option == "b" or option == "B" then
		return 1, 1, position
	elseif option == "h" or option == "H" then
		return 2, 2, position
	elseif option == "l" or option == "L" then
		return ffi.sizeof("long"), ffi.sizeof("long"), position
	elseif option == "j" or option == "J" or option == "T" then
		return 8, 8, position
	elseif option == "i" or option == "I" then
		local size
		size, position = read_number(format, position, 4)
		return size, size, position
	elseif option == "f" then
		return 4, 4, position
	elseif option == "d" or option == "n" then
		return 8, 8, position
	elseif option == "c" then
		local size
		size, position = read_number(format, position, nil)
		if not size then
			fail("missing size for format option 'c'", 2)
		end
		return size, 1, position
	elseif option == "s" then
		local size
		size, position = read_number(format, position, ffi.sizeof("size_t"))
		return size, size, position
	elseif option == "z" or option == "x" then
		return 1, 1, position
	end
	return nil, nil, position
end

local function integer_bytes(value, size, little_endian)
	local number = ffi.cast(uint64_t, value)
	local bytes = {}
	for index = 1, size do
		bytes[index] = string.char(tonumber(number % u256))
		number = number / u256
	end
	local result = table.concat(bytes)
	return little_endian and result or result:reverse()
end

local function integer_value(data, position, size, little_endian, signed)
	local value = uint64_t(0)
	if little_endian then
		for index = position + size - 1, position, -1 do
			value = value * u256 + data:byte(index)
		end
	else
		for index = position, position + size - 1 do
			value = value * u256 + data:byte(index)
		end
	end
	if signed then
		local sign = 2 ^ (size * 8 - 1)
		if size < 8 and tonumber(value) >= sign then
			return tonumber(value) - 2 ^ (size * 8)
		elseif size == 8 then
			return tonumber(ffi.cast(int64_t, value))
		end
	end
	return tonumber(value)
end

local function float_bytes(value, ctype, size, little_endian)
	local buffer = ffi.new(ctype .. "[1]", value)
	local result = ffi.string(buffer, size)
	if little_endian ~= native_little_endian then
		result = result:reverse()
	end
	return result
end

local function float_value(data, position, ctype, size, little_endian)
	local bytes = data:sub(position, position + size - 1)
	if little_endian ~= native_little_endian then
		bytes = bytes:reverse()
	end
	local buffer = ffi.new(ctype .. "[1]")
	ffi.copy(buffer, bytes, size)
	return tonumber(buffer[0])
end

local function normalize_position(data, position)
	position = position or 1
	if position < 0 then
		position = #data + position + 1
	end
	if position < 1 or position > #data + 1 then
		fail("initial position out of string", 2)
	end
	return position
end

function string.pack(format, ...)
	local arguments = table.pack(...)
	local argument_index = 1
	local format_index = 1
	local output = {}
	local output_size = 0
	local little_endian = native_little_endian
	local maximum_alignment = 1

	local function append(value)
		output[#output + 1] = value
		output_size = output_size + #value
	end

	while format_index <= #format do
		local option = format:sub(format_index, format_index)
		format_index = format_index + 1
		if option:match("%s") then
			-- ignored
		elseif option == "<" then
			little_endian = true
		elseif option == ">" then
			little_endian = false
		elseif option == "=" then
			little_endian = native_little_endian
		elseif option == "!" then
			maximum_alignment, format_index =
				read_number(format, format_index, ffi.sizeof("void *"))
		elseif option == "X" then
			fail("format option 'X' is not supported by the LuaJIT compatibility layer", 2)
		else
			local size, alignment
			size, alignment, format_index = option_info(option, format, format_index)
			if not size then
				fail("invalid format option '" .. option .. "'", 2)
			end
			local pad = padding(output_size, alignment, maximum_alignment)
			if pad > 0 then
				append(string.rep("\0", pad))
			end
			if option == "x" then
				append("\0")
			elseif option == "z" then
				local value = tostring(arguments[argument_index])
				argument_index = argument_index + 1
				if value:find("\0", 1, true) then
					fail("string contains zeros", 2)
				end
				append(value .. "\0")
			elseif option == "c" then
				local value = tostring(arguments[argument_index])
				argument_index = argument_index + 1
				if #value > size then
					fail("string longer than given size", 2)
				end
				append(value .. string.rep("\0", size - #value))
			elseif option == "s" then
				local value = tostring(arguments[argument_index])
				argument_index = argument_index + 1
				append(integer_bytes(#value, size, little_endian))
				append(value)
			elseif option == "f" then
				append(float_bytes(arguments[argument_index], "float", 4, little_endian))
				argument_index = argument_index + 1
			elseif option == "d" or option == "n" then
				append(float_bytes(arguments[argument_index], "double", 8, little_endian))
				argument_index = argument_index + 1
			else
				append(integer_bytes(arguments[argument_index], size, little_endian))
				argument_index = argument_index + 1
			end
		end
	end
	if argument_index <= arguments.n then
		-- Lua ignores extra arguments; keep the same behavior.
	end
	return table.concat(output)
end

function string.unpack(format, data, position)
	position = normalize_position(data, position)
	local format_index = 1
	local little_endian = native_little_endian
	local maximum_alignment = 1
	local results = { n = 0 }

	local function push(value)
		results.n = results.n + 1
		results[results.n] = value
	end

	while format_index <= #format do
		local option = format:sub(format_index, format_index)
		format_index = format_index + 1
		if option:match("%s") then
			-- ignored
		elseif option == "<" then
			little_endian = true
		elseif option == ">" then
			little_endian = false
		elseif option == "=" then
			little_endian = native_little_endian
		elseif option == "!" then
			maximum_alignment, format_index =
				read_number(format, format_index, ffi.sizeof("void *"))
		elseif option == "X" then
			fail("format option 'X' is not supported by the LuaJIT compatibility layer", 2)
		else
			local size, alignment
			size, alignment, format_index = option_info(option, format, format_index)
			if not size then
				fail("invalid format option '" .. option .. "'", 2)
			end
			position = position + padding(position - 1, alignment, maximum_alignment)
			if option == "x" then
				if position > #data then fail("data string too short", 2) end
				position = position + 1
			elseif option == "z" then
				local zero = data:find("\0", position, true)
				if not zero then fail("unfinished string for format 'z'", 2) end
				push(data:sub(position, zero - 1))
				position = zero + 1
			elseif option == "c" then
				if position + size - 1 > #data then fail("data string too short", 2) end
				push(data:sub(position, position + size - 1))
				position = position + size
			elseif option == "s" then
				if position + size - 1 > #data then fail("data string too short", 2) end
				local length = integer_value(data, position, size, little_endian, false)
				position = position + size
				if position + length - 1 > #data then fail("data string too short", 2) end
				push(data:sub(position, position + length - 1))
				position = position + length
			elseif option == "f" then
				if position + 3 > #data then fail("data string too short", 2) end
				push(float_value(data, position, "float", 4, little_endian))
				position = position + 4
			elseif option == "d" or option == "n" then
				if position + 7 > #data then fail("data string too short", 2) end
				push(float_value(data, position, "double", 8, little_endian))
				position = position + 8
			else
				if position + size - 1 > #data then fail("data string too short", 2) end
				local signed = option == "b" or option == "h" or option == "l" or
					option == "j" or option == "i"
				push(integer_value(data, position, size, little_endian, signed))
				position = position + size
			end
		end
	end
	push(position)
	return table.unpack(results, 1, results.n)
end

function string.packsize(format)
	local format_index = 1
	local size = 0
	local maximum_alignment = 1
	while format_index <= #format do
		local option = format:sub(format_index, format_index)
		format_index = format_index + 1
		if option:match("%s") or option == "<" or option == ">" or option == "=" then
			-- no size
		elseif option == "!" then
			maximum_alignment, format_index =
				read_number(format, format_index, ffi.sizeof("void *"))
		elseif option == "X" or option == "s" or option == "z" then
			fail("variable-length format", 2)
		else
			local option_size, alignment
			option_size, alignment, format_index = option_info(option, format, format_index)
			if not option_size then
				fail("invalid format option '" .. option .. "'", 2)
			end
			size = size + padding(size, alignment, maximum_alignment) + option_size
		end
	end
	return size
end

return compatibility
