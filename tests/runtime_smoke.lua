local skynet = require "skynet"
local socket = require "skynet.socket"
local sharetable = require "skynet.sharetable"

local function run()
	local marker
	local function marker_owner() return marker end
	local pointer = debug.upvalueid(marker_owner, 1)
	local child = { value = 42 }
	local function transform(value) return value + 1 end
	local source = {
		version = 1,
		message = "LuaJIT sharetable",
		values = { 10, 20, 30 },
		first = child,
		second = child,
		transform = transform,
		cfunction = math.abs,
		pointer = pointer,
	}
	source.self = source
	source[child] = "table-key"
	source[transform] = "function-key"
	source[math.abs] = "cfunction-key"
	source[pointer] = "pointer-key"
	source[1.5] = "float-key"
	source[false] = "boolean-key"
	sharetable.loadtable("runtime_smoke", source)

	local shared = assert(sharetable.query("runtime_smoke"))
	assert(shared.version == 1 and shared.values[2] == 20)
	assert(shared.self == shared, "cycle identity was not preserved")
	assert(shared.first == shared.second, "repeated table identity was not preserved")
	assert(shared[shared.first] == "table-key")
	assert(shared.transform(10) == 11 and shared[shared.transform] == "function-key")
	assert(shared[math.abs] == "cfunction-key")
	assert(shared.cfunction(-7) == 7)
	assert(shared.pointer == pointer and shared[pointer] == "pointer-key")
	assert(shared[1.5] == "float-key" and shared[false] == "boolean-key")
	assert(#shared.values == 3)
	local sum = 0
	for _, value in ipairs(shared.values) do
		sum = sum + value
	end
	assert(sum == 60)
	local saw_version = false
	local key
	while true do
		key = next(shared, key)
		if key == nil then break end
		if key == "version" then saw_version = true end
	end
	assert(saw_version and rawget(shared, "message") == "LuaJIT sharetable")
	local mutation_ok = pcall(function() shared.version = 99 end)
	assert(not mutation_ok, "sharetable proxy must be read-only")
	local raw_mutation_ok = pcall(rawset, shared, "version", 99)
	assert(not raw_mutation_ok, "rawset must preserve sharetable immutability")
	assert(not pcall(table.insert, shared.values, 99))
	assert(not pcall(table.remove, shared.values))
	assert(not pcall(table.sort, shared.values))
	assert(not pcall(table.move, { 1 }, 1, 1, 1, shared.values))

	sharetable.loadstring("runtime_smoke", [[
		local version, pointer = ...
		local child = { value = 84 }
		local function transform(value) return value * 2 end
		local root = {
			version = version,
			message = "updated",
			values = { 40, 50 },
			first = child,
			second = child,
			transform = transform,
			cfunction = math.abs,
			pointer = pointer,
		}
		root.self = root
		root[child] = "updated-table-key"
		root[transform] = "updated-function-key"
		root[math.abs] = "updated-cfunction-key"
		root[pointer] = "updated-pointer-key"
		root[1.5] = "updated-float-key"
		root[false] = "updated-boolean-key"
		return root
	]], 2, pointer)
	assert(shared.version == 1, "old view changed before sharetable.update")
	local retained_child = shared.first
	sharetable.update("runtime_smoke")
	assert(shared.version == 2 and shared.values[1] == 40)
	assert(shared.self == shared and shared.first == shared.second)
	assert(shared.first == retained_child and retained_child.value == 84)
	assert(shared[shared.first] == "updated-table-key")
	assert(shared.transform(6) == 12 and shared[shared.transform] == "updated-function-key")
	assert(shared[math.abs] == "updated-cfunction-key")
	assert(shared.cfunction(-9) == 9)
	assert(shared.pointer == pointer and shared[pointer] == "updated-pointer-key")
	assert(shared[1.5] == "updated-float-key" and
		shared[false] == "updated-boolean-key")
	local ordinary = sharetable.copy(shared)
	assert(ordinary.self == ordinary and ordinary.first == ordinary.second)
	assert(ordinary[ordinary.first] == "updated-table-key")
	table.insert(ordinary.values, 60)
	assert(ordinary.values[3] == 60)
	assert(sharetable.queryall({ "runtime_smoke" }).runtime_smoke.version == 2)
	sharetable.loadstring("runtime_function_args", [[
		local lua_function, c_function, value = ...
		return {
			lua_result = lua_function(value),
			c_result = c_function(-value),
		}
	]], transform, math.abs, 5)
	local function_args = assert(sharetable.query("runtime_function_args"))
	assert(function_args.lua_result == 6 and function_args.c_result == 5)

	local fixture = "../../tests/sharetable_fixture.lua"
	sharetable.loadfile(fixture, 3)
	assert(sharetable.query(fixture).version == 3)
	local all = sharetable.queryall()
	assert(all.runtime_smoke.version == 2 and all[fixture].source == "loadfile")

	local captured = 1
	local function closure() return captured end
	assert(not pcall(sharetable.loadtable, "unsupported_closure", { closure }),
		"functions with upvalues must be rejected")
	assert(not pcall(sharetable.loadtable, "unsupported_metatable",
		setmetatable({}, {})), "table metatables must be rejected")
	assert(not pcall(sharetable.loadtable, "unsupported_userdata", { io.stdout }),
		"full userdata must be rejected")

	local id, _, port = assert(socket.listen("127.0.0.1", 0))
	assert(port > 0, "socket.listen must allocate an ephemeral port")
	socket.start(id)
	socket.close(id)

	local success_file = assert(io.open("runtime-smoke.ok", "wb"))
	success_file:write("ok\n")
	success_file:close()
	io.stdout:write("runtime-smoke: Skynet x64 socket loop succeeded\n")
	io.stdout:flush()
	return true
end

skynet.start(function()
	local ok, message = xpcall(run, debug.traceback)
	if not ok then
		io.stderr:write("runtime-smoke: failed\n", tostring(message), "\n")
		io.stderr:flush()
		os.exit(1)
	end
	os.exit(0)
end)
