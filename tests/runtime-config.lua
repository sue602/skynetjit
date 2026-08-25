root = "./"
luaservice = root .. "service/?.lua;" .. root .. "../../tests/?.lua"
lualoader = root .. "lualib/loader.lua"
lua_path = root .. "lualib/?.lua;" .. root .. "lualib/?/init.lua"
lua_cpath = root .. "luaclib/?.so"
cpath = root .. "cservice/?.so"

thread = 2
logger = nil
harbor = 0
start = "runtime_smoke"
bootstrap = "snlua bootstrap"
