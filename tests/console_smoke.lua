local skynet = require "skynet"

skynet.start(function()
	assert(skynet.newservice("console"))
	skynet.exit()
end)
