local skynet = require "skynet"
local socket = require "skynet.socket"

skynet.start(function()
	local accepted = false
	local listener, _, port = assert(socket.listen("127.0.0.1", 0))
	assert(port > 0)
	socket.start(listener, function(client)
		skynet.fork(function()
			assert(socket.start(client))
			assert(socket.readline(client) == "ping")
			socket.write(client, "pong\n")
			socket.close(client)
			accepted = true
		end)
	end)
	-- Allow the accept request to reach the ring before any client connects.
	skynet.sleep(2)

	local client = assert(socket.open("127.0.0.1", port))
	-- Likewise, leave the server receive request pending before the send.
	skynet.sleep(2)
	socket.write(client, "ping\n")
	assert(socket.readline(client) == "pong")
	socket.close(client)
	while not accepted do
		skynet.sleep(1)
	end
	socket.close(listener)

	local udp_done = false
	local udp_server
	local udp_port
	for offset = 0, 20 do
		local candidate = 39000 + ((os.time() + offset) % 1000)
		local ok, id = pcall(function()
			return socket.udp(function(payload, address)
				assert(payload == "udp-ping")
				assert(socket.sendto(udp_server, address, "udp-pong"))
			end, "127.0.0.1", candidate)
		end)
		if ok then
			udp_server = id
			udp_port = candidate
			break
		end
	end
	assert(udp_server, "could not reserve a loopback UDP port")
	local udp_client = socket.udp(function(payload)
		assert(payload == "udp-pong")
		udp_done = true
	end)
	socket.udp_connect(udp_client, "127.0.0.1", udp_port)
	assert(socket.write(udp_client, "udp-ping"))
	while not udp_done do
		skynet.sleep(1)
	end
	socket.close(udp_client)
	socket.close(udp_server)

	local marker = assert(io.open("uring-socket.ok", "wb"))
	marker:write("ok\n")
	marker:close()
	io.stdout:write("uring-socket: completion-driven TCP and UDP round trips succeeded\n")
	io.stdout:flush()
	os.exit(0)
end)
