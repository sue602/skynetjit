/* rl_common.c -- shared object model and RESP-style reply helpers for the
 * redislib facades. See rl_common.h for the contract. */

#include <string.h>
#include <stdlib.h>

#include "rl_common.h"

/* One metatable per kind, registered under the exact luaclib/redis/<name>
 * require path so debug tracebacks read naturally. */
static const char *const class_names[] = {
    "redis.bitmap",
    "redis.hll",
    "redis.zset",
    "redis.geo",
};

rl_object *rl_check(lua_State *L, int idx, rl_kind kind) {
    rl_object *o = (rl_object *)lua_touserdata(L, idx);
    if (!o || o->kind != kind) {
        /* Mirrors checkType() on a live server: the "key" here is the object
         * the method was called on. */
        luaL_error(L, "WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return o;
}

void rl_free_ud(lua_State *L, void *p) {
    rl_object *o = (rl_object *)p;
    (void)L;
    switch (o->kind) {
    case RL_BITMAP:
    case RL_HLL:
        if (o->str) sdsfree(o->str);
        o->str = NULL;
        break;
    case RL_ZSET:
    case RL_GEO:
        /* zset release is contributed with the sortedset facade (Task 4);
         * a GC can only run after an object was created, and creation for
         * these kinds comes with that task, so this arm is unreachable for
         * bitmap/hll modules built before it. */
        break;
    }
    o->kind = (rl_kind)-1;
}

void rl_pushnilreply(lua_State *L) {
    lua_pushboolean(L, 0);
}

void rl_pushok(lua_State *L) {
    lua_pushliteral(L, "OK");
}

void rl_pushint(lua_State *L, long long v) {
    lua_pushinteger(L, (lua_Integer)v);
}

void rl_pushbool(lua_State *L, int v) {
    lua_pushinteger(L, v ? 1 : 0);
}

static int rl_gc(lua_State *L) {
    rl_free_ud(L, lua_touserdata(L, 1));
    return 0;
}

/* Build the "redis.<name>" metatable in the registry: __index carries the
 * command methods (installed by the caller, after this returns the table is
 * left on the stack for the module to fill), __gc releases the object. */
void rl_register_class(lua_State *L, rl_kind kind,
                       const luaL_Reg *methods, lua_CFunction gcfn)
{
    (void)gcfn; /* rl_gc below is the single release path for every kind */
    luaL_newmetatable(L, class_names[kind]);
    lua_pushvalue(L, -1);
    lua_setfield(L, -2, "__index");
    lua_pushcfunction(L, rl_gc);
    lua_setfield(L, -2, "__gc");
    luaL_setfuncs(L, methods, 0);
}

void rl_newobject(lua_State *L, rl_kind kind) {
    rl_object *o = (rl_object *)lua_newuserdata(L, sizeof(*o));
    memset(o, 0, sizeof(*o));
    o->kind = kind;
    o->L = L;
    luaL_getmetatable(L, class_names[kind]);
    lua_setmetatable(L, -2);
}
