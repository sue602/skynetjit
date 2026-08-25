local skynet = require "skynet"
require "skynet.manager"

local marker = assert(io.open("abort-smoke.ok", "wb"))
marker:write("ok\n")
marker:close()
io.stdout:write("abort-smoke: graceful shutdown requested\n")
io.stdout:flush()
skynet.abort()
