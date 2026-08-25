#include <lauxlib.h>
#include <lua.h>

LUALIB_API int
luaopen_skynet_sharetable_core(lua_State *L) {
	return luaL_error(L,
		"skynet.sharetable.core needs private Lua GC internals; "
		"require 'skynet.sharetable' to use the LuaJIT sharedata backend");
}
