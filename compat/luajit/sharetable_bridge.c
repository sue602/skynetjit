#include <lauxlib.h>
#include <lua.h>

#include <stdint.h>
#include <string.h>

static int
pack_lightuserdata(lua_State *L) {
	uintptr_t value;
	if (!lua_islightuserdata(L, 1))
		return luaL_argerror(L, 1, "lightuserdata expected");
	value = (uintptr_t)lua_touserdata(L, 1);
	lua_pushlstring(L, (const char *)&value, sizeof(value));
	return 1;
}

static int
unpack_lightuserdata(lua_State *L) {
	uintptr_t value;
	size_t size;
	const char *data = luaL_checklstring(L, 1, &size);
	if (size != sizeof(value))
		return luaL_argerror(L, 1, "invalid pointer encoding");
	memcpy(&value, data, sizeof(value));
	lua_pushlightuserdata(L, (void *)value);
	return 1;
}

static int
pack_cfunction(lua_State *L) {
	lua_CFunction function;
	if (!lua_iscfunction(L, 1))
		return luaL_argerror(L, 1, "C function expected");
	function = lua_tocfunction(L, 1);
	if (function == NULL)
		return luaL_argerror(L, 1,
			"LuaJIT fast function has no reusable C entry point");
	lua_pushlstring(L, (const char *)&function, sizeof(function));
	return 1;
}

static int
unpack_cfunction(lua_State *L) {
	lua_CFunction function;
	size_t size;
	const char *data = luaL_checklstring(L, 1, &size);
	if (size != sizeof(function))
		return luaL_argerror(L, 1, "invalid C function encoding");
	memcpy(&function, data, sizeof(function));
	lua_pushcfunction(L, function);
	return 1;
}

LUALIB_API int
luaopen_skynet_sharetable_bridge(lua_State *L) {
	const luaL_Reg functions[] = {
		{ "pack_lightuserdata", pack_lightuserdata },
		{ "unpack_lightuserdata", unpack_lightuserdata },
		{ "pack_cfunction", pack_cfunction },
		{ "unpack_cfunction", unpack_cfunction },
		{ NULL, NULL },
	};
	luaL_newlib(L, functions);
	return 1;
}
