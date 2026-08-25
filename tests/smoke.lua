package.path = "./lualib/?.lua;./lualib/?/init.lua;" .. package.path
package.cpath = "./luaclib/?.so;" .. package.cpath

assert(require "skynetjit.compat")
assert(jit.arch == "x64", "LuaJIT is not running in x64 mode")
assert(require("ffi").sizeof("void *") == 8, "pointer size is not 64-bit")

local encoded = string.pack(">I2<I4s1z", 0x1234, 0x78563412, "abc", "tail")
local a, b, c, d, next_position = string.unpack(">I2<I4s1z", encoded)
assert(a == 0x1234 and b == 0x78563412)
assert(c == "abc" and d == "tail" and next_position == #encoded + 1)
assert(string.packsize("<I2I4d") == 14)

assert(require "bson")
assert(require "sproto.core")
assert(require "lpeg")
assert(require "md5")
assert(require "client.crypt")
local core_loader, core_error = package.loadlib("./luaclib/skynet.so", "luaopen_skynet_core")
assert(core_loader, core_error)
local client_loader, client_error = package.loadlib("./luaclib/client.so", "luaopen_client_socket")
assert(client_loader, client_error)
assert(require "skynet.crypt")

local ok, message = pcall(require, "skynet.sharetable.core")
assert(not ok and tostring(message):find("unavailable with the LuaJIT backend", 1, true))

io.stdout:write("smoke: LuaJIT x64 and core modules loaded successfully\n")
