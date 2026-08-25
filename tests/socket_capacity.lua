local skynet = require "skynet"
local socket = require "skynet.socket"

local SOCKET_COUNT = 8192

local function close_all(sockets)
	for index, id in ipairs(sockets) do
		socket.abandon(id)
		socket.close_fd(id)
		if index % 512 == 0 then
			skynet.sleep(1)
		end
	end
end

local function run()
	local sockets = {}
	local expected = {}
	for index = 1, SOCKET_COUNT do
		local id = assert(socket.udp())
		sockets[index] = id
		expected[id] = true
		if index % 512 == 0 then
			skynet.sleep(1)
		end
	end

	-- Let the socket thread consume every queued registration before inspecting it.
	skynet.sleep(10)
	local registered = 0
	for _, info in ipairs(socket.netstat()) do
		if expected[info.id] then
			registered = registered + 1
		end
	end
	assert(registered == SOCKET_COUNT,
		("expected %d registered sockets, got %d"):format(
			SOCKET_COUNT, registered))

	close_all(sockets)
	skynet.sleep(10)
	local marker = assert(io.open("socket-capacity.ok", "wb"))
	marker:write("ok\n")
	marker:close()
	io.stdout:write(("socket-capacity: %d sockets registered\n"):format(
		SOCKET_COUNT))
	io.stdout:flush()
end

skynet.start(function()
	local ok, message = xpcall(run, debug.traceback)
	if not ok then
		io.stderr:write("socket-capacity: failed\n", tostring(message), "\n")
		io.stderr:flush()
		os.exit(1)
	end
	os.exit(0)
end)
