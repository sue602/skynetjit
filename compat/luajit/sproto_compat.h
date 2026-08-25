#pragma once

#include <lauxlib.h>
#include <lua.h>

#ifndef LUAMOD_API
#define LUAMOD_API LUALIB_API
#endif
#ifndef luai_unlikely
#define luai_unlikely(value) __builtin_expect(!!(value), 0)
#endif

/*
 * lsproto.c has a Lua 5.1 fallback with this name. OpenResty LuaJIT keeps
 * LUA_VERSION_NUM at 501 but already exports lua_tointegerx, so rename only
 * the local fallback after lua.h has declared the real API.
 */
#define lua_tointegerx skynet_sproto_tointegerx
