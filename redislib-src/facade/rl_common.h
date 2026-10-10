/* rl_common.h -- skynetjit redislib: the facade object model and the RESP-style
 * reply helpers shared by every luaclib/redis module.
 *
 * This header is the Task 1 contract (docs/superpowers/plans/2026-10-10-redislib.md,
 * "公共约定：facade 对象模型"): the signatures below are what bitmap.c / geo.c /
 * hll.c / sortedset.c / dump.c are written against, and the implementation is
 * contributed by the first facade task (Task 2) as facade/rl_common.c. It is
 * deliberately not compiled by anything yet -- Task 1 has no facade .so target.
 *
 * An rl_object is a Redis value held entirely in memory by one Lua userdata:
 * there is no keyspace, no client and no server, so the kernel's robj stays the
 * minimal shim struct (redislib-src/kernel/redislib_port.h) and the sds / zset*
 * members below are what the kernel functions are handed through o->ptr.
 */

#ifndef REDISLIB_RL_COMMON_H
#define REDISLIB_RL_COMMON_H

#include <lua.h>
#include <lauxlib.h>

#include "redislib_port.h"

typedef enum { RL_BITMAP, RL_HLL, RL_ZSET, RL_GEO } rl_kind;

typedef struct rl_object {
    rl_kind kind;
    sds str;        /* bitmap / hll: the Redis string value itself. An HLL value
                     * carries its 16 byte "HYLL" header, exactly like a real
                     * Redis string object, so hllCount()/hllAdd() can be handed
                     * (struct hllhdr *)s->ptr directly. */
    zset *zs;       /* zset / geo. A GEO value IS a sorted set, like in Redis:
                     * the member is the member as the caller wrote it and the
                     * score is the 52 bit geohash (geohashAlign52Bits). */
    lua_State *L;   /* owning state, for __gc and for error messages */
} rl_object;

/* Argument check for the method tables: returns the rl_object at index idx or
 * raises a lua_error whose text is the same one Redis replies with (spec §2.4).
 * A kind mismatch is the "WRONGTYPE ..." case. */
rl_object *rl_check(lua_State *L, int idx, rl_kind kind);

/* __gc entry: releases whatever the kind owns (sdsfree for str, the skiplist +
 * dict or listpack for zs) through the same allocator the kernel used. */
void rl_free_ud(lua_State *L, void *p);

/* RESP reply pushers, one per Redis reply shape (spec §3.2). They keep the
 * mapping table in one place so every command file reads the same way:
 *   rl_pushnilreply -> false        (Redis nil reply, "no such key/bit")
 *   rl_pushok       -> "OK"         (simple status string)
 *   rl_pushint      -> integer
 *   rl_pushbool     -> 1 / 0        (Redis "integer reply used as boolean")
 */
void rl_pushnilreply(lua_State *L);
void rl_pushok(lua_State *L);
void rl_pushint(lua_State *L, long long v);
void rl_pushbool(lua_State *L, int v);

/* Create the per-kind metatable ("redis.bitmap" & friends), install the
 * command methods and __gc, leaving the metatable on the stack for the
 * caller to attach anything else it needs. */
void rl_register_class(lua_State *L, rl_kind kind,
                       const luaL_Reg *methods, lua_CFunction gcfn);

/* Push a fresh zeroed rl_object userdata of the given kind with the class
 * metatable attached. */
void rl_newobject(lua_State *L, rl_kind kind);

#endif /* REDISLIB_RL_COMMON_H */
