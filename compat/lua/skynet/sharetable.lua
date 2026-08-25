local skynet = require "skynet"
local service = require "skynet.service"
local core = require "skynet.sharedata.corelib"

-- LuaJIT cannot safely reuse a Lua Table* across independent Lua states.
-- This backend stores immutable trees in sharedata's process-wide C heap and
-- exposes table proxies in each service while preserving sharetable's API.
local function sharetable_service()
	local skynet = require "skynet"
	local core = require "skynet.sharedata.corelib"
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

	local function validate_tree(value, seen, path)
		if type(value) ~= "table" then
			return
		end
		if seen[value] then
			error("sharetable LuaJIT backend requires an acyclic tree; repeated table at " .. path)
		end
		seen[value] = true
		for key, child in pairs(value) do
			local key_type = type(key)
			if key_type ~= "string" and
				(key_type ~= "number" or math.tointeger(key) == nil) then
				error("unsupported sharetable key type " .. key_type .. " at " .. path)
			end
			local child_type = type(child)
			if child_type == "table" then
				validate_tree(child, seen, path .. "." .. tostring(key))
			elseif child_type ~= "nil" and child_type ~= "number" and
				child_type ~= "string" and child_type ~= "boolean" then
				error("unsupported sharetable value type " .. child_type .. " at " .. path)
			end
		end
	end

	local function install(name, value)
		assert(type(name) == "string")
		assert(type(value) == "table", "sharetable source must return a table")
		validate_tree(value, {}, name)

		local object = core.host.new(value)
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
		install(name, chunk(...))
	end

	local command = {}

	function command.loadtable(name, value)
		install(name, value)
		return true
	end

	function command.loadfile(name, filename, ...)
		load_source(name, "@" .. filename, ...)
		return true
	end

	function command.loadstring(name, source, ...)
		load_source(name, source, ...)
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
local records = {}

local function proxy_ipairs_iterator(object, index)
	index = index + 1
	local value = object[index]
	if value ~= nil then
		return index, value
	end
end

local function proxy_ipairs(object)
	return proxy_ipairs_iterator, object, 0
end

local function configure_proxy(object)
	local metatable = getmetatable(object)
	if metatable and metatable.__ipairs == nil then
		metatable.__ipairs = proxy_ipairs
	end
	return object
end

local function get_address()
	if not address then
		address = service.new("sharetable_luajit", sharetable_service)
	end
	return address
end

local function remember(name, object, pointer)
	local map = records[name]
	if not map then
		map = setmetatable({}, { __mode = "k" })
		records[name] = map
	end
	map[object] = true
	cache[name] = object
	skynet.send(get_address(), "lua", "confirm", pointer)
	return object
end

local function box(name, pointer)
	if pointer == nil then
		return nil
	end
	local current = cache[name]
	if current and current.__obj == pointer then
		skynet.send(get_address(), "lua", "confirm", pointer)
		return current
	end
	return remember(name, configure_proxy(core.box(pointer)), pointer)
end

local sharetable = {}

function sharetable.loadtable(name, value)
	assert(type(value) == "table")
	return skynet.call(get_address(), "lua", "loadtable", name, value)
end

function sharetable.loadfile(filename, ...)
	return skynet.call(get_address(), "lua", "loadfile", filename, filename, ...)
end

function sharetable.loadstring(name, source, ...)
	return skynet.call(get_address(), "lua", "loadstring", name, source, ...)
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
	for index = 1, select("#", ...) do
		local name = select(index, ...)
		local map = records[name]
		if map then
			local pointer = skynet.call(get_address(), "lua", "query", name)
			if pointer then
				for object in pairs(map) do
					if object.__obj ~= pointer then
						core.update(object, pointer)
					end
				end
				skynet.send(get_address(), "lua", "confirm", pointer)
			end
		end
	end
end

return sharetable
