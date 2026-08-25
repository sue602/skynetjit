local skynet = require "skynet"
local socket = require "skynet.socket"
local sharetable = require "skynet.sharetable"

skynet.start(function()
	sharetable.loadtable("runtime_smoke", {
		version = 1,
		message = "LuaJIT sharetable",
		values = { 10, 20, 30 },
	})
	local shared = assert(sharetable.query("runtime_smoke"))
	assert(shared.version == 1 and shared.values[2] == 20)
	assert(#shared.values == 3)
	local sum = 0
	for _, value in ipairs(shared.values) do
		sum = sum + value
	end
	assert(sum == 60)
	local fields = 0
	for _ in pairs(shared) do
		fields = fields + 1
	end
	assert(fields == 3)
	local mutation_ok = pcall(function() shared.version = 99 end)
	assert(not mutation_ok, "sharetable proxy must be read-only")

	sharetable.loadstring("runtime_smoke", [[
		return { version = ..., message = "updated", values = { 40, 50 } }
	]], 2)
	assert(shared.version == 1, "old view changed before sharetable.update")
	sharetable.update("runtime_smoke")
	assert(shared.version == 2 and shared.values[1] == 40)
	assert(sharetable.queryall({ "runtime_smoke" }).runtime_smoke.version == 2)

	local fixture = "../../tests/sharetable_fixture.lua"
	sharetable.loadfile(fixture, 3)
	assert(sharetable.query(fixture).version == 3)
	local all = sharetable.queryall()
	assert(all.runtime_smoke.version == 2 and all[fixture].source == "loadfile")

	local id = assert(socket.listen("127.0.0.1", 25261))
	socket.start(id)
	socket.close(id)

	io.stdout:write("runtime-smoke: Skynet x64 socket loop succeeded\n")
	io.stdout:flush()
	os.exit(0)
end)
