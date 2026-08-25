#pragma once

#include <lauxlib.h>
#include <lua.h>
#include <lualib.h>

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <time.h>

#ifndef LUAMOD_API
#define LUAMOD_API LUALIB_API
#endif

static int
skynet_lua_absindex(lua_State *L, int index) {
	return index > 0 || index <= LUA_REGISTRYINDEX
		? index
		: lua_gettop(L) + index + 1;
}

#ifndef lua_absindex
#define lua_absindex skynet_lua_absindex
#endif

#ifndef lua_rawlen
#define lua_rawlen lua_objlen
#endif

static int
skynet_lua_getfield(lua_State *L, int index, const char *key) {
	(lua_getfield)(L, index, key);
	return lua_type(L, -1);
}

static int
skynet_lua_rawget(lua_State *L, int index) {
	(lua_rawget)(L, index);
	return lua_type(L, -1);
}

#define lua_getfield skynet_lua_getfield
#define lua_rawget skynet_lua_rawget

static int
skynet_lua_geti(lua_State *L, int index, lua_Integer key) {
	index = skynet_lua_absindex(L, index);
	lua_pushinteger(L, key);
	lua_gettable(L, index);
	return lua_type(L, -1);
}

static void
skynet_lua_seti(lua_State *L, int index, lua_Integer key) {
	index = skynet_lua_absindex(L, index);
	lua_pushinteger(L, key);
	lua_insert(L, -2);
	lua_settable(L, index);
}

#define lua_geti skynet_lua_geti
#define lua_seti skynet_lua_seti

static int
skynet_lua_rawgetp(lua_State *L, int index, const void *key) {
	index = skynet_lua_absindex(L, index);
	lua_pushlightuserdata(L, (void *)key);
	(lua_rawget)(L, index);
	return lua_type(L, -1);
}

static void
skynet_lua_rawsetp(lua_State *L, int index, const void *key) {
	index = skynet_lua_absindex(L, index);
	lua_pushlightuserdata(L, (void *)key);
	lua_insert(L, -2);
	lua_rawset(L, index);
}

#define lua_rawgetp skynet_lua_rawgetp
#define lua_rawsetp skynet_lua_rawsetp

static void *
skynet_lua_newuserdatauv(lua_State *L, size_t size, int uservalues) {
	void *result = lua_newuserdata(L, size);
	if (uservalues > 0) {
		lua_createtable(L, uservalues, 0);
		lua_setfenv(L, -2);
	}
	return result;
}

static int
skynet_lua_setiuservalue(lua_State *L, int index, int n) {
	index = skynet_lua_absindex(L, index);
	lua_getfenv(L, index);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_newtable(L);
		lua_pushvalue(L, -1);
		lua_setfenv(L, index);
	}
	lua_pushinteger(L, n);
	lua_pushvalue(L, -3);
	lua_rawset(L, -3);
	lua_pop(L, 1);
	lua_pop(L, 1);
	return 1;
}

static int
skynet_lua_getiuservalue(lua_State *L, int index, int n) {
	index = skynet_lua_absindex(L, index);
	lua_getfenv(L, index);
	if (!lua_istable(L, -1)) {
		lua_pop(L, 1);
		lua_pushnil(L);
		return LUA_TNIL;
	}
	lua_pushinteger(L, n);
	(lua_rawget)(L, -2);
	lua_remove(L, -2);
	return lua_type(L, -1);
}

#define lua_newuserdatauv skynet_lua_newuserdatauv
#define lua_setiuservalue skynet_lua_setiuservalue
#define lua_getiuservalue skynet_lua_getiuservalue

static int
skynet_lua_isinteger(lua_State *L, int index) {
	lua_Number number;
	lua_Integer integer;
	if (lua_type(L, index) != LUA_TNUMBER)
		return 0;
	number = lua_tonumber(L, index);
	integer = lua_tointeger(L, index);
	return (lua_Number)integer == number;
}

#define lua_isinteger skynet_lua_isinteger

static lua_State *
skynet_lua_newstate(lua_Alloc allocator, void *userdata, unsigned int seed) {
	(void)seed;
	return (lua_newstate)(allocator, userdata);
}

#define lua_newstate skynet_lua_newstate

static int
skynet_lua_resume(lua_State *L, lua_State *from, int nargs, int *nresults) {
	int status;
	(void)from;
	status = (lua_resume)(L, nargs);
	if (nresults)
		*nresults = lua_gettop(L);
	return status;
}

#define lua_resume skynet_lua_resume

#ifndef LUA_GCGEN
#define LUA_GCGEN 1000
#endif

static int
skynet_lua_gc(lua_State *L, int operation, ...) {
	va_list args;
	int data;
	if (operation == LUA_GCGEN)
		return 0;
	va_start(args, operation);
	data = va_arg(args, int);
	va_end(args);
	return (lua_gc)(L, operation, data);
}

#define lua_gc skynet_lua_gc

static int
skynet_lua_closethread(lua_State *L, lua_State *from) {
	(lua_resetthread)(L, from);
	return 0;
}

#define lua_closethread skynet_lua_closethread

static void
skynet_luaL_requiref(lua_State *L, const char *name, lua_CFunction openf,
		     int global) {
	lua_getfield(L, LUA_REGISTRYINDEX, "_LOADED");
	lua_getfield(L, -1, name);
	if (lua_isnil(L, -1)) {
		lua_pop(L, 1);
		lua_pushcfunction(L, openf);
		lua_pushstring(L, name);
		lua_call(L, 1, 1);
		lua_pushvalue(L, -1);
		lua_setfield(L, -3, name);
	}
	lua_remove(L, -2);
	if (global) {
		lua_pushvalue(L, -1);
		lua_setglobal(L, name);
	}
}

static const char *
skynet_luaL_tolstring(lua_State *L, int index, size_t *length) {
	index = skynet_lua_absindex(L, index);
	if (luaL_callmeta(L, index, "__tostring")) {
		if (!lua_isstring(L, -1))
			luaL_error(L, "'__tostring' must return a string");
	} else {
		switch (lua_type(L, index)) {
		case LUA_TNUMBER:
		case LUA_TSTRING:
			lua_pushvalue(L, index);
			break;
		case LUA_TBOOLEAN:
			lua_pushstring(L, lua_toboolean(L, index) ? "true" : "false");
			break;
		case LUA_TNIL:
			lua_pushliteral(L, "nil");
			break;
		default:
			lua_pushfstring(L, "%s: %p", luaL_typename(L, index),
					lua_topointer(L, index));
			break;
		}
	}
	return lua_tolstring(L, -1, length);
}

static unsigned int
skynet_luaL_makeseed(lua_State *L) {
	uintptr_t pointer = (uintptr_t)L;
	return (unsigned int)time(NULL) ^ (unsigned int)pointer ^
	       (unsigned int)(pointer >> 32);
}

#define luaL_requiref skynet_luaL_requiref
#define luaL_tolstring skynet_luaL_tolstring
#define luaL_makeseed skynet_luaL_makeseed
#define luaL_checkversion(L) ((void)(L))
#define luaL_buffinitsize(L, B, size) \
	((void)(size), luaL_buffinit((L), (B)), luaL_prepbuffer((B)))
