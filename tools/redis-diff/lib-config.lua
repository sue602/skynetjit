-- skynet config for replay_lib.lua. Run from the built output directory:
--   cd build/linux-out && ./skynet ../../tools/redis-diff/lib-config.lua
-- The two paths below are what the runner script rewrites for a given run.
root = "./"
luaservice = root .. "service/?.lua;" .. root .. "../../tools/redis-diff/?.lua"
lualoader = root .. "lualib/loader.lua"
lua_path = root .. "lualib/?.lua;" .. root .. "lualib/?/init.lua"
lua_cpath = root .. "luaclib/?.so"
cpath = root .. "cservice/?.so"

thread = 2
logger = nil
harbor = 0

redisdiff_oplog = "../../tools/redis-diff/work/oplog.jsonl"
redisdiff_out = "../../tools/redis-diff/work/lib-results.jsonl"

start = "replay_lib"
bootstrap = "snlua bootstrap"
