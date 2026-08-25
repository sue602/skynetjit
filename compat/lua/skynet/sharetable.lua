local skynet = require "skynet"
local service = require "skynet.service"
local core = require "skynet.sharedata.corelib"
local codec = require "skynet.sharetable.codec"
local compatibility = require "skynetjit.compat"

-- The stock sharetable shares Lua's internal Table* across states. LuaJIT's
-- GC and JIT cannot safely do that, so this implementation shares an encoded
-- object graph in sharedata's C heap and exposes immutable Lua table proxies.
local function sharetable_service()
	local skynet = require "skynet"
	local core = require "skynet.sharedata.corelib"
	local codec = require "skynet.sharetable.codec"
	local files = {}
	local retired = {}
	local no_response = {}

	local function collect_retired()
		for index = #retired, 1, -1 do
			local object = retired[index]
			if core.host.getref(object) <= 0 then
				core.host.delete(object)
				table.remove(retired, index)
			end
		end
	end

	local function install(name, document)
		assert(type(name) == "string")
		assert(type(document) == "table" and document.format == 2,
			"invalid LuaJIT sharetable graph")

		local object = core.host.new(document)
		core.host.incref(object) -- owner reference held by files[name]
		local old = files[name]
		files[name] = object
		if old then
			core.host.markdirty(old)
			core.host.decref(old)
			retired[#retired + 1] = old
		end
		collect_retired()
	end

	local function load_source(name, source, ...)
		local chunk, message
		if source:sub(1, 1) == "@" then
			chunk, message = loadfile(source:sub(2))
		else
			chunk, message = loadstring(source, "=" .. name)
		end
		assert(chunk, message)
		local environment = setmetatable({}, { __index = _G })
		setfenv(chunk, environment)
		install(name, codec.encode(chunk(...)))
	end

	local function decode_arguments(document)
		local arguments = codec.decode(document)
		return unpack(arguments, 1, arguments.n)
	end

	local command = {}

	function command.loadgraph(name, document)
		install(name, document)
		return true
	end

	function command.loadfile(name, filename, arguments)
		load_source(name, "@" .. filename, decode_arguments(arguments))
		return true
	end

	function command.loadstring(name, source, arguments)
		load_source(name, source, decode_arguments(arguments))
		return true
	end

	function command.query(name)
		local object = files[name]
		if object then
			core.host.incref(object) -- transient reference until confirm
		end
		return object
	end

	function command.queryall(names)
		local result = {}
		if names then
			for _, name in ipairs(names) do
				local object = files[name]
				if object then
					core.host.incref(object)
					result[name] = object
				end
			end
		else
			for name, object in pairs(files) do
				core.host.incref(object)
				result[name] = object
			end
		end
		return result
	end

	function command.confirm(object)
		core.host.decref(object)
		collect_retired()
		return no_response
	end

	skynet.dispatch("lua", function(_, _, name, ...)
		local handler = assert(command[name], name)
		local result = handler(...)
		if result ~= no_response then
			skynet.retpack(result)
		end
	end)
end

local address
local cache = setmetatable({}, { __mode = "v" })
local roots_by_name = {}
local proxy_record = setmetatable({}, { __mode = "k" })

local proxy_metatable = {}
local proxy_for
local decode

local function get_address()
	if not address then
		address = service.new("sharetable_luajit", sharetable_service)
	end
	return address
end

local function node_for(record)
	local node = record.view.document.nodes[record.id]
	if node == nil then
		error("shared table no longer exists after update", 3)
	end
	return node
end

local function decode_function(view, id)
	local value = view.functions[id]
	if value then
		return value
	end
	local definition = assert(view.document.functions[id], "invalid shared function")
	value = codec.unpack_function(definition[1], definition[2])
	view.functions[id] = value
	return value
end

function decode(view, descriptor)
	local tag = descriptor[1]
	local value = descriptor[2]
	if tag == codec.NIL then
		return nil
	elseif tag == codec.NUMBER or tag == codec.STRING or tag == codec.BOOLEAN then
		return value
	elseif tag == codec.TABLE then
		return proxy_for(view, value)
	elseif tag == codec.LIGHTUSERDATA then
		return codec.unpack_lightuserdata(value)
	elseif tag == codec.FUNCTION then
		return decode_function(view, value)
	end
	error("invalid shared value tag " .. tostring(tag), 3)
end

local function descriptor_matches(view, descriptor, key)
	local tag = descriptor[1]
	local value = descriptor[2]
	if tag == codec.NUMBER then
		return type(key) == "number" and key == value
	elseif tag == codec.STRING then
		return type(key) == "string" and key == value
	elseif tag == codec.BOOLEAN then
		return type(key) == "boolean" and key == value
	elseif tag == codec.TABLE then
		local record = proxy_record[key]
		return record ~= nil and record.view == view and record.id == value
	elseif tag == codec.FUNCTION then
		return type(key) == "function" and decode_function(view, value) == key
	elseif tag == codec.LIGHTUSERDATA then
		local encoded = codec.pack_lightuserdata(key)
		return encoded ~= nil and encoded == value
	end
	return false
end

local function find_entry(record, key)
	local entries = node_for(record).entries
	for index = 1, #entries do
		local entry = entries[index]
		if descriptor_matches(record.view, entry[1], key) then
			return entry, index
		end
	end
	return nil
end

local function proxy_index(object, key)
	local record = assert(proxy_record[object], "invalid shared table proxy")
	local entry = find_entry(record, key)
	if entry then
		return decode(record.view, entry[2])
	end
end

local function proxy_next(object, key)
	local record = assert(proxy_record[object], "invalid shared table proxy")
	local entries = node_for(record).entries
	local index = 1
	if key ~= nil then
		local entry
		entry, index = find_entry(record, key)
		if entry == nil then
			error("invalid key to 'next'", 2)
		end
		index = index + 1
	end
	local entry = entries[index]
	if entry then
		return decode(record.view, entry[1]), decode(record.view, entry[2])
	end
end

local function proxy_pairs(object)
	return proxy_next, object, nil
end

local function proxy_ipairs_iterator(object, index)
	index = index + 1
	local value = proxy_index(object, index)
	if value ~= nil then
		return index, value
	end
end

local function proxy_ipairs(object)
	return proxy_ipairs_iterator, object, 0
end

local function proxy_length(object)
	return node_for(assert(proxy_record[object])).length
end

local function readonly_error(_, key)
	error("attempt to change shared table key " .. tostring(key), 2)
end

proxy_metatable.__index = proxy_index
proxy_metatable.__newindex = readonly_error
proxy_metatable.__len = proxy_length
proxy_metatable.__pairs = proxy_pairs
proxy_metatable.__ipairs = proxy_ipairs
proxy_metatable.__metatable = "skynet.sharetable"

local proxy_operations = {
	next = proxy_next,
	rawget = proxy_index,
	rawset = readonly_error,
}

function proxy_for(view, id)
	local object = view.proxies[id]
	if object then
		return object
	end
	object = setmetatable({}, proxy_metatable)
	compatibility.register_table_proxy(object, proxy_operations)
	view.proxies[id] = object
	proxy_record[object] = { view = view, id = id }
	return object
end

local function new_view(pointer)
	local document = core.box(pointer)
	assert(document.format == 2, "unsupported shared graph format")
	local view = {
		pointer = pointer,
		document = document,
		proxies = setmetatable({}, { __mode = "v" }),
		functions = setmetatable({}, { __mode = "v" }),
	}
	local root = decode(view, document.root)
	assert(type(root) == "table")
	return root, view
end

local function remember(name, object, view, pointer)
	local roots = roots_by_name[name]
	if not roots then
		roots = setmetatable({}, { __mode = "k" })
		roots_by_name[name] = roots
	end
	roots[object] = view
	cache[name] = object
	skynet.send(get_address(), "lua", "confirm", pointer)
	return object
end

local function box(name, pointer)
	if pointer == nil then
		return nil
	end
	local current = cache[name]
	if current then
		local view = proxy_record[current].view
		if view.pointer == pointer then
			skynet.send(get_address(), "lua", "confirm", pointer)
			return current
		end
	end
	local object, view = new_view(pointer)
	return remember(name, object, view, pointer)
end

local sharetable = {}

local function encode_arguments(...)
	return codec.encode({ n = select("#", ...), ... })
end

function sharetable.loadtable(name, value)
	return skynet.call(get_address(), "lua", "loadgraph", name, codec.encode(value))
end

function sharetable.loadfile(filename, ...)
	return skynet.call(get_address(), "lua", "loadfile", filename, filename,
		encode_arguments(...))
end

function sharetable.loadstring(name, source, ...)
	return skynet.call(get_address(), "lua", "loadstring", name, source,
		encode_arguments(...))
end


function sharetable.query(name)
	return box(name, skynet.call(get_address(), "lua", "query", name))
end

function sharetable.queryall(names)
	local pointers = skynet.call(get_address(), "lua", "queryall", names)
	local result = {}
	for name, pointer in pairs(pointers) do
		result[name] = box(name, pointer)
	end
	return result
end


function sharetable.update(...)
	for argument = 1, select("#", ...) do
		local name = select(argument, ...)
		local roots = roots_by_name[name]
		if roots then
			local pointer = skynet.call(get_address(), "lua", "query", name)
			if pointer then
				for _, view in pairs(roots) do
					if view.pointer ~= pointer then
						core.update(view.document, pointer)
						view.pointer = pointer
						view.functions = setmetatable({}, { __mode = "v" })
					end
				end
				skynet.send(get_address(), "lua", "confirm", pointer)
			end
		end
	end
end

local function copy_proxy(value, copies)
	if type(value) ~= "table" or proxy_record[value] == nil then
		return value
	end
	local result = copies[value]
	if result then
		return result
	end
	result = {}
	copies[value] = result
	local key
	while true do
		local next_key, next_value = proxy_next(value, key)
		if next_key == nil then break end
		result[copy_proxy(next_key, copies)] = copy_proxy(next_value, copies)
		key = next_key
	end
	return result
end

-- Extension for C modules or Lua table-library functions that require a real
-- table and intentionally bypass proxy metamethods.
function sharetable.copy(value)
	assert(type(value) == "table" and proxy_record[value] ~= nil,
		"shared table proxy expected")
	return copy_proxy(value, {})
end

return sharetable
