/* sortedset.c -- the redis.zset facade: Redis 8.10.0's sorted-set commands over
 * an in-memory zset object (listpack or skiplist encoded, exactly like a real
 * Redis zset).
 *
 * Faithful transcription of Redis 8.10.0 src/t_zset.c's command layer with the
 * client / keyspace layer removed: the key lookup becomes the rl_object's own
 * robj, addReply* becomes the rl_push* mapping (see rl_common.h), and every
 * algorithmic step is a call into the ported kernel (zsetAdd / zsetDel /
 * zsetScore / zsetRank / zslNthInRange / zslNthInLexRange / zslGetElementByRank
 * / zzl* / zsetTypeCreate / zsetConvert / the zui* set-op iterator) rather than
 * a rewrite. Error texts are the upstream strings.
 *
 * What the port could not inherit: the range helpers the commands used to call
 * are file-local to t_zset.c (zslParseRange and the zsl/zzlDeleteRangeBy*
 * family). Rather than duplicate them -- they reach into statics like
 * zslDeleteNode and zzlDelete -- the facade parses bounds the way zslParseRange
 * does (fast_float_strtod + the same NaN/trailing-character rejections) and
 * deletes ranges by walking the range with the exported iterators and calling
 * zsetDel() per member. rl: notes mark both, and the semantic result, reply
 * shape and error text are upstream's.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>
#include <float.h>
#include <limits.h>

#include "rl_common.h"
#include "fast_float_strtod.h"

/* Kernel entry points not covered by facade-side t_zset.h (upstream declares
 * them in server.h, which is not part of this port). Same rule NOTICE §4
 * records for bitmap.c and hll.c: declare what you call, next to the kernel
 * file you mirror. */
robj *zsetTypeCreate(size_t size_hint, size_t val_len_hint);
void zsetTypeMaybeConvert(robj *zobj, size_t size_hint);
zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank);
void zsetTypeRandomElement(robj *zsetobj, unsigned long zsetsize,
                           listpackEntry *key, double *score);
sds zsetSdsFromListpackEntry(listpackEntry *e);

/* The zsetopsrc / zsetopval iterator types, the score-aggregation helpers and
 * the zui* declarations, copied/declared together in one header (see its banner
 * and NOTICE §3.1). */
#include "zset_int.h"

/* t_zset.c:1963, the reply when a score ends up not representable. */
static const char *nanerr = "resulting score is not a number (NaN)";

/* ---- argument helpers --------------------------------------------------- */

/* getDoubleFromObject()'s parse (object.c:1141), non-raising: C_OK with the
 * value in *v, or C_ERR. A Lua number is already the double, but string2d()
 * refuses NaN, so a NaN number argument is refused the same way: that is also
 * exactly what Redis' own scripting path does, since it turns the number into
 * the string "-nan" before parsing.
 *
 * Only a real LUA_TSTRING is read through string2d(): lua_isstring() is true
 * for numbers as well, and lua_tolstring() would rewrite the stack slot with
 * LuaJIT's %.14g rendering and cost a double its precision. */
static int
rl_todouble(lua_State *L, int idx, double *v) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        *v = lua_tonumber(L, idx);
        if (isnan(*v)) return C_ERR;
        return C_OK;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        size_t len;
        const char *s = lua_tostring(L, idx);

        if (!string2d(s, len = lua_objlen(L, idx), v) || isnan(*v)) return C_ERR;
        return C_OK;
    }
    return C_ERR;
}

/* getDoubleFromObjectOrReply(c,o,target,NULL) -- object.c:1156. */
static double
rl_checkdouble(lua_State *L, int idx) {
    double v;

    if (rl_todouble(L, idx, &v) != C_OK)
        luaL_error(L, "value is not a valid float");
    return v;
}

/* getLongLongFromObject()'s parse (object.c, non-raising like rl_todouble):
 * C_OK with the value in *out or C_ERR. */
static int
rl_tolong(lua_State *L, int idx, long long *out) {
    if (lua_type(L, idx) == LUA_TNUMBER) {
        double d = lua_tonumber(L, idx);
        long long v;

        if (!isfinite(d)) return C_ERR;
        v = (long long)d;
        if ((double)v != d) return C_ERR;   /* not an integer, or beyond the range */
        *out = v;
        return C_OK;
    }
    if (lua_type(L, idx) == LUA_TSTRING)
        return string2ll(lua_tostring(L, idx), lua_objlen(L, idx), out) ?
            C_OK : C_ERR;
    return C_ERR;
}

/* getLongFromObjectOrReply() (object.c:1255), including its two error texts:
 * the shared one from getLongLongFromObjectOrReply() and the narrowing one. */
static long
rl_checklong(lua_State *L, int idx) {
    long long v;

    if (rl_tolong(L, idx, &v) != C_OK)
        luaL_error(L, "value is not an integer or out of range");
    if (v < LONG_MIN || v > LONG_MAX)
        luaL_error(L, "value is out of range");
    return (long)v;
}

/* One ZRANGEBYSCORE-style bound: "(5" is exclusive, and the number itself is
 * parsed by fast_float_strtod() with zslParseRange()'s rejections (kernel
 * t_zset.c:714-750) -- note that is a different parser than the score one: it
 * takes "1e400" as +inf where string2d() would refuse it.
 * Returns C_OK/C_ERR; the caller replies "min or max is not a float". */
static int
rl_rangebound(lua_State *L, int idx, double *out, int *ex) {
    char *eptr;

    *ex = 0;
    if (lua_type(L, idx) == LUA_TNUMBER) {
        *out = lua_tonumber(L, idx);
        if (isnan(*out)) return C_ERR;
        return C_OK;
    }
    if (lua_type(L, idx) == LUA_TSTRING) {
        size_t len = lua_objlen(L, idx);
        const char *s = lua_tostring(L, idx);

        if (len > 0 && s[0] == '(') {
            *out = fast_float_strtod(s+1, len-1, &eptr);
            if (eptr[0] != '\0' || isnan(*out)) return C_ERR;
            *ex = 1;
            return C_OK;
        }
        *out = fast_float_strtod(s, len, &eptr);
        if (eptr[0] != '\0' || isnan(*out)) return C_ERR;
        return C_OK;
    }
    return C_ERR;
}

static int
rl_checkrangespec(lua_State *L, int imin, int imax, zrangespec *spec) {
    if (rl_rangebound(L, imin, &spec->min, &spec->minex) == C_ERR ||
        rl_rangebound(L, imax, &spec->max, &spec->maxex) == C_ERR)
        return C_ERR;
    return C_OK;
}

/* A member as the kernel wants it: an sds. zsetAdd()/zsetDel()/zsetScore()
 * copy what they keep (the skiplist owns its own strings), so the facade owns
 * this one and frees it. */
static sds
rl_member(lua_State *L, int idx) {
    size_t len;
    const char *s;

    if (!lua_isstring(L, idx))
        luaL_error(L, "string or number expected, got %s",
                   lua_typename(L, lua_type(L, idx)));
    s = lua_tolstring(L, idx, &len);
    return sdsnewlen(s, len);
}

/* The zset payload of the object at idx. */
static robj *
rl_zset(lua_State *L, int idx) {
    return rl_check(L, idx, RL_ZSET)->zobj;
}

/* ---- object lifecycle --------------------------------------------------- */

/* sortedset.new() => an empty zset, the same object a first ZADD creates
 * (zsetTypeCreate() picks listpack for a small set, like Redis). */
static int
lzset_new(lua_State *L) {
    rl_object *o;

    rl_newobject(L, RL_ZSET);
    o = (rl_object*)lua_touserdata(L, -1);
    o->zobj = zsetTypeCreate(0, 0);
    return 1;
}

static int
lzset_free(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_ZSET);
    rl_free_ud(L, o);
    return 0;
}

/* ---- ZADD / ZINCRBY ----------------------------------------------------- */

/* ZADD key [NX|XX] [GT|LT] [CH] [INCR] score member [score member ...]
 * ZINCRBY key delta member                                   (zaddGenericCommand,
 * t_zset.c:1962)                                             and :2103) */
static int
lzadd_generic(lua_State *L, int flags) {
    rl_object *o = rl_check(L, 1, RL_ZSET);
    robj *zobj = o->zobj;
    int ch = 0, scoreidx = 2;
    int added = 0, updated = 0, processed = 0;
    int incr, nx, xx, gt, lt;
    int elements, j;
    double *scores = NULL;
    double score = 0;

    /* Option words are strings only: a number is already the start of the
     * score-member pairs (and lua_isstring() would answer true for it, whose
     * lua_tostring() side effect costs the double its precision). */
    while (lua_type(L, scoreidx) == LUA_TSTRING) {
        const char *opt = lua_tostring(L, scoreidx);
        if      (!strcasecmp(opt,"nx")) flags |= ZADD_IN_NX;
        else if (!strcasecmp(opt,"xx")) flags |= ZADD_IN_XX;
        else if (!strcasecmp(opt,"ch")) ch = 1;
        else if (!strcasecmp(opt,"incr")) flags |= ZADD_IN_INCR;
        else if (!strcasecmp(opt,"gt")) flags |= ZADD_IN_GT;
        else if (!strcasecmp(opt,"lt")) flags |= ZADD_IN_LT;
        else break;
        scoreidx++;
    }

    incr = (flags & ZADD_IN_INCR) != 0;
    nx   = (flags & ZADD_IN_NX) != 0;
    xx   = (flags & ZADD_IN_XX) != 0;
    gt   = (flags & ZADD_IN_GT) != 0;
    lt   = (flags & ZADD_IN_LT) != 0;

    elements = lua_gettop(L) - scoreidx + 1;
    if (elements % 2 || !elements) return luaL_error(L, "syntax error");
    elements /= 2;

    if (nx && xx)
        return luaL_error(L,
            "XX and NX options at the same time are not compatible");
    if ((gt && nx) || (lt && nx) || (gt && lt))
        return luaL_error(L,
            "GT, LT, and/or NX options at the same time are not compatible");
    if (incr && elements > 1)
        return luaL_error(L,
            "INCR option supports a single increment-element pair");

    /* Start parsing all the scores, we need to emit any syntax error
     * before executing additions to the sorted set, as the command should
     * either execute fully or nothing at all. */
    scores = zmalloc(sizeof(double)*elements);
    for (j = 0; j < elements; j++) {
        if (rl_todouble(L, scoreidx+j*2, &scores[j]) != C_OK) {
            zfree(scores);
            return luaL_error(L, "value is not a valid float");
        }
    }

    /* rl: the key always exists here, so Redis's "no key + XX option: nothing
     * to do" short circuit has no input that can reach it; adding to an empty
     * set with XX is a no-op anyway and falls out of the loop below. */
    zsetTypeMaybeConvert(zobj, elements);

    for (j = 0; j < elements; j++) {
        double newscore;
        int retflags = 0;
        sds ele = rl_member(L, scoreidx+1+j*2);
        int retval;

        score = scores[j];
        retval = zsetAdd(zobj, score, ele, flags, &retflags, &newscore);
        sdsfree(ele);
        if (retval == 0) {
            /* rl: like upstream the elements already added stay added. */
            zfree(scores);
            luaL_error(L, "%s", nanerr);
        }
        if (retflags & ZADD_OUT_ADDED) added++;
        if (retflags & ZADD_OUT_UPDATED) updated++;
        if (!(retflags & ZADD_OUT_NOP)) processed++;
        score = newscore;
    }
    zfree(scores);

    if (incr) { /* ZINCRBY or INCR option. */
        if (processed) rl_pushdouble(L, score);
        else rl_pushnilreply(L);
    } else {    /* ZADD. */
        rl_pushint(L, ch ? added+updated : added);
    }
    return 1;
}

static int
lzadd(lua_State *L) {
    return lzadd_generic(L, ZADD_IN_NONE);
}

static int
lzincrby(lua_State *L) {
    /* ZINCRBY key delta member */
    return lzadd_generic(L, ZADD_IN_INCR);
}

/* ---- ZREM --------------------------------------------------------------- */

/* ZREM key member [member ...] => :n                                (t_zset.c:2107) */
static int
lzrem(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_ZSET);
    robj *zobj = o->zobj;
    int deleted = 0, j;
    int top = lua_gettop(L);

    if (zobj->encoding == OBJ_ENCODING_SKIPLIST)
        dictPauseAutoResize(((zset*)zobj->ptr)->dict);

    for (j = 2; j <= top; j++) {
        sds ele = rl_member(L, j);
        if (zsetDel(zobj, ele)) deleted++;
        sdsfree(ele);
        /* rl: Redis deletes the key when the set empties; the object stays, and
         * the further deletions below are no-ops either way. */
        if (zsetLength(zobj) == 0) break;
    }

    if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        dictResumeAutoResize(((zset*)zobj->ptr)->dict);
        dictShrinkIfNeeded(((zset*)zobj->ptr)->dict);
    }
    rl_pushint(L, deleted);
    return 1;
}

/* ---- range deletion (ZREMRANGEBYRANK / BYSCORE / BYLEX) ----------------- */

/* t_zset.c:2153 zrange_type -- shared by ZREMRANGEBY* and the ZRANGE family. */
typedef enum {
    RL_ZRANGE_AUTO = 0,
    RL_ZRANGE_RANK,
    RL_ZRANGE_SCORE,
    RL_ZRANGE_LEX,
} rl_range_type;

/* Collect the members of [start,end] (rank), or of the score / lex range, in
 * ascending order. rl: upstream hands the range to the file-local
 * zsl/zzlDeleteRangeBy* helpers; the facade walks the range with the exported
 * iterators instead and deletes through zsetDel(), so the deleted count and the
 * surviving members are the same. A listpack zset never grows past
 * zset_max_listpack_entries without being converted, so the per-member delete
 * stays cheap there; on a skiplist it is a delete by dict lookup. */
static sds *
rl_collect_range(robj *zobj, rl_range_type rangetype,
                 zrangespec *range, zlexrangespec *lexrange,
                 long start, long end, unsigned long *out) {
    sds *members = NULL;
    unsigned long n = 0, cap = 0;

#define RL_COLLECT(ele) do { \
        if (n == cap) { cap = cap ? cap*2 : 16; members = zrealloc(members, sizeof(sds)*cap); } \
        members[n++] = (ele); \
    } while(0)

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        if (rangetype == RL_ZRANGE_RANK) {
            if (zsetLength(zobj) == 0) { *out = 0; return NULL; }
            eptr = lpSeek(zl, start*2);
        } else if (rangetype == RL_ZRANGE_SCORE) {
            eptr = zzlFirstInRange(zl, range);
        } else {
            eptr = zzlFirstInLexRange(zl, lexrange);
        }
        if (eptr == NULL) { *out = 0; return NULL; }

        sptr = lpNext(zl, eptr);
        while (eptr) {
            if (rangetype == RL_ZRANGE_RANK) {
                if (n > (unsigned long)(end-start)) break;
                RL_COLLECT(lpGetObject(eptr));
            } else if (rangetype == RL_ZRANGE_SCORE) {
                if (!zslValueLteMax(zzlGetScore(sptr), range)) break;
                RL_COLLECT(lpGetObject(eptr));
            } else {
                if (!zzlLexValueLteMax(eptr, lexrange)) break;
                RL_COLLECT(lpGetObject(eptr));
            }
            zzlNext(zl, &eptr, &sptr);
        }
    } else {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank = 0;

        if (rangetype == RL_ZRANGE_RANK) {
            if (zsetLength(zobj) == 0) { *out = 0; return NULL; }
            zn = zslGetElementByRank(zsl, (unsigned long)start + 1);
        } else if (rangetype == RL_ZRANGE_SCORE) {
            zn = zslNthInRange(zsl, range, 0, &rank);
        } else {
            zn = zslNthInLexRange(zsl, lexrange, 0, &rank);
        }
        if (zn == NULL) { *out = 0; return NULL; }

        while (zn) {
            if (rangetype == RL_ZRANGE_RANK) {
                if (n > (unsigned long)(end-start)) break;
                RL_COLLECT(sdsdup(zslGetNodeElement(zn)));
            } else if (rangetype == RL_ZRANGE_SCORE) {
                if (!zslValueLteMax(zn->score, range)) break;
                RL_COLLECT(sdsdup(zslGetNodeElement(zn)));
            } else {
                if (!zslLexValueLteMax(zslGetNodeElement(zn), lexrange)) break;
                RL_COLLECT(sdsdup(zslGetNodeElement(zn)));
            }
            zn = zn->level[0].forward;
        }
    }
    *out = n;
    return members;
}

static int
lzremrange_generic(lua_State *L, rl_range_type rangetype) {
    robj *zobj = rl_zset(L, 1);
    zrangespec range = {0, 0, 0, 0};
    zlexrangespec lexrange = {NULL, NULL, 0, 0};
    long start = 0, end = 0, llen;
    sds *members;
    unsigned long deleted = 0, j;

    /* Step 1: parse the range. */
    if (rangetype == RL_ZRANGE_RANK) {
        start = rl_checklong(L, 2);
        end   = rl_checklong(L, 3);
    } else if (rangetype == RL_ZRANGE_SCORE) {
        if (rl_checkrangespec(L, 2, 3, &range) != C_OK)
            return luaL_error(L, "min or max is not a float");
    } else {
        sds min = rl_member(L, 2), max = rl_member(L, 3);
        robj vmin, vmax;
        int rc;

        /* zslParseLexRange() takes the two bounds as string objects, so wrap
         * the two sds values in kernel views for the call. */
        vmin.type = OBJ_STRING; vmin.encoding = OBJ_ENCODING_RAW; vmin.ptr = min;
        vmax.type = OBJ_STRING; vmax.encoding = OBJ_ENCODING_RAW; vmax.ptr = max;
        rc = zslParseLexRange(&vmin, &vmax, &lexrange);
        sdsfree(min); sdsfree(max);
        if (rc != C_OK)
            return luaL_error(L, "min or max not valid string range item");
    }

    /* Step 2: range sanity checks for the rank form. */
    if (rangetype == RL_ZRANGE_RANK) {
        llen = (long)zsetLength(zobj);
        if (start < 0) start = llen+start;
        if (end < 0) end = llen+end;
        if (start < 0) start = 0;

        /* Invariant: start >= 0, so this test is true when end < 0.
         * The range is empty when start > end or start >= length. */
        if (start > end || start >= llen) { rl_pushint(L, 0); goto cleanup; }
        if (end >= llen) end = llen-1;
    }

    /* Step 3: perform the range deletion. */
    if (zobj->encoding == OBJ_ENCODING_SKIPLIST)
        dictPauseAutoResize(((zset*)zobj->ptr)->dict);

    members = rl_collect_range(zobj, rangetype, &range, &lexrange,
                               start, end, &deleted);
    for (j = 0; j < deleted; j++) {
        zsetDel(zobj, members[j]);
        sdsfree(members[j]);
    }
    if (members) zfree(members);

    if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        dictResumeAutoResize(((zset*)zobj->ptr)->dict);
        dictShrinkIfNeeded(((zset*)zobj->ptr)->dict);
    }

    rl_pushint(L, (long long)deleted);

cleanup:
    if (rangetype == RL_ZRANGE_LEX) zslFreeLexRange(&lexrange);
    return 1;
}

static int
lzremrangebyrank(lua_State *L) { return lzremrange_generic(L, RL_ZRANGE_RANK); }
static int
lzremrangebyscore(lua_State *L) { return lzremrange_generic(L, RL_ZRANGE_SCORE); }
static int
lzremrangebylex(lua_State *L) { return lzremrange_generic(L, RL_ZRANGE_LEX); }

/* ---- the range family --------------------------------------------------- */

/* ZRANGE / ZREVRANGE / ZRANGEBYSCORE / ZREVRANGEBYSCORE / ZRANGEBYLEX /
 * ZREVRANGEBYLEX / ZRANGESTORE all share zrangeGenericCommand()'s grammar
 * (t_zset.c:3930) and the three walkers behind it. Upstream routes the result
 * through a zrange_result_handler that either replies to a client or fills a
 * destination object; the facade keeps that split as a flag on one small
 * struct: `store` results go into a fresh zset, everything else goes into a
 * flat Lua table, where WITHSCORES interleaves member and score exactly like
 * the RESP2 array Redis sends (spec §3.2).
 *
 * Redis 8.10.0 has no BYRANK / BYINDEX / ZMIN-ZMAX filter words -- the option
 * set below is the whole grammar of this release. */
typedef enum { RL_RANGE_AUTO, RL_RANGE_FORWARD, RL_RANGE_REVERSE } rl_range_direction;

typedef struct {
    lua_State *L;
    int tab;                /* result table, client form */
    int n;                  /* next field index in it */
    int withscores;
    int store;              /* ZRANGESTORE */
    robj *dstobj;           /* the fresh destination, store form */
    size_t count;
} rl_zrange_out;

/* With WITHSCORES the RESP2 reply is one flat array of twice the elements, so
 * the table is sized for that (t_zset.c:3276 does the same for the reply). */
#define RL_RESULT_SIZE(out, length) \
    (((out)->withscores && (length) > 0) ? (length)*2 : ((length) > 0 ? (length) : 0))

static void
rl_zrange_begin(rl_zrange_out *out, long length) {
    if (out->store) {
        /* zrangeResultBeginStore(): t_zset.c:3331 */
        out->dstobj = zsetTypeCreate(length >= 0 ? (size_t)length : 0, 0);
        return;
    }
    lua_createtable(out->L, RL_RESULT_SIZE(out, length), 0);
    out->tab = lua_gettop(out->L);
    out->n = 1;
}

static void
rl_zrange_emit(rl_zrange_out *out, const void *p, size_t len, double score) {
    if (out->store) {
        /* zrangeResultEmitCBufferForStore(): t_zset.c:3336 */
        double newscore;
        int retflags = 0;
        sds ele = sdsnewlen(p, len);
        int retval = zsetAdd(out->dstobj, score, ele, ZADD_IN_NONE, &retflags, &newscore);

        sdsfree(ele);
        serverAssert(retval);
    } else {
        lua_pushlstring(out->L, (const char*)p, len);
        lua_rawseti(out->L, out->tab, out->n++);
        if (out->withscores) {
            rl_pushdouble(out->L, score);
            lua_rawseti(out->L, out->tab, out->n++);
        }
    }
    out->count++;
}

/* A listpack member as bytes: either an inline string or the decimal form of
 * an integer entry, which is what Redis replies (addReplyBulkLongLong). */
static void
rl_pushlp_member(lua_State *L, unsigned char *eptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    vstr = lpGetValue(eptr, &vlen, &vlong);
    if (vstr == NULL) {
        char buf[LONG_STR_SIZE];
        lua_pushlstring(L, buf, ll2string(buf, sizeof(buf), vlong));
    } else {
        lua_pushlstring(L, (const char*)vstr, vlen);
    }
}

/* Push the current listpack member (and, when asked, its score) into the
 * result. `score` is what the caller already read, or 0 when it was ignored. */
static void
rl_zrange_emit_lp(lua_State *L, rl_zrange_out *out, unsigned char *eptr,
                  double score) {
    if (out->store) {
        sds ele = lpGetObject(eptr);
        rl_zrange_emit(out, ele, sdslen(ele), score);
        sdsfree(ele);
        return;
    }
    rl_pushlp_member(L, eptr);
    lua_rawseti(L, out->tab, out->n++);
    if (out->withscores) {
        rl_pushdouble(L, score);
        lua_rawseti(L, out->tab, out->n++);
    }
    out->count++;
}

/* Replace the zset a live facade object holds, releasing the previous payload.
 * Deliberately not rl_free_ud(): that one also marks the userdata dead, and
 * here the object stays in use. */
static void
rl_zset_replace(rl_object *dst, robj *fresh) {
    rl_free_zset(dst->zobj);
    dst->zobj = fresh;
}

static void
rl_zrange_finalize(rl_zrange_out *out) {
    if (out->store) {
        robj *fresh = out->dstobj;

        if (!out->count) {
            /* zrangeResultFinalizeStore()'s empty branch: upstream releases the
             * throwaway destination and deletes the destination key. An empty
             * object is what that deletion means for a handle that stays alive,
             * so the destination is reset rather than left as it was. */
            rl_free_zset(fresh);
            fresh = zsetTypeCreate(0, 0);
        }
        rl_zset_replace((rl_object*)lua_touserdata(out->L, 1), fresh);
        rl_pushint(out->L, (long long)out->count);
        return;
    }

    /* The client form is done: the table on the stack is the reply, and a flat
     * WITHSCORES table was already filled that way. */
    lua_pushvalue(out->L, out->tab);
}

/* genericZrangebyrankCommand() -- t_zset.c:3413 */
static void
rl_zrange_byrank(rl_zrange_out *out, robj *zobj, long start, long end,
                 int reverse) {
    long llen, rangelen;
    lua_State *L = out->L;

    /* Sanitize indexes. */
    llen = (long)zsetLength(zobj);
    if (start < 0) start = llen+start;
    if (end < 0) end = llen+end;
    if (start < 0) start = 0;

    /* Invariant: start >= 0, so this test will be true when end < 0.
     * The range is empty when start > end or start >= length. */
    if (start > end || start >= llen) {
        rl_zrange_begin(out, 0);
        rl_zrange_finalize(out);
        return;
    }
    if (end >= llen) end = llen-1;
    rangelen = (end-start)+1;

    rl_zrange_begin(out, rangelen);

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        if (reverse) eptr = lpSeek(zl, -2-(2*start));
        else         eptr = lpSeek(zl, 2*start);

        sptr = lpNext(zl, eptr);
        while (rangelen--) {
            double score = 0.0;

            /* don't bother to extract the score if it's gonna be ignored. */
            if (out->withscores || out->store) score = zzlGetScore(sptr);
            rl_zrange_emit_lp(L, out, eptr, score);
            if (reverse) zzlPrev(zl, &eptr, &sptr);
            else         zzlNext(zl, &eptr, &sptr);
        }
    } else {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        /* Check if starting point is trivial, before doing log(N) lookup. */
        if (reverse) {
            ln = zsl->tail;
            if (start > 0) ln = zslGetElementByRank(zsl, (unsigned long)(llen-start));
        } else {
            ln = zsl->header->level[0].forward;
            if (start > 0) ln = zslGetElementByRank(zsl, (unsigned long)start+1);
        }

        while (rangelen--) {
            sds ele = zslGetNodeElement(ln);

            rl_zrange_emit(out, ele, sdslen(ele), ln->score);
            ln = reverse ? ln->backward : ln->level[0].forward;
        }
    }
    rl_zrange_finalize(out);
}

/* genericZrangebyscoreCommand() -- t_zset.c:3528 */
static void
rl_zrange_byscore(rl_zrange_out *out, zrangespec *range, robj *zobj,
                  long offset, long limit, int reverse) {
    lua_State *L = out->L;

    rl_zrange_begin(out, -1);

    /* For invalid offset, return directly. */
    if (offset < 0 || (offset > 0 && offset >= (long)zsetLength(zobj))) {
        rl_zrange_begin(out, 0);
        rl_zrange_finalize(out);
        return;
    }

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        /* If reversed, get the last node in range as starting point. */
        eptr = reverse ? zzlLastInRange(zl, range) : zzlFirstInRange(zl, range);
        if (eptr) sptr = lpNext(zl, eptr);

        /* If there is an offset, just traverse the number of elements without
         * checking the score because that is done in the next loop. */
        while (eptr && offset--) {
            if (reverse) zzlPrev(zl, &eptr, &sptr);
            else         zzlNext(zl, &eptr, &sptr);
        }

        while (eptr && limit--) {
            double score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. */
            if (reverse) { if (!zslValueGteMin(score, range)) break; }
            else         { if (!zslValueLteMax(score, range)) break; }

            rl_zrange_emit_lp(L, out, eptr, score);
            if (reverse) zzlPrev(zl, &eptr, &sptr);
            else         zzlNext(zl, &eptr, &sptr);
        }
    } else {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        /* If reversed, get the last node in range as starting point. */
        if (reverse) ln = zslNthInRange(zsl, range, -offset-1, NULL);
        else         ln = zslNthInRange(zsl, range, offset, NULL);

        while (ln && limit--) {
            if (reverse) { if (!zslValueGteMin(ln->score, range)) break; }
            else         { if (!zslValueLteMax(ln->score, range)) break; }

            {
                sds ele = zslGetNodeElement(ln);
                rl_zrange_emit(out, ele, sdslen(ele), ln->score);
            }
            ln = reverse ? ln->backward : ln->level[0].forward;
        }
    }
    rl_zrange_finalize(out);
}

/* genericZrangebylexCommand() -- t_zset.c:3799 */
static void
rl_zrange_bylex(rl_zrange_out *out, zlexrangespec *range, robj *zobj,
                long offset, long limit, int reverse) {
    lua_State *L = out->L;

    rl_zrange_begin(out, -1);

    /* For invalid offset, return directly. */
    if (offset < 0 || (offset > 0 && offset >= (long)zsetLength(zobj))) {
        rl_zrange_begin(out, 0);
        rl_zrange_finalize(out);
        return;
    }

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = reverse ? zzlLastInLexRange(zl, range) : zzlFirstInLexRange(zl, range);
        if (eptr) sptr = lpNext(zl, eptr);

        while (eptr && offset--) {
            if (reverse) zzlPrev(zl, &eptr, &sptr);
            else         zzlNext(zl, &eptr, &sptr);
        }

        while (eptr && limit--) {
            double score = 0;

            if (out->withscores || out->store) score = zzlGetScore(sptr);

            /* Abort when the node is no longer in range. */
            if (reverse) { if (!zzlLexValueGteMin(eptr, range)) break; }
            else         { if (!zzlLexValueLteMax(eptr, range)) break; }

            rl_zrange_emit_lp(L, out, eptr, score);
            if (reverse) zzlPrev(zl, &eptr, &sptr);
            else         zzlNext(zl, &eptr, &sptr);
        }
    } else {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        if (reverse) ln = zslNthInLexRange(zsl, range, -offset-1, NULL);
        else         ln = zslNthInLexRange(zsl, range, offset, NULL);

        while (ln && limit--) {
            sds ele = zslGetNodeElement(ln);

            if (reverse) { if (!zslLexValueGteMin(ele, range)) break; }
            else         { if (!zslLexValueLteMax(ele, range)) break; }

            rl_zrange_emit(out, ele, sdslen(ele), ln->score);
            ln = reverse ? ln->backward : ln->level[0].forward;
        }
    }
    rl_zrange_finalize(out);
}

/* zrangeGenericCommand() -- t_zset.c:3930.
 *
 * `keyidx` is the source object argument (1 for the read commands, 2 for
 * ZRANGESTORE, whose destination is argument 1 and which has no WITHSCORES). */
static int
rl_zrange_generic(lua_State *L, int store, rl_range_type rangetype,
                  rl_range_direction direction) {
    int keyidx = store ? 2 : 1;
    robj *zobj = rl_zset(L, keyidx);
    int minidx = keyidx + 1, maxidx = keyidx + 2;
    zrangespec range = {0, 0, 0, 0};
    zlexrangespec lexrange = {NULL, NULL, 0, 0};
    sds lexmin = NULL, lexmax = NULL;
    long opt_start = 0, opt_end = 0, opt_offset = 0, opt_limit = -1;
    int opt_withscores = 0;
    rl_zrange_out out;
    int j, rc;

    memset(&out, 0, sizeof(out));
    out.L = L;
    out.store = store;

    /* Step 1: skip <src> <min> <max> and parse the remaining options. */
    for (j = keyidx + 3; j <= lua_gettop(L); j++) {
        long leftargs = lua_gettop(L) - j;
        if (lua_type(L, j) != LUA_TSTRING) return luaL_error(L, "syntax error");
        const char *opt = lua_tostring(L, j);

        if (!store && !strcasecmp(opt, "withscores")) {
            opt_withscores = 1;
        } else if (!strcasecmp(opt, "limit") && leftargs >= 2) {
            opt_offset = rl_checklong(L, j+1);
            opt_limit  = rl_checklong(L, j+2);
            j += 2;
        } else if (direction == RL_RANGE_AUTO && !strcasecmp(opt, "rev")) {
            direction = RL_RANGE_REVERSE;
        } else if (rangetype == RL_ZRANGE_AUTO && !strcasecmp(opt, "bylex")) {
            rangetype = RL_ZRANGE_LEX;
        } else if (rangetype == RL_ZRANGE_AUTO && !strcasecmp(opt, "byscore")) {
            rangetype = RL_ZRANGE_SCORE;
        } else {
            return luaL_error(L, "syntax error");
        }
    }

    /* Use defaults if not overridden by arguments. */
    if (direction == RL_RANGE_AUTO) direction = RL_RANGE_FORWARD;
    if (rangetype == RL_ZRANGE_AUTO) rangetype = RL_ZRANGE_RANK;

    /* Check for conflicting arguments. */
    if (opt_limit != -1 && rangetype == RL_ZRANGE_RANK)
        return luaL_error(L,
            "syntax error, LIMIT is only supported in combination with either BYSCORE or BYLEX");
    if (opt_withscores && rangetype == RL_ZRANGE_LEX)
        return luaL_error(L,
            "syntax error, WITHSCORES not supported in combination with BYLEX");

    if (direction == RL_RANGE_REVERSE &&
        (rangetype == RL_ZRANGE_SCORE || rangetype == RL_ZRANGE_LEX)) {
        /* Range is given as [max,min] */
        int tmp = maxidx; maxidx = minidx; minidx = tmp;
    }

    /* Step 2: parse the range. */
    if (rangetype == RL_ZRANGE_RANK) {
        opt_start = rl_checklong(L, minidx);
        opt_end   = rl_checklong(L, maxidx);
    } else if (rangetype == RL_ZRANGE_SCORE) {
        if (rl_checkrangespec(L, minidx, maxidx, &range) != C_OK)
            return luaL_error(L, "min or max is not a float");
    } else {
        robj vmin, vmax;

        lexmin = rl_member(L, minidx);
        lexmax = rl_member(L, maxidx);
        vmin.type = OBJ_STRING; vmin.encoding = OBJ_ENCODING_RAW; vmin.ptr = lexmin;
        vmax.type = OBJ_STRING; vmax.encoding = OBJ_ENCODING_RAW; vmax.ptr = lexmax;
        rc = zslParseLexRange(&vmin, &vmax, &lexrange);
        sdsfree(lexmin); sdsfree(lexmax);
        lexmin = lexmax = NULL;
        if (rc != C_OK)
            return luaL_error(L, "min or max not valid string range item");
    }

    out.withscores = opt_withscores || store;

    /* Step 3/4: walk the range into the result. */
    if (rangetype == RL_ZRANGE_RANK) {
        rl_zrange_byrank(&out, zobj, opt_start, opt_end,
                         direction == RL_RANGE_REVERSE);
    } else if (rangetype == RL_ZRANGE_SCORE) {
        rl_zrange_byscore(&out, &range, zobj, opt_offset, opt_limit,
                          direction == RL_RANGE_REVERSE);
    } else {
        rl_zrange_bylex(&out, &lexrange, zobj, opt_offset, opt_limit,
                        direction == RL_RANGE_REVERSE);
    }

    if (rangetype == RL_ZRANGE_LEX) zslFreeLexRange(&lexrange);
    return 1;
}

/* ZRANGE <min> <max> [BYSCORE|BYLEX] [REV] [WITHSCORES] [LIMIT offset count] */
static int
lzrange(lua_State *L) {
    return rl_zrange_generic(L, 0, RL_ZRANGE_AUTO, RL_RANGE_AUTO);
}

/* ZREVRANGE <start> <stop> [WITHSCORES] */
static int
lzrevrange(lua_State *L) {
    return rl_zrange_generic(L, 0, RL_ZRANGE_RANK, RL_RANGE_REVERSE);
}

/* ZRANGEBYSCORE <min> <max> [WITHSCORES] [LIMIT offset count] */
static int
lzrangebyscore(lua_State *L) {
    return rl_zrange_generic(L, 0, RL_ZRANGE_SCORE, RL_RANGE_FORWARD);
}

/* ZREVRANGEBYSCORE <max> <min> [WITHSCORES] [LIMIT offset count] */
static int
lzrevrangebyscore(lua_State *L) {
    return rl_zrange_generic(L, 0, RL_ZRANGE_SCORE, RL_RANGE_REVERSE);
}

/* ZRANGEBYLEX <min> <max> [LIMIT offset count] */
static int
lzrangebylex(lua_State *L) {
    return rl_zrange_generic(L, 0, RL_ZRANGE_LEX, RL_RANGE_FORWARD);
}

/* ZREVRANGEBYLEX <max> <min> [LIMIT offset count] */
static int
lzrevrangebylex(lua_State *L) {
    return rl_zrange_generic(L, 0, RL_ZRANGE_LEX, RL_RANGE_REVERSE);
}

/* ZRANGESTORE <dst> <src> <min> <max> [BYSCORE|BYLEX] [REV] [LIMIT ...]
 * The destination is argument 1 in both call shapes:
 * sortedset.zrangestore(dst, src, ...) and dst:zrangestore(src, ...). */
static int
lzrangestore(lua_State *L) {
    (void)rl_check(L, 1, RL_ZSET);
    return rl_zrange_generic(L, 1, RL_ZRANGE_AUTO, RL_RANGE_AUTO);
}

/* ---- ZCARD / ZSCORE / ZMSCORE / ZRANK / ZREVRANK ------------------------ */

static int
lzcard(lua_State *L) {
    rl_pushint(L, (long long)zsetLength(rl_zset(L, 1)));
    return 1;
}

/* ZSCORE key member => Bulk(score) or nil                        (t_zset.c:4092) */
static int
lzscore(lua_State *L) {
    robj *zobj = rl_zset(L, 1);
    sds ele = rl_member(L, 2);
    double score;
    int rc = zsetScore(zobj, ele, &score);

    sdsfree(ele);
    if (rc == C_ERR) rl_pushnilreply(L);
    else rl_pushdouble(L, score);
    return 1;
}

/* ZMSCORE key member [member ...] => array of score|nil          (t_zset.c:4112) */
static int
lzmscore(lua_State *L) {
    robj *zobj = rl_zset(L, 1);
    int top = lua_gettop(L), j;

    lua_createtable(L, top-2, 0);
    for (j = 2; j <= top; j++) {
        sds ele = rl_member(L, j);
        double score;
        int rc = zsetScore(zobj, ele, &score);

        sdsfree(ele);
        if (rc == C_ERR) rl_pushnilreply(L);
        else rl_pushdouble(L, score);
        lua_rawseti(L, -2, j-1);
    }
    return 1;
}

/* ZRANK / ZREVRANK key member [WITHSCORE]                        (t_zset.c:4134)
 * A rank is 0-based, like Redis replies it. */
static int
lrank_generic(lua_State *L, int reverse, const char *cmdname) {
    robj *zobj = rl_zset(L, 1);
    sds ele;
    long rank;
    double score;
    int opt_withscore = 0;
    int top = lua_gettop(L);

    if (top > 3)
        return luaL_error(L, "wrong number of arguments for '%s' command", cmdname);
    if (top == 3) {
        if (!strcasecmp(luaL_checkstring(L, 3), "withscore")) opt_withscore = 1;
        else return luaL_error(L, "syntax error");
    }

    ele = rl_member(L, 2);
    rank = zsetRank(zobj, ele, reverse, opt_withscore ? &score : NULL);
    sdsfree(ele);

    if (rank < 0) {
        /* rl: Redis replies a null array for the WITHSCORE form and a null bulk
         * otherwise; both are `false` here (spec §3.2). */
        rl_pushnilreply(L);
        return 1;
    }
    if (opt_withscore) {
        lua_createtable(L, 2, 0);
        rl_pushint(L, rank);
        lua_rawseti(L, -2, 1);
        rl_pushdouble(L, score);
        lua_rawseti(L, -2, 2);
        return 1;
    }
    rl_pushint(L, rank);
    return 1;
}

static int
lzrank(lua_State *L) { return lrank_generic(L, 0, "zrank"); }
static int
lzrevrank(lua_State *L) { return lrank_generic(L, 1, "zrevrank"); }

/* ---- ZCOUNT / ZLEXCOUNT ------------------------------------------------- */

/* ZCOUNT key min max                                             (t_zset.c:3646) */
static int
lzcount(lua_State *L) {
    robj *zobj = rl_zset(L, 1);
    zrangespec range;
    unsigned long count = 0;

    if (rl_checkrangespec(L, 2, 3, &range) != C_OK)
        return luaL_error(L, "min or max is not a float");

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = zzlFirstInRange(zl, &range);
        if (eptr == NULL) { rl_pushint(L, 0); return 1; }
        sptr = lpNext(zl, eptr);
        while (eptr) {
            if (!zslValueLteMax(zzlGetScore(sptr), &range)) break;
            count++;
            zzlNext(zl, &eptr, &sptr);
        }
    } else {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        /* Find first element in range and get its rank. */
        zn = zslNthInRange(zsl, &range, 0, &rank);
        if (zn != NULL) {
            count = (zsl->length - (rank - 1));
            /* Find last element in range (n = -1) and get its rank. */
            zn = zslNthInRange(zsl, &range, -1, &rank);
            if (zn != NULL) count -= (zsl->length - rank);
        }
    }
    rl_pushint(L, (long long)count);
    return 1;
}

/* ZLEXCOUNT key min max                                          (t_zset.c:3721) */
static int
lzlexcount(lua_State *L) {
    robj *zobj = rl_zset(L, 1);
    zlexrangespec range;
    unsigned long count = 0;
    sds min = rl_member(L, 2), max = rl_member(L, 3);
    robj vmin, vmax;

    vmin.type = OBJ_STRING; vmin.encoding = OBJ_ENCODING_RAW; vmin.ptr = min;
    vmax.type = OBJ_STRING; vmax.encoding = OBJ_ENCODING_RAW; vmax.ptr = max;
    if (zslParseLexRange(&vmin, &vmax, &range) != C_OK) {
        sdsfree(min); sdsfree(max);
        return luaL_error(L, "min or max not valid string range item");
    }
    sdsfree(min); sdsfree(max);

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = zzlFirstInLexRange(zl, &range);
        if (eptr == NULL) { zslFreeLexRange(&range); rl_pushint(L, 0); return 1; }
        sptr = lpNext(zl, eptr);
        while (eptr) {
            if (!zzlLexValueLteMax(eptr, &range)) break;
            count++;
            zzlNext(zl, &eptr, &sptr);
        }
    } else {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *zn;
        unsigned long rank;

        zn = zslNthInLexRange(zsl, &range, 0, &rank);
        if (zn != NULL) {
            count = (zsl->length - (rank - 1));
            zn = zslNthInLexRange(zsl, &range, -1, &rank);
            if (zn != NULL) count -= (zsl->length - rank);
        }
    }
    zslFreeLexRange(&range);
    rl_pushint(L, (long long)count);
    return 1;
}

/* ---- ZPOPMIN / ZPOPMAX / ZMPOP ------------------------------------------ */

#define RL_ZSET_MIN 0
#define RL_ZSET_MAX 1

/* One popped batch: the members (owned here) and their scores.
 *
 * genericZpopCommand() (t_zset.c:4227) pops and replies in the same loop; the
 * facade collects first and builds the reply after, because the reply shapes of
 * its two callers differ (a flat member/score array for ZPOPMIN/MAX, a
 * {source, pairs} pair for ZMPOP) and nothing can observe the difference while
 * popping. The `do { } while(--rangelen)` of upstream is a safe loop there only
 * because a Redis key never holds an empty zset; here the object may be empty,
 * so the count is clamped first. */
typedef struct {
    sds *eles;
    double *scores;
    long n;
} rl_pop_result;

static void
rl_pop_collect(robj *zobj, int where, long count, rl_pop_result *res) {
    long llen = (long)zsetLength(zobj);
    long rangelen = (count > llen) ? llen : count;

    /* Everything the free path touches is initialised here, before any early
     * return: an empty result must not hand the caller garbage pointers. */
    res->eles = NULL;
    res->scores = NULL;
    res->n = 0;
    if (rangelen <= 0) return;
    res->eles = zmalloc(sizeof(sds)*rangelen);
    res->scores = zmalloc(sizeof(double)*rangelen);

    while (rangelen--) {
        sds ele;
        double score;

        if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
            unsigned char *zl = zobj->ptr;
            unsigned char *eptr, *sptr;
            unsigned char *vstr;
            unsigned int vlen;
            long long vlong;

            /* Get the first or last element in the sorted set. */
            eptr = lpSeek(zl, where == RL_ZSET_MAX ? -2 : 0);
            vstr = lpGetValue(eptr, &vlen, &vlong);
            ele = vstr ? sdsnewlen(vstr, vlen) : sdsfromlonglong(vlong);
            sptr = lpNext(zl, eptr);
            score = zzlGetScore(sptr);
        } else {
            zset *zs = zobj->ptr;
            zskiplist *zsl = zs->zsl;
            zskiplistNode *zln = (where == RL_ZSET_MAX ? zsl->tail :
                                               zsl->header->level[0].forward);

            ele = sdsdup(zslGetNodeElement(zln));
            score = zln->score;
        }

        serverAssert(zsetDel(zobj, ele));
        res->eles[res->n] = ele;
        res->scores[res->n] = score;
        res->n++;
    }
    /* rl: upstream deletes the key when the set empties; the handle stays, and
     * an empty zset is what a missing key means for every other command. */
}

static void
rl_pop_result_free(rl_pop_result *res) {
    long i;
    for (i = 0; i < res->n; i++) sdsfree(res->eles[i]);
    if (res->eles) zfree(res->eles);
    if (res->scores) zfree(res->scores);
    res->eles = NULL; res->scores = NULL; res->n = 0;
}

/* ZPOPMIN / ZPOPMAX key [count] -- RESP2 always flat. */
static int
lzpop_generic(lua_State *L, int where) {
    robj *zobj = rl_zset(L, 1);
    long count = -1;                  /* -1 for a plain single pop */
    rl_pop_result res;
    long j;

    int top = lua_gettop(L);
    if (top > 2) return luaL_error(L, "syntax error");
    if (top == 2) {
        long long v;
        if (rl_tolong(L, 2, &v) != C_OK || v < 0)
            return luaL_error(L, "value is out of range, must be positive");
        count = (long)v;
    }
    if (count == -1) count = 1;

    rl_pop_collect(zobj, where, count, &res);
    lua_createtable(L, (int)res.n*2, 0);
    for (j = 0; j < res.n; j++) {
        lua_pushlstring(L, res.eles[j], sdslen(res.eles[j]));
        lua_rawseti(L, -2, j*2+1);
        rl_pushdouble(L, res.scores[j]);
        lua_rawseti(L, -2, j*2+2);
    }
    rl_pop_result_free(&res);
    return 1;
}

static int
lzpopmin(lua_State *L) { return lzpop_generic(L, RL_ZSET_MIN); }
static int
lzpopmax(lua_State *L) { return lzpop_generic(L, RL_ZSET_MAX); }

/* ZMPOP {objects} <MIN|MAX> [COUNT count]
 *
 * Redis replies {keyname, [[member,score],...]}, or nil when none of the keys
 * holds anything. A handle is this port's keyname, so the reply is
 * {<object>, {{member,score},...}} and the source is the first operand that
 * still holds elements, in argument order -- the candidate loop of
 * genericZpopCommand() (t_zset.c:4238). An empty handle stands for a key Redis
 * would not find: an empty zset is deleted there. */
static int
lzmpop(lua_State *L) {
    int where;
    long count = -1;
    rl_object *picked = NULL;
    unsigned int kn, k, picked_idx = 0;
    rl_pop_result res;
    long j;

    luaL_checktype(L, 1, LUA_TTABLE);
    kn = (unsigned int)lua_objlen(L, 1);
    if (kn == 0) return luaL_error(L, "numkeys should be greater than 0");

    if (lua_type(L, 2) != LUA_TSTRING) return luaL_error(L, "syntax error");
    if (!strcasecmp(lua_tostring(L, 2), "min")) where = RL_ZSET_MIN;
    else if (!strcasecmp(lua_tostring(L, 2), "max")) where = RL_ZSET_MAX;
    else return luaL_error(L, "syntax error");

    if (lua_gettop(L) == 4) {
        long long v;

        if (lua_type(L, 3) != LUA_TSTRING ||
            strcasecmp(lua_tostring(L, 3), "count"))
            return luaL_error(L, "syntax error");
        if (rl_tolong(L, 4, &v) != C_OK || v < 1)
            return luaL_error(L, "count should be greater than 0");
        count = (long)v;
    } else if (lua_gettop(L) != 2) {
        return luaL_error(L, "syntax error");
    }
    if (count == -1) count = 1;

    for (k = 1; k <= kn; k++) {
        rl_object *o;

        lua_rawgeti(L, 1, (lua_Integer)k);
        o = rl_check(L, -1, RL_ZSET);
        lua_pop(L, 1);
        if (zsetLength(o->zobj) == 0) continue;
        picked = o;
        picked_idx = k;
        break;
    }
    if (picked == NULL) { rl_pushnilreply(L); return 1; }

    rl_pop_collect(picked->zobj, where, count, &res);

    lua_createtable(L, 2, 0);
    lua_rawgeti(L, 1, (lua_Integer)picked_idx);
    lua_rawseti(L, -2, 1);
    lua_createtable(L, (int)res.n, 0);
    for (j = 0; j < res.n; j++) {
        lua_createtable(L, 2, 0);
        lua_pushlstring(L, res.eles[j], sdslen(res.eles[j]));
        lua_rawseti(L, -2, 1);
        rl_pushdouble(L, res.scores[j]);
        lua_rawseti(L, -2, 2);
        lua_rawseti(L, -2, j+1);
    }
    lua_rawseti(L, -2, 2);
    rl_pop_result_free(&res);
    return 1;
}

/* ---- ZRANDMEMBER -------------------------------------------------------- */

/* Push one listpack entry as Redis replies it: an inline string, or the decimal
 * form of an integer entry. */
static void
rl_pushlpentry(lua_State *L, listpackEntry *ele) {
    if (ele->sval) lua_pushlstring(L, (const char*)ele->sval, ele->slen);
    else {
        char buf[LONG_STR_SIZE];
        lua_pushlstring(L, buf, ll2string(buf, sizeof(buf), ele->lval));
    }
}

/* Walk every (member, score) pair of a zset in ascending order.
 * rl: upstream iterates through the zui* machinery of t_zset.c, which is built
 * on the zsetopsrc / zsetopval typedefs that live inside that file. The two
 * places a facade walk is needed here want only the pair, so each encoding is
 * walked directly instead of copying those types into a facade header. */
static void
rl_zset_walk_all(robj *zobj, void (*emit)(void *arg, sds ele, double score),
                 void *arg) {
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr = lpSeek(zl, 0), *sptr;

        while (eptr) {
            sptr = lpNext(zl, eptr);
            {
                sds ele = lpGetObject(eptr);
                emit(arg, ele, zzlGetScore(sptr));
                sdsfree(ele);
            }
            zzlNext(zl, &eptr, &sptr);
        }
    } else {
        zskiplistNode *ln = ((zset*)zobj->ptr)->zsl->header->level[0].forward;

        while (ln) {
            emit(arg, zslGetNodeElement(ln), ln->score);
            ln = ln->level[0].forward;
        }
    }
}

/* zrandmemberWithCountCommand()'s strategy thresholds -- t_zset.c:4498, :4503. */
#define RL_ZRAND_SUB_STRATEGY_MUL 3
#define RL_ZRAND_RANDOM_SAMPLE_LIMIT 1000

typedef struct {
    lua_State *L;
    int tab;                /* absolute index of the result table */
    int n;                  /* next field in it */
    int withscores;
} rl_rand_out;

/* Push one member (and its score when asked) into the result table */
static void
rl_rand_push_entry(rl_rand_out *out, listpackEntry *ele, double score) {
    rl_pushlpentry(out->L, ele);
    lua_rawseti(out->L, out->tab, out->n++);
    if (out->withscores) {
        rl_pushdouble(out->L, score);
        lua_rawseti(out->L, out->tab, out->n++);
    }
}

static void
rl_rand_push_sds(rl_rand_out *out, sds ele, double score) {
    lua_pushlstring(out->L, ele, sdslen(ele));
    lua_rawseti(out->L, out->tab, out->n++);
    if (out->withscores) {
        rl_pushdouble(out->L, score);
        lua_rawseti(out->L, out->tab, out->n++);
    }
}

/* CASE 3 staging: a dict of every member, from which random ones are removed
 * until the requested count is left (t_zset.c:4637). */
typedef struct {
    dict *d;
} rl_rand_all;

static void
rl_rand_dict_add(void *arg, sds ele, double score) {
    rl_rand_all *staging = arg;
    dictEntry *de = dictAddRaw(staging->d, sdsdup(ele), NULL);

    serverAssert(de);
    dictSetDoubleVal(de, score);
}

/* t_zset.c:950 -- one line over the exported fast_float_strtod(); a listpack
 * score entry that did not fit an integer keeps its textual form. */
static double
rl_zzlstrtod(unsigned char *vstr, unsigned int vlen) {
    return fast_float_strtod((char*)vstr, vlen, NULL);
}

static void
rl_rand_walk_push(void *arg, sds ele, double score) {
    rl_rand_push_sds((rl_rand_out*)arg, ele, score);
}

/* ZRANDMEMBER key [<count> [WITHSCORES]]                      (t_zset.c:4720)
 *
 * RESP2 replies, which is what the flat Lua table mirrors: no count -> one
 * member; count -> an array, and with WITHSCORES an interleaved
 * member/score array. */
static int
lzrandmember(lua_State *L) {
    robj *zsetobj = rl_zset(L, 1);
    unsigned long size = zsetLength(zsetobj);
    unsigned long count;
    long l;
    int withscores = 0, uniq = 1;
    rl_rand_out out;
    int top = lua_gettop(L);

    memset(&out, 0, sizeof(out));
    out.L = L;

    if (top == 1) {
        /* Variant without <count>: reply a single bulk string, nil for a key
         * that is not there -- an empty handle is that, here. */
        listpackEntry ele;

        if (size == 0) { rl_pushnilreply(L); return 1; }
        zsetTypeRandomElement(zsetobj, size, &ele, NULL);
        rl_pushlpentry(L, &ele);
        return 1;
    }

    l = rl_checklong(L, 2);
    if (top == 3) {
        if (lua_type(L, 3) != LUA_TSTRING ||
            strcasecmp(lua_tostring(L, 3), "withscores"))
            return luaL_error(L, "syntax error");
        withscores = 1;
    } else if (top > 3) {
        return luaL_error(L, "syntax error");
    }

    if (l >= 0) count = (unsigned long)l;
    else { count = (unsigned long)(-l); uniq = 0; }

    if (withscores && (l < -LONG_MAX/2 || l > LONG_MAX/2))
        return luaL_error(L, "value is out of range");

    /* If count is zero, serve it ASAP to avoid special cases later. */
    if (count == 0) { lua_createtable(L, 0, 0); return 1; }

    /* CASE 1: a negative count samples "return N random elements" repeatedly,
     * and count == 1 needs no auxiliary structure. The only case that also
     * replies the elements in random order. */
    if (!uniq || count == 1) {
        unsigned long remaining = count;

        out.withscores = withscores;
        lua_createtable(L, (int)(withscores ? count*2 : count), 0);
        out.tab = lua_gettop(L);
        out.n = 1;

        if (zsetobj->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = zsetobj->ptr;
            while (remaining--) {
                dictEntry *de = dictGetFairRandomKey(zs->dict);
                zskiplistNode *znode = dictGetKey(de);
                sds key = zslGetNodeElement(znode);

                rl_rand_push_sds(&out, key, znode->score);
            }
        } else {
            unsigned char *zl = zsetobj->ptr;
            listpackEntry *keys, *vals = NULL;
            unsigned long limit, sample_count;

            limit = count > RL_ZRAND_RANDOM_SAMPLE_LIMIT ?
                    RL_ZRAND_RANDOM_SAMPLE_LIMIT : count;
            keys = zmalloc(sizeof(listpackEntry)*limit);
            if (withscores) vals = zmalloc(sizeof(listpackEntry)*limit);
            while (remaining) {
                unsigned long i;

                sample_count = remaining > limit ? limit : remaining;
                remaining -= sample_count;
                lpRandomPairs(zl, (unsigned int)sample_count, keys, vals, 2);
                for (i = 0; i < sample_count; i++)
                    rl_rand_push_entry(&out, &keys[i],
                        vals ? (vals[i].sval ? rl_zzlstrtod(vals[i].sval, vals[i].slen)
                                             : (double)vals[i].lval) : 0);
            }
            zfree(keys);
            zfree(vals);
        }
        lua_pushvalue(L, out.tab);
        return 1;
    }

    /* CASE 2: the count is at least the size, so reply the whole set. */
    out.withscores = withscores;
    count = count < size ? count : size;     /* the reply size, like reply_size */
    lua_createtable(L, (int)(withscores ? count*2 : count), 0);
    out.tab = lua_gettop(L);
    out.n = 1;

    if (count >= size) {
        /* rl: the walk pushes as it goes, so no iterator type is copied. */
        rl_zset_walk_all(zsetobj, rl_rand_walk_push, &out);
        lua_pushvalue(L, out.tab);
        return 1;
    }

    /* CASE 2.5: listpack only, unique elements, non-random order. */
    if (zsetobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zsetobj->ptr;
        listpackEntry *keys, *vals = NULL;
        unsigned long i;

        keys = zmalloc(sizeof(listpackEntry)*count);
        if (withscores) vals = zmalloc(sizeof(listpackEntry)*count);
        serverAssert(lpRandomPairsUnique(zl, (unsigned int)count, keys, vals, 2)
                     == count);
        for (i = 0; i < count; i++)
            rl_rand_push_entry(&out, &keys[i],
                vals ? (vals[i].sval ? rl_zzlstrtod(vals[i].sval, vals[i].slen)
                                     : (double)vals[i].lval) : 0);
        zfree(keys);
        zfree(vals);
        lua_pushvalue(L, out.tab);
        return 1;
    }

    /* CASE 3: the set is not much bigger than the count, so take everything
     * and remove random members until the count is left. */
    if (count*RL_ZRAND_SUB_STRATEGY_MUL > size) {
        dict *d = dictCreate(&sdsReplyDictType);
        rl_rand_all staging;
        dictIterator di;
        dictEntry *de;

        dictExpand(d, size);
        staging.d = d;
        /* rl: same walk as CASE 2, but the pairs land in the staging dict. */
        rl_zset_walk_all(zsetobj, rl_rand_dict_add, &staging);
        serverAssert(dictSize(d) == size);

        /* Remove random elements to reach the right count. */
        while (size > count) {
            de = dictGetFairRandomKey(d);
            dictUnlink(d, dictGetKey(de));
            sdsfree(dictGetKey(de));
            dictFreeUnlinkedEntry(d, de);
            size--;
        }

        dictInitIterator(&di, d);
        while ((de = dictNext(&di)) != NULL) {
            /* sdsReplyDictType has no destructors (upstream hands the keys
             * straight to the reply); this facade copies them into Lua, so the
             * sds is released here instead. */
            sds key = dictGetKey(de);

            rl_rand_push_sds(&out, key, withscores ? dictGetDoubleVal(de) : 0);
            sdsfree(key);
        }
        dictResetIterator(&di);
        dictRelease(d);
        lua_pushvalue(L, out.tab);
        return 1;
    }

    /* CASE 4: the set is big compared to the count, so sample members and keep
     * the ones not seen yet in a staging dict. */
    {
        dict *d = dictCreate(&hashDictType);
        unsigned long added = 0;

        dictExpand(d, count);
        while (added < count) {
            listpackEntry key;
            double score = 0;
            sds skey;

            zsetTypeRandomElement(zsetobj, size, &key, withscores ? &score : NULL);
            skey = zsetSdsFromListpackEntry(&key);
            if (dictAdd(d, skey, NULL) != DICT_OK) {
                sdsfree(skey);
                continue;
            }
            added++;
            rl_rand_push_entry(&out, &key, score);
        }
        dictRelease(d);
    }
    lua_pushvalue(L, out.tab);
    return 1;
}

/* ---- ZUNION / ZINTER / ZDIFF and their STORE / CARDINALITY forms -------- */

#define RL_SET_OP_UNION 0
#define RL_SET_OP_DIFF  1
#define RL_SET_OP_INTER 2

/* zunionInterDiffGenericCommand() -- t_zset.c:2885.
 *
 * Redis' `numkeys key [key ...]` collapses to one array table of handles
 * (spec §3.1), so the call shapes are
 *
 *   sortedset.zunion({a,b}, ["weights", {1,2}], ["aggregate", "min"], ["withscores"])
 *   sortedset.zunionstore(dst, {a,b}, ...)   and   dst:zunionstore({a,b}, ...)
 *   sortedset.zintercard({a,b}, ["limit", n])
 *
 * `keyidx` is the index of the operand table: 1 for the read forms, 2 for the
 * store forms, whose destination is argument 1. rl: Redis counts the keys in an
 * integer argument, so its "would the declared count overflow the real argv"
 * check has no input here -- the table length IS the count. */
static int
rl_setop_generic(lua_State *L, int keyidx, int op, int cardinality_only,
                 const char *cmdname) {
    rl_object *dst = keyidx == 2 ? rl_check(L, 1, RL_ZSET) : NULL;
    zsetopsrc *src;
    zsetopval zval;
    long setnum, i;
    int j, aggregate = REDIS_AGGR_SUM;
    int withscores = 0;
    long limit = 0, top = lua_gettop(L);
    size_t maxelelen = 0, totelelen = 0;
    robj *dstobj = NULL;
    zset *dstzset = NULL;
    unsigned long cardinality = 0;

    luaL_checktype(L, keyidx, LUA_TTABLE);
    setnum = (long)lua_objlen(L, keyidx);
    if (setnum < 1)
        return luaL_error(L,
            "at least 1 input key is needed for '%s' command", cmdname);

    src = ztrycalloc(sizeof(zsetopsrc)*setnum);
    if (src == NULL)
        return luaL_error(L,
            "Insufficient memory, failed allocating transient memory, too many args.");

    /* read keys to be used for input */
    for (i = 0; i < setnum; i++) {
        rl_object *o;

        lua_rawgeti(L, keyidx, (lua_Integer)(i+1));
        o = rl_check(L, -1, RL_ZSET);
        lua_pop(L, 1);
        src[i].subject = o->zobj;
        src[i].type = OBJ_ZSET;
        src[i].encoding = o->zobj->encoding;
        src[i].weight = 1.0;         /* Default all weights to 1. */
    }

    /* parse optional extra arguments */
    for (j = keyidx + 1; j <= top;) {
        /* Arguments still to read, this option word included -- the same
         * counting upstream does with `c->argc - j`. */
        long remaining = top - j + 1;
        const char *opt;

        if (lua_type(L, j) != LUA_TSTRING) {
            zfree(src);
            return luaL_error(L, "syntax error");
        }
        opt = lua_tostring(L, j);

        if (op != RL_SET_OP_DIFF && !cardinality_only &&
            remaining >= 2 && !strcasecmp(opt, "weights")) {
            long k;

            j++;
            if (lua_type(L, j) != LUA_TTABLE ||
                (long)lua_objlen(L, j) != setnum) {
                zfree(src);
                return luaL_error(L, "syntax error");
            }
            for (k = 0; k < setnum; k++) {
                double w;
                int rc;

                lua_rawgeti(L, j, (lua_Integer)(k+1));
                rc = rl_todouble(L, -1, &w);
                lua_pop(L, 1);
                if (rc != C_OK) {
                    zfree(src);
                    return luaL_error(L, "weight value is not a float");
                }
                src[k].weight = w;
            }
            j++;
        } else if (op != RL_SET_OP_DIFF && !cardinality_only &&
                   remaining >= 2 && !strcasecmp(opt, "aggregate")) {
            j++;
            if (lua_type(L, j) != LUA_TSTRING) {
                zfree(src);
                return luaL_error(L, "syntax error");
            }
            if      (!strcasecmp(lua_tostring(L, j), "sum"))   aggregate = REDIS_AGGR_SUM;
            else if (!strcasecmp(lua_tostring(L, j), "min"))   aggregate = REDIS_AGGR_MIN;
            else if (!strcasecmp(lua_tostring(L, j), "max"))   aggregate = REDIS_AGGR_MAX;
            else if (!strcasecmp(lua_tostring(L, j), "count")) aggregate = REDIS_AGGR_COUNT;
            else {
                zfree(src);
                return luaL_error(L, "syntax error");
            }
            j++;
        } else if (remaining >= 1 && !dst && !cardinality_only &&
                   !strcasecmp(opt, "withscores")) {
            j++;
            withscores = 1;
        } else if (cardinality_only && remaining >= 1 &&
                   !strcasecmp(opt, "limit")) {
            long long v;

            j++;
            if (rl_tolong(L, j, &v) != C_OK || v < 0) {
                zfree(src);
                return luaL_error(L, "LIMIT can't be negative");
            }
            limit = (long)v;
            j++;
        } else {
            zfree(src);
            return luaL_error(L, "syntax error");
        }
    }

    if (op != RL_SET_OP_DIFF) {
        /* sort sets from the smallest to largest, this will improve our
         * algorithm's performance */
        qsort(src, (size_t)setnum, sizeof(zsetopsrc), zuiCompareByCardinality);
    }

    /* We need a temp zset object to store our union/inter/diff. */
    if (!cardinality_only) {
        dstobj = createZsetObject();
        dstzset = dstobj->ptr;
    }
    memset(&zval, 0, sizeof(zval));

    if (op == RL_SET_OP_INTER) {
        /* Skip everything if the smallest input is empty. */
        if (zuiLength(&src[0]) > 0) {
            /* Precondition: as src[0] is non-empty and the inputs are ordered
             * by size, all src[i > 0] are non-empty too. */
            zuiInitIterator(&src[0]);
            while (zuiNext(&src[0], &zval)) {
                double score, value;
                long n;
                int present = 1;

                score = zuiWeightedScore(zval.score, src[0].weight, aggregate);
                if (isnan(score)) score = 0;

                for (n = 1; n < setnum; n++) {
                    /* It is not safe to access the zset we are iterating, so
                     * explicitly check for equal object. */
                    if (src[n].subject == src[0].subject) {
                        value = zuiWeightedScore(zval.score, src[n].weight, aggregate);
                        zunionInterAggregate(&score, value, aggregate);
                    } else if (zuiFind(&src[n], &zval, &value)) {
                        value = zuiWeightedScore(value, src[n].weight, aggregate);
                        zunionInterAggregate(&score, value, aggregate);
                    } else {
                        present = 0;
                        break;
                    }
                }

                /* Only continue when present in every input. */
                if (present && cardinality_only) {
                    cardinality++;
                    if (limit && cardinality >= (unsigned long)limit) {
                        zuiDiscardDirtyValue(&zval);
                        break;
                    }
                } else if (present) {
                    sds tmp = zuiNewSdsFromValue(&zval);
                    zskiplistNode *znode = zslInsert(dstzset->zsl, score, tmp);

                    dictAdd(dstzset->dict, znode, NULL);
                    totelelen += sdslen(tmp);
                    if (sdslen(tmp) > maxelelen) maxelelen = sdslen(tmp);
                    sdsfree(tmp);   /* zslInsert copied it */
                }
            }
            zuiClearIterator(&src[0]);
        }
    } else if (op == RL_SET_OP_UNION) {
        /* rl: upstream aggregates straight into the destination's dict of
         * not-yet-linked skiplist nodes, which needs the file-local
         * zslCreateNode()/zslInsertNode(); the facade aggregates member ->
         * score in a dict and then inserts each finished score with the
         * exported zslInsert(). Same O(sum of sizes + K log K) work, same
         * result, no copied skiplist internals. */
        dict *agg = dictCreate(&sdsReplyDictType);
        dictIterator adi;
        dictEntry *ade;

        if (setnum) {
            /* Our union is at least as large as the largest set (src is sorted
             * by cardinality, so src[setnum-1] is the biggest). */
            dictExpand(agg, (long)zuiLength(&src[setnum-1]));
        }

        for (i = 0; i < setnum; i++) {
            if (zuiLength(&src[i]) == 0) continue;

            zuiInitIterator(&src[i]);
            while (zuiNext(&src[i], &zval)) {
                double score = zuiWeightedScore(zval.score, src[i].weight, aggregate);
                dictEntry *de;

                if (isnan(score)) score = 0;
                de = dictFind(agg, zuiSdsFromValue(&zval));
                if (de == NULL) {
                    sds key = zuiNewSdsFromValue(&zval);

                    de = dictAddRaw(agg, key, NULL);
                    serverAssert(de);
                    dictSetDoubleVal(de, score);
                } else {
                    double cur = dictGetDoubleVal(de);

                    zunionInterAggregate(&cur, score, aggregate);
                    dictSetDoubleVal(de, cur);
                }
            }
            zuiClearIterator(&src[i]);
        }

        dictInitIterator(&adi, agg);
        while ((ade = dictNext(&adi)) != NULL) {
            sds key = dictGetKey(ade);
            zskiplistNode *znode = zslInsert(dstzset->zsl, dictGetDoubleVal(ade), key);

            dictAdd(dstzset->dict, znode, NULL);
            totelelen += sdslen(key);
            if (sdslen(key) > maxelelen) maxelelen = sdslen(key);
            /* sdsReplyDictType owns nothing (that is why upstream can hand its
             * keys straight to a reply); the zset has its own copy now. */
            sdsfree(key);
        }
        dictResetIterator(&adi);
        dictRelease(agg);
    } else if (op == RL_SET_OP_DIFF) {
        /* rl: upstream's ZDIFF algorithm block is file-local and its algorithm
         * 2 deletes through the file-local zslDelete(), so the facade runs the
         * same algorithm 2 with the exported dict API instead: stage the first
         * operand, delete every later operand's members from the staging dict,
         * then insert what survived. Same result, same weights-ignored rule, and
         * ZDIFF takes src[0] as the minuend in both (no size sort for DIFF). */
        dict *left = dictCreate(&sdsReplyDictType);
        dictIterator di;
        dictEntry *de;

        dictExpand(left, (long)zuiLength(&src[0]));
        if (zuiLength(&src[0]) > 0) {
            zuiInitIterator(&src[0]);
            while (zuiNext(&src[0], &zval)) {
                sds key = zuiNewSdsFromValue(&zval);

                de = dictAddRaw(left, key, NULL);
                serverAssert(de);
                dictSetDoubleVal(de, zval.score);
            }
            zuiClearIterator(&src[0]);
        }

        for (i = 1; i < setnum && dictSize(left) > 0; i++) {
            if (zuiLength(&src[i]) == 0) continue;

            zuiInitIterator(&src[i]);
            while (zuiNext(&src[i], &zval)) {
                dictEntry *hit = dictFind(left, zuiSdsFromValue(&zval));

                if (hit != NULL) {
                    sds owned = dictGetKey(hit);

                    dictDelete(left, owned);
                    sdsfree(owned);    /* sdsReplyDictType owns nothing */
                }
            }
            zuiClearIterator(&src[i]);
        }

        dictInitIterator(&di, left);
        while ((de = dictNext(&di)) != NULL) {
            sds key = dictGetKey(de);
            zskiplistNode *znode = zslInsert(dstzset->zsl, dictGetDoubleVal(de), key);

            dictAdd(dstzset->dict, znode, NULL);
            totelelen += sdslen(key);
            if (sdslen(key) > maxelelen) maxelelen = sdslen(key);
            sdsfree(key);
        }
        dictResetIterator(&di);
        dictRelease(left);
    } else {
        zfree(src);
        redislib_panic("redislib: unknown set operation %d", op);
    }
    zfree(src);

    if (dst) {
        if (dstzset->zsl->length) {
            /* zrangeResultFinalizeStore()/setKey() equivalent: the destination
             * ends up holding exactly the computed value. */
            zsetConvertToListpackIfNeeded(dstobj, maxelelen, totelelen);
            rl_zset_replace(dst, dstobj);
            rl_pushint(L, (long long)zsetLength(dstobj));
        } else {
            /* Redis replies 0 and deletes the destination key; an empty handle
             * is that, for a value that stays addressable. */
            rl_free_zset(dstobj);
            rl_zset_replace(dst, zsetTypeCreate(0, 0));
            rl_pushint(L, 0);
        }
        return 1;
    }

    if (cardinality_only) {
        rl_pushint(L, (long long)cardinality);
        return 1;
    }

    /* ZUNION / ZINTER / ZDIFF reply: the result in ascending order, flat with
     * the score interleaved when WITHSCORES was asked (RESP2 shape). */
    {
        unsigned long length = dstzset->zsl->length;
        zskiplistNode *zn = dstzset->zsl->header->level[0].forward;
        int idx = 1;

        lua_createtable(L, (int)(withscores ? length*2 : length), 0);
        while (zn != NULL) {
            sds ele = zslGetNodeElement(zn);

            lua_pushlstring(L, ele, sdslen(ele));
            lua_rawseti(L, -2, idx++);
            if (withscores) {
                rl_pushdouble(L, zn->score);
                lua_rawseti(L, -2, idx++);
            }
            zn = zn->level[0].forward;
        }
        rl_free_zset(dstobj);
    }
    return 1;
}

static int
lzunion(lua_State *L) { return rl_setop_generic(L, 1, RL_SET_OP_UNION, 0, "zunion"); }
static int
lzinter(lua_State *L) { return rl_setop_generic(L, 1, RL_SET_OP_INTER, 0, "zinter"); }
static int
lzdiff(lua_State *L) { return rl_setop_generic(L, 1, RL_SET_OP_DIFF, 0, "zdiff"); }
static int
lzintercard(lua_State *L) {
    return rl_setop_generic(L, 1, RL_SET_OP_INTER, 1, "zintercard");
}

/* The store family: the destination is argument 1 in both call shapes. */
static int
lsetop_store(lua_State *L, int op, const char *cmdname) {
    (void)rl_check(L, 1, RL_ZSET);
    return rl_setop_generic(L, 2, op, 0, cmdname);
}

static int
lzunionstore(lua_State *L) { return lsetop_store(L, RL_SET_OP_UNION, "zunionstore"); }
static int
lzinterstore(lua_State *L) { return lsetop_store(L, RL_SET_OP_INTER, "zinterstore"); }
static int
lzdiffstore(lua_State *L) { return lsetop_store(L, RL_SET_OP_DIFF, "zdiffstore"); }

/* ---- registration ------------------------------------------------------- */

static const luaL_Reg zset_methods[] = {
    {"zadd", lzadd},
    {"zincrby", lzincrby},
    {"zrem", lzrem},
    {"zremrangebyrank", lzremrangebyrank},
    {"zremrangebyscore", lzremrangebyscore},
    {"zremrangebylex", lzremrangebylex},
    {"zcard", lzcard},
    {"zscore", lzscore},
    {"zmscore", lzmscore},
    {"zrank", lzrank},
    {"zrevrank", lzrevrank},
    {"zcount", lzcount},
    {"zlexcount", lzlexcount},
    {"zrange", lzrange},
    {"zrevrange", lzrevrange},
    {"zrangebyscore", lzrangebyscore},
    {"zrevrangebyscore", lzrevrangebyscore},
    {"zrangebylex", lzrangebylex},
    {"zrevrangebylex", lzrevrangebylex},
    {"zrangestore", lzrangestore},
    {"zpopmin", lzpopmin},
    {"zpopmax", lzpopmax},
    {"zmpop", lzmpop},
    {"zrandmember", lzrandmember},
    {"zunionstore", lzunionstore},
    {"zinterstore", lzinterstore},
    {"zdiffstore", lzdiffstore},
    {"free", lzset_free},
    {NULL, NULL},
};

static const luaL_Reg zset_module_methods[] = {
    {"new", lzset_new},
    {"zrangestore", lzrangestore},
    {"zmpop", lzmpop},
    {"zunion", lzunion},
    {"zinter", lzinter},
    {"zdiff", lzdiff},
    {"zintercard", lzintercard},
    {"zunionstore", lzunionstore},
    {"zinterstore", lzinterstore},
    {"zdiffstore", lzdiffstore},
    {NULL, NULL},
};

/* LuaJIT's searcher uses the full module name with dots converted to
 * underscores (same rule as NetBull's luaopen_skiplist_c for "skiplist.c"). */
int luaopen_redis_sortedset(lua_State *L) {
    rl_register_class(L, RL_ZSET, zset_methods, NULL);
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, zset_module_methods, 0);
    return 1;
}
