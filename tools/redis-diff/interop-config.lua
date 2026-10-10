-- skynet config for interop_lib.lua (spec 4 round trip). Paths are written by
-- tools/redis-diff/interop.sh, which generates this file's siblings at run time.
root = "./"
luaservice = root .. "service/?.lua;" .. root .. "../../tools/redis-diff/?.lua"
lualoader = root .. "lualib/loader.lua"
lua_path = root .. "lualib/?.lua;" .. root .. "lualib/?/init.lua"
lua_cpath = root .. "luaclib/?.so"
cpath = root .. "cservice/?.so"

thread = 2
logger = nil
harbor = 0

redisdiff_in = "../../tools/redis-diff/work/interop-from-redis.json"
redisdiff_out = "../../tools/redis-diff/work/interop-from-lib.json"
redisdiff_report = "../../tools/redis-diff/work/interop-lib-report.json"

start = "interop_lib"
bootstrap = "snlua bootstrap"
