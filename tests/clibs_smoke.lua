local skynet = require "skynet"

local steps

local function step(name)
	steps:write(name, "\n")
	steps:flush()
end

local function test_cjson()
	local cjson = require "cjson"
	local decoded = cjson.decode(cjson.encode({
		number = 42,
		string = "skynetjit",
		array = { 1, 2, 3 },
		nested = { ok = true },
	}))
	assert(decoded.number == 42 and decoded.string == "skynetjit")
	assert(#decoded.array == 3 and decoded.nested.ok)
end

local function test_lfs()
	local lfs = require "lfs"
	assert(type(lfs.currentdir()) == "string")
	assert(lfs.attributes("lualib/skynet.lua", "mode") == "file")
end

local function test_zlib()
	local zlib = require "zlib"
	local sample = string.rep("skynetjit-zlib-sample-", 512)
	local deflated = zlib.deflate()
	local compressed, done = deflated(sample, "finish")
	assert(done and #compressed > 0)
	local inflated = zlib.inflate()
	local restored, idone = inflated(compressed)
	assert(idone and restored == sample)
	local crc32 = zlib.crc32()
	local sum = crc32("abc")
	assert(type(sum) == "number")
end

local function test_sqlite3()
	local sqlite3 = require "sqlite3"
	local env = sqlite3.sqlite3()
	local conn = assert(env:connect(":memory:"))
	assert(conn:execute("create table smoke(id integer primary key, name text)"))
	assert(conn:execute("insert into smoke values (1, 'alpha')"))
	local cursor = assert(conn:execute("select id, name from smoke where id = 1"))
	local id, name = cursor:fetch()
	assert(id == 1 and name == "alpha")
	cursor:close()
	conn:close()
	env:close()
end

local function test_zset()
	local zset = require "zset"
	local ranked = zset.new()
	ranked:add(10, "first")
	ranked:add(20, "second")
	ranked:add(30, "third")
	assert(ranked:count() == 3)
	local members = ranked:range(1, 3)
	assert(members[1] == "first" and members[3] == "third")
	assert(ranked:score("second") == 20)
end

local function test_luacurl()
	local curl = require "luacurl"
	local handle = curl.easy()
	handle:setopt(curl.OPT_URL, "http://127.0.0.1:1/")
	handle:setopt(curl.OPT_NOSIGNAL, true)
	local received = 0
	handle:setopt(curl.OPT_WRITEFUNCTION, function(chunk)
		received = received + #chunk
		return #chunk
	end)
	handle:close()
end

local function test_pb()
	local pb = require "pb"
	local protoc = require "protoc"
	protoc:load [[
		package smoke;
		message Point {
			optional double x = 1;
			optional double y = 2;
			optional string label = 3;
		}
	]]
	local encoded = assert(pb.encode("smoke.Point", { x = 1.5, y = -2.5, label = "p" }))
	local point = assert(pb.decode("smoke.Point", encoded))
	assert(point.x == 1.5 and point.y == -2.5 and point.label == "p")
end

local tests = {
	{ "cjson", test_cjson },
	{ "lfs", test_lfs },
	{ "zlib", test_zlib },
	{ "sqlite3", test_sqlite3 },
	{ "zset", test_zset },
	{ "luacurl", test_luacurl },
	{ "pb", test_pb },
}

local function run()
	steps = assert(io.open("clibs-steps.txt", "wb"))
	local selection
	local argstr = skynet.getenv("clibs_select")
	if argstr and argstr ~= "" then
		selection = {}
		for name in argstr:gmatch("[^,]+") do
			selection[name] = true
		end
	end
	for _, item in ipairs(tests) do
		if selection == nil or selection[item[1]] then
			step("start " .. item[1])
			item[2]()
			step("done " .. item[1])
		end
	end
	steps:close()
	local success_file = assert(io.open("clibs-smoke.ok", "wb"))
	success_file:write("ok\n")
	success_file:close()
	io.stdout:write("clibs-smoke: NetBull luaclib modules succeeded\n")
	io.stdout:flush()
	return true
end

skynet.start(function()
	local ok, message = xpcall(run, debug.traceback)
	if not ok then
		io.stderr:write("clibs-smoke: failed\n", tostring(message), "\n")
		io.stderr:flush()
		os.exit(1)
	end
	os.exit(0)
end)
