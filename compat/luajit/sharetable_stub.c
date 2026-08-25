#include <lauxlib.h>
#include <lua.h>

LUALIB_API int
luaopen_skynet_sharetable_core(lua_State *L) {
	return luaL_error(L,
		"skynet.sharetable is unavailable with the LuaJIT backend; "
		"it depends on private Lua 5.4/5.5 GC internals");
}
