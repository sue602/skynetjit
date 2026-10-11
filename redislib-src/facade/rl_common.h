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
} rl_object;

/* Raise the error reply Redis would have sent for this failure, as the text a
 * client actually sees (spec §3.2).
 *
 * addReplyErrorLength() (networking.c:657) is the whole rule: a message that
 * already starts with '-' carries its own error code ("-WRONGTYPE ...",
 * "-INVALIDOBJ ...") and is sent as it is, minus that marker; every other
 * message gets the generic code, so the client reads "ERR syntax error",
 * "ERR need numeric radius". Upstream's literals are kept verbatim in the
 * command files, including the leading '-', so this one function reproduces the
 * wire text for every command instead of each call site guessing.
 *
 * The raised message still gets LuaJIT's usual "file:line: " prefix, like any
 * other Lua error. Never returns. */
#if defined(__GNUC__)
__attribute__((format(printf, 2, 3)))
#endif
int rl_error(lua_State *L, const char *fmt, ...);

/* Argument check for the method tables: returns the rl_object at index idx or
 * raises a lua_error whose text is the same one Redis replies with (spec §2.4).
 * A kind mismatch is the "WRONGTYPE ..." case. */
rl_object *rl_check(lua_State *L, int idx, rl_kind kind);

/* ---- numeric arguments, shared by every facade --------------------------
 *
 * These mirror object.c's getDoubleFromObjectOrReply() /
 * getLong*FromObjectOrReply() pair, including which text each failure uses, so
 * a command can tell "not a number" from "out of range" the way Redis does.
 *
 * The _to forms do not raise (C_OK with the value, C_ERR otherwise) because
 * some commands must keep parsing -- GEOADD checks the decoded pair after the
 * numbers are read. The _check forms raise with the Redis text.
 *
 * Only a real LUA_TSTRING is handed to the kernel's parsers: lua_isstring() is
 * also true for numbers, and lua_tolstring() would rewrite the stack slot with
 * LuaJIT's %.14g rendering and cost a double its precision. string2d() refuses
 * NaN, so a NaN number argument is refused the same way -- which is also what
 * Redis' own scripting path does, since it turns the number into the string
 * "-nan" before parsing. */
int rl_todouble(lua_State *L, int idx, double *v);
int rl_tolong(lua_State *L, int idx, long long *v);
double rl_checkdouble(lua_State *L, int idx);   /* "value is not a valid float" */
long rl_checklong(lua_State *L, int idx);        /* object.c:1241-1267 texts    */

/* __gc entry: releases whatever the kind owns (sdsfree for str, the skiplist +
 * dict or listpack for zobj) through the same allocator the kernel used. */
void rl_free_ud(lua_State *L, void *p);

/* A member / lex-bound argument as the kernel wants it: a fresh sds the caller
 * frees. Numbers are accepted through Lua's own tostring, which is the string a
 * client would send for that value. */
sds rl_member(lua_State *L, int idx);

/* The same type check, without allocating: a command that must validate every
 * argument before it changes anything (ZADD / GEOADD's "fully or nothing at
 * all") checks the element types with this, and only then builds the vector it
 * cannot build without -- because this raise, unlike the ones above, happens
 * after memory has been taken. */
void rl_checkmember(lua_State *L, int idx);

/* A required command argument that a Lua caller did not pass at all, and one
 * that came with the wrong kind: a server answers the first from its command
 * table's arity check and the second from inside the command. There is no
 * command table here (NOTICE §6), so both texts come from these two helpers --
 * the arity line is Redis' own (server.c:2225), the kind line reuses
 * rl_checkmember's documented text. cmdname is the lower-case Redis command. */
void rl_checklist(lua_State *L, int idx, const char *cmdname);
const char *rl_argstring(lua_State *L, int idx, const char *cmdname);

/* Release one kernel zset object -- object.c's freeSortedSet() for the two
 * encodings a redislib zset ever reaches. rl_free_ud() calls it for the
 * RL_ZSET / RL_GEO arm, and a facade that swaps the payload of a live object
 * (ZRANGESTORE) calls it directly, without going through the __gc marking. */
void rl_free_zset(robj *zobj);

/* Make `fresh` the zset that `dst` holds, releasing the previous one. The store
 * commands (ZRANGESTORE, ZUNION/INTER/DIFFSTORE, GEOSEARCHSTORE, GEORADIUS
 * STORE) replace their destination outright, which is what setKey() does to a
 * key, and the object stays the same userdata so Lua references keep working.
 * `dst` must be RL_ZSET or RL_GEO; its kind is preserved. */
void rl_object_set_zset(rl_object *dst, robj *fresh);

/* The two create/convert entry points every sorted-set kind calls are declared
 * here instead of per facade, because three files call them and only the
 * linker would notice a signature that drifted. Upstream keeps them in
 * server.h, which is not part of this port; t_zset.c:1413 and :1428 define
 * them. */
robj *zsetTypeCreate(size_t size_hint, size_t val_len_hint);
void zsetTypeMaybeConvert(robj *zobj, size_t size_hint);

/* ---- the portable blob of the sorted-set family (spec §4) ---------------
 *
 * rl_zset_walk() calls `emit` for every member in ascending score order for
 * either encoding -- listpack entries and skiplist level-0 links are both kept
 * in score order -- which is what makes dump() byte-stable for a given set.
 *
 * rl_dump_zset() returns spec §4's blob as a fresh sds the caller frees:
 *   uint32 count, then count records of  float64 score, uint32 len, len bytes
 * every integer little-endian and written by hand, so a blob written here is
 * readable on any host and by any language (spec §4's "可跨语言解析").
 *
 * rl_load_zset() is the inverse and raises on anything that is not exactly that
 * shape -- truncation, a member length running past the end, trailing bytes, a
 * NaN score -- with upstream's own payload text, "Bad data format" (the reply
 * Redis gives for a serialized value it cannot parse, cluster.c:290). The
 * object is pre-sized with zsetTypeCreate(count), the same hint path a ZADD of
 * that many elements takes, and every record goes in through zsetAdd(), so a
 * loaded set is indistinguishable from one that was added to element by
 * element, encoding conversions included. */
void rl_zset_walk(robj *zobj, void (*emit)(void *arg, sds ele, double score),
                  void *arg);
sds rl_dump_zset(robj *zobj);
robj *rl_load_zset(lua_State *L, const char *buf, size_t len);

/* The argument of a load() function: a binary string, with no numeric coercion
 * (a number would be a different blob than the bytes the caller meant). */
const char *rl_checkblob(lua_State *L, int idx, size_t *len);

/* RESP reply pushers, one per Redis reply shape (spec §3.2). They keep the
 * mapping table in one place so every command file reads the same way:
 *   rl_pushnilreply -> false        (Redis nil reply, "no such key/bit")
 *   rl_pushok       -> "OK"         (simple status string)
 *   rl_pushint      -> integer
 *   rl_pushdouble   -> number       (Redis addReplyDouble; see below)
 */
void rl_pushnilreply(lua_State *L);
void rl_pushok(lua_State *L);
void rl_pushint(lua_State *L, long long v);

/* Push the Redis string value a bitmap / HLL object holds, as Lua bytes (this
 * is what spec §4's dump() is, and what a GET of such a key returns). An object
 * that has not been given a value yet pushes the empty string, which is what a
 * key set to "" reads as; sdslen() cannot be asked about NULL, so the length is
 * taken from the same test as the pointer. */
void rl_pushvalue(lua_State *L, rl_object *o);

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
