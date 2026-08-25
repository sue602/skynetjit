local skynet = require "skynet"
local socket = require "skynet.socket"

skynet.start(function()
	local id = assert(socket.listen("127.0.0.1", 25261))
	socket.start(id)
	socket.close(id)

	io.stdout:write("runtime-smoke: Skynet x64 socket loop succeeded\n")
	io.stdout:flush()
	os.exit(0)
end)
