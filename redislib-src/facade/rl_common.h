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
    robj *zobj;     /* zset / geo: the kernel's own object view (encoding +
                     * payload), created through zsetTypeCreate()/
                     * createZsetObject()/createZsetListpackObject() and freed by
                     * rl_free_ud(). Every t_zset.c entry point takes a robj*,
                     * and the payload is a zset* only for the skiplist encoding
                     * and a listpack for the other one, so the facade cannot
                     * hold a zset* here -- Task 4 amended this member (it
                     * replaced the earlier `zset *zs`).
                     * A GEO value IS a sorted set, like in Redis: the member is
                     * the member as the caller wrote it and the score is the 52
                     * bit geohash (geohashAlign52Bits). */
    lua_State *L;   /* owning state, for __gc and for error messages */
} rl_object;

/* Argument check for the method tables: returns the rl_object at index idx or
 * raises a lua_error whose text is the same one Redis replies with (spec §2.4).
 * A kind mismatch is the "WRONGTYPE ..." case. */
rl_object *rl_check(lua_State *L, int idx, rl_kind kind);

/* __gc entry: releases whatever the kind owns (sdsfree for str, the skiplist +
 * dict or listpack for zobj) through the same allocator the kernel used. */
void rl_free_ud(lua_State *L, void *p);

/* Release one kernel zset object -- object.c's freeSortedSet() for the two
 * encodings a redislib zset ever reaches. rl_free_ud() calls it for the
 * RL_ZSET / RL_GEO arm, and a facade that swaps the payload of a live object
 * (ZRANGESTORE) calls it directly, without going through the __gc marking. */
void rl_free_zset(robj *zobj);

/* RESP reply pushers, one per Redis reply shape (spec §3.2). They keep the
 * mapping table in one place so every command file reads the same way:
 *   rl_pushnilreply -> false        (Redis nil reply, "no such key/bit")
 *   rl_pushok       -> "OK"         (simple status string)
 *   rl_pushint      -> integer
 *   rl_pushdouble   -> number       (Redis addReplyDouble; see below)
 *   rl_pushbool     -> 1 / 0        (Redis "integer reply used as boolean")
 */
void rl_pushnilreply(lua_State *L);
void rl_pushok(lua_State *L);
void rl_pushint(lua_State *L, long long v);
void rl_pushbool(lua_State *L, int v);

/* addReplyDouble() puts the human-readable form of the double into a bulk
 * string, because that is what a wire protocol needs. Scores are numbers on
 * both sides of this API (ZADD takes them as numbers), and a Lua number IS the
 * double, so the reply is pushed as a number: no formatting round trip, no
 * precision loss, no tonumber() at every call site. Spec §3.1's "Lua native
 * types only" is what this choice follows; tools/redis-diff (Task 10) compares
 * the two sides numerically. */
void rl_pushdouble(lua_State *L, double v);

/* Create the per-kind metatable ("redis.bitmap" & friends), install the
 * command methods and __gc, leaving the metatable on the stack for the
 * caller to attach anything else it needs. */
void rl_register_class(lua_State *L, rl_kind kind,
                       const luaL_Reg *methods, lua_CFunction gcfn);

/* Push a fresh zeroed rl_object userdata of the given kind with the class
 * metatable attached. */
void rl_newobject(lua_State *L, rl_kind kind);

#endif /* REDISLIB_RL_COMMON_H */
