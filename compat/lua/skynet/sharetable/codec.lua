local bridge = require "skynet.sharetable.bridge"

local codec = {
	NIL = 0,
	NUMBER = 1,
	STRING = 2,
	BOOLEAN = 3,
	TABLE = 4,
	LIGHTUSERDATA = 5,
	FUNCTION = 6,
}

local raw_next = next
local raw_get = rawget
local raw_length = rawlen or function(value) return #value end
local getmetatable = getmetatable
local getupvalue = debug.getupvalue
local getinfo = debug.getinfo

local function hex(value)
	return (value:gsub(".", function(byte)
		return string.format("%02x", string.byte(byte))
	end))
end

local function pointer_encoding(value)
	local ok, encoded = pcall(bridge.pack_lightuserdata, value)
	if not ok then
		return nil
	end
	return encoded
end

local function find_path(root, target, maximum_depth)
	local visited = {}
	local function visit(container, path, depth)
		if visited[container] then
			return nil
		end
		visited[container] = true

		local keys = {}
		local key = raw_next(container)
		while key ~= nil do
			if type(key) == "string" then
				keys[#keys + 1] = key
			end
			key = raw_next(container, key)
		end
		table.sort(keys)

		for _, entry_key in ipairs(keys) do
			if raw_get(container, entry_key) == target then
				local result = {}
				for index = 1, #path do result[index] = path[index] end
				result[#result + 1] = entry_key
				return result
			end
		end
		if depth == maximum_depth then
			return nil
		end
		for _, entry_key in ipairs(keys) do
			local child = raw_get(container, entry_key)
			if type(child) == "table" then
				path[#path + 1] = entry_key
				local result = visit(child, path, depth + 1)
				path[#path] = nil
				if result then return result end
			end
		end
	end
	return visit(root, {}, 0)
end

local function function_reference(value)
	local path = find_path(_G, value, 3)
	if path then
		return { scope = "global", path = path }
	end

	local loaded = package and package.loaded
	if type(loaded) ~= "table" then
		return nil
	end
	local modules = {}
	local name = raw_next(loaded)
	while name ~= nil do
		if type(name) == "string" and type(raw_get(loaded, name)) == "table" then
			modules[#modules + 1] = name
		end
		name = raw_next(loaded, name)
	end
	table.sort(modules)
	for _, module_name in ipairs(modules) do
		path = find_path(raw_get(loaded, module_name), value, 3)
		if path then
			return { scope = "module", module = module_name, path = path }
		end
	end
end

local function function_encoding(value)
	local upvalue = getupvalue(value, 1)
	if upvalue ~= nil then
		error("sharetable functions cannot have upvalues", 3)
	end
	if getinfo(value, "S").what ~= "C" then
		return "L", string.dump(value)
	end
	local reference = function_reference(value)
	if reference then
		return "R", reference
	end
	local ok, encoded = pcall(bridge.pack_cfunction, value)
	if ok then
		return "C", encoded
	end
	error("sharetable cannot encode an unnamed LuaJIT fast function", 3)
end

local function key_order(value)
	local value_type = type(value)
	if value_type == "number" then
		return "1:" .. string.format("%+.17g", value)
	elseif value_type == "string" then
		return "2:" .. hex(value)
	elseif value_type == "boolean" then
		return value and "3:1" or "3:0"
	elseif value_type == "userdata" then
		local encoded = pointer_encoding(value)
		if encoded then
			return "4:" .. hex(encoded)
		end
	elseif value_type == "function" then
		local kind, encoded = function_encoding(value)
		if kind == "R" then
			local module = encoded.module or ""
			return "5:R:" .. module .. ":" .. table.concat(encoded.path, ".")
		end
		return "5:" .. kind .. ":" .. hex(encoded)
	elseif value_type == "table" then
		return "6:" .. tostring(value)
	end
	error("unsupported sharetable key type " .. value_type, 3)
end

local function path_token(value, ordinal)
	local value_type = type(value)
	if value_type == "string" then
		return "s" .. hex(value)
	elseif value_type == "number" then
		return "n" .. string.format("%+.17g", value)
	elseif value_type == "boolean" then
		return value and "b1" or "b0"
	end
	return string.sub(value_type, 1, 1) .. ordinal
end

function codec.encode(root)
	assert(type(root) == "table", "sharetable source must return a table")

	local nodes = {}
	local functions = {}
	local table_ids = {}
	local function_ids = {}
	local allocated_paths = {}
	local encode_value

	local function unique_path(suggested)
		local path = suggested
		local suffix = 1
		while allocated_paths[path] do
			suffix = suffix + 1
			path = suggested .. "#" .. suffix
		end
		allocated_paths[path] = true
		return path
	end

	local function encode_function(value, suggested)
		local id = function_ids[value]
		if id then
			return { codec.FUNCTION, id }
		end
		id = unique_path(suggested)
		function_ids[value] = id
		local kind, encoded = function_encoding(value)
		functions[id] = { kind, encoded }
		return { codec.FUNCTION, id }
	end

	local function encode_table(value, suggested)
		local id = table_ids[value]
		if id then
			return { codec.TABLE, id }
		end
		if getmetatable(value) ~= nil then
			error("sharetable cannot encode a table with a metatable", 3)
		end

		id = unique_path(suggested)
		table_ids[value] = id
		local node = { length = raw_length(value), entries = {} }
		nodes[id] = node

		local keys = {}
		local key = raw_next(value)
		while key ~= nil do
			keys[#keys + 1] = key
			key = raw_next(value, key)
		end
		table.sort(keys, function(left, right)
			return key_order(left) < key_order(right)
		end)

		for index, entry_key in ipairs(keys) do
			local token = path_token(entry_key, index)
			node.entries[index] = {
				encode_value(entry_key, id .. "/k/" .. token),
				encode_value(value[entry_key], id .. "/v/" .. token),
			}
		end
		return { codec.TABLE, id }
	end

	function encode_value(value, suggested)
		local value_type = type(value)
		if value_type == "nil" then
			return { codec.NIL }
		elseif value_type == "number" then
			return { codec.NUMBER, value }
		elseif value_type == "string" then
			return { codec.STRING, value }
		elseif value_type == "boolean" then
			return { codec.BOOLEAN, value }
		elseif value_type == "table" then
			return encode_table(value, suggested)
		elseif value_type == "function" then
			return encode_function(value, suggested)
		elseif value_type == "userdata" then
			local encoded = pointer_encoding(value)
			if encoded then
				return { codec.LIGHTUSERDATA, encoded }
			end
		end
		error("unsupported sharetable value type " .. value_type, 3)
	end

	return {
		format = 2,
		root = encode_table(root, "$"),
		nodes = nodes,
		functions = functions,
	}
end

function codec.decode(document)
	assert(type(document) == "table" and document.format == 2,
		"invalid LuaJIT sharetable graph")
	local tables = {}
	local functions = {}
	local decode_value

	local function decode_function(id)
		local value = functions[id]
		if value then return value end
		local definition = assert(document.functions[id], "invalid shared function")
		value = codec.unpack_function(definition[1], definition[2])
		functions[id] = value
		return value
	end

	local function decode_table(id)
		local value = tables[id]
		if value then return value end
		value = {}
		tables[id] = value
		local node = assert(document.nodes[id], "invalid shared table")
		for index = 1, #node.entries do
			local entry = node.entries[index]
			value[decode_value(entry[1])] = decode_value(entry[2])
		end
		return value
	end

	function decode_value(descriptor)
		local tag = descriptor[1]
		local value = descriptor[2]
		if tag == codec.NIL then
			return nil
		elseif tag == codec.NUMBER or tag == codec.STRING or tag == codec.BOOLEAN then
			return value
		elseif tag == codec.TABLE then
			return decode_table(value)
		elseif tag == codec.LIGHTUSERDATA then
			return codec.unpack_lightuserdata(value)
		elseif tag == codec.FUNCTION then
			return decode_function(value)
		end
		error("invalid shared value tag " .. tostring(tag), 2)
	end

	return decode_value(document.root)
end

function codec.unpack_lightuserdata(encoded)
	return bridge.unpack_lightuserdata(encoded)
end

function codec.unpack_function(kind, encoded)
	if kind == "C" then
		return bridge.unpack_cfunction(encoded)
	elseif kind == "R" then
		local value
		if encoded.scope == "global" then
			value = _G
		elseif encoded.scope == "module" then
			value = require(encoded.module)
		else
			error("invalid shared function reference scope")
		end
		for index = 1, #encoded.path do
			value = assert(value[encoded.path[index]],
				"shared function reference is unavailable")
		end
		assert(type(value) == "function", "shared function reference is not callable")
		return value
	end
	local function_value, message = loadstring(encoded, "=sharetable")
	assert(function_value, message)
	return function_value
end

function codec.pack_lightuserdata(value)
	return pointer_encoding(value)
end

return codec
