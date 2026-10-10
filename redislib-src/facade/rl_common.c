/* rl_common.c -- shared object model and RESP-style reply helpers for the
 * redislib facades. See rl_common.h for the contract. */

#include <string.h>
#include <stdlib.h>
#include <stdarg.h>
#include <math.h>
#include <limits.h>

#include "rl_common.h"

/* One metatable per kind, registered under the exact luaclib/redis/<name>
 * require path so debug tracebacks read naturally. */
static const char *const class_names[] = {
    "redis.bitmap",
    "redis.hll",
    "redis.zset",
    "redis.geo",
};

int rl_error(lua_State *L, const char *fmt, ...) {
    va_list ap;
    sds msg;

    /* sdscatvprintf() is what addReplyErrorFormatInternal() uses, so a format
     * string renders here exactly as it does in the server. */
    va_start(ap, fmt);
    msg = sdscatvprintf(sdsempty(), fmt, ap);
    va_end(ap);

    if (msg[0] == '-') lua_pushstring(L, msg + 1);
    else lua_pushfstring(L, "ERR %s", msg);
    sdsfree(msg);

    luaL_where(L, 1);
    lua_insert(L, -2);
    lua_concat(L, 2);
    return lua_error(L);
}

rl_object *rl_check(lua_State *L, int idx, rl_kind kind) {
    rl_object *o = (rl_object *)lua_touserdata(L, idx);
    if (!o || o->kind != kind) {
        /* Mirrors checkType() on a live server: the "key" here is the object
         * the method was called on. */
        rl_error(L, "-WRONGTYPE Operation against a key holding the wrong kind of value");
    }
    return o;
}

int rl_todouble(lua_State *L, int idx, double *v) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        *v = lua_tonumber(L, idx);
        if (isnan(*v)) return C_ERR;
        return C_OK;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        const char *s = lua_tostring(L, idx);

        if (!string2d(s, lua_objlen(L, idx), v) || isnan(*v)) return C_ERR;
        return C_OK;
    }
    return C_ERR;
}

double rl_checkdouble(lua_State *L, int idx) {
    double v;

    if (rl_todouble(L, idx, &v) != C_OK)
        rl_error(L, "value is not a valid float");
    return v;
}

int rl_tolong(lua_State *L, int idx, long long *out) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        double d = lua_tonumber(L, idx);
        long long v;

        if (!isfinite(d)) return C_ERR;
        v = (long long)d;
        if ((double)v != d) return C_ERR;   /* not an integer, or past the range */
        *out = v;
        return C_OK;
    }
    if (lua_type(L, idx) == LUA_TSTRING)
        return string2ll(lua_tostring(L, idx), lua_objlen(L, idx), out) ?
            C_OK : C_ERR;
    return C_ERR;
}

/* getLongFromObjectOrReply() (object.c:1255): the shared failure text comes
 * from getLongLongFromObjectOrReply(), the narrowing one is its own. */
long rl_checklong(lua_State *L, int idx) {
    long long v;

    if (rl_tolong(L, idx, &v) != C_OK)
        rl_error(L, "value is not an integer or out of range");
    if (v < LONG_MIN || v > LONG_MAX)
        rl_error(L, "value is out of range");
    return (long)v;
}

void rl_free_zset(robj *zobj) {
    /* object.c's freeSortedSet() for the two encodings a redislib zset reaches.
     * The skiplist owns the element strings, the dict does not; a listpack
     * zset has no dict at all. */
    if (zobj == NULL) return;
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        lpFree(zobj->ptr);
    } else {
        zset *zs = zobj->ptr;
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
    }
    zfree(zobj);
}

void rl_checkmember(lua_State *L, int idx) {
    if (!lua_isstring(L, idx))
        rl_error(L, "string or number expected, got %s",
                 lua_typename(L, lua_type(L, idx)));
}

sds rl_member(lua_State *L, int idx) {
    const char *s;
    size_t len;

    rl_checkmember(L, idx);
    s = lua_tolstring(L, idx, &len);
    return sdsnewlen(s, len);
}

void rl_object_set_zset(rl_object *dst, robj *fresh) {
    rl_free_zset(dst->zobj);
    dst->zobj = fresh;
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
        rl_free_zset(o->zobj);
        o->zobj = NULL;
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

void rl_pushdouble(lua_State *L, double v) {
    lua_pushnumber(L, (lua_Number)v);
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
