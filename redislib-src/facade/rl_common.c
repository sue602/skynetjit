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

/* ---- the sorted-set family blob (spec §4) ------------------------------- */

void rl_zset_walk(robj *zobj, void (*emit)(void *arg, sds ele, double score),
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

/* The blob's integers are little-endian by construction, written and read with
 * shifts rather than a cast, so neither byte order nor alignment is assumed. */
static void
rl_wr32(unsigned char *p, uint32_t v) {
    p[0] = v & 0xff; p[1] = (v >> 8) & 0xff;
    p[2] = (v >> 16) & 0xff; p[3] = (v >> 24) & 0xff;
}

static void
rl_wr64(unsigned char *p, uint64_t v) {
    int i;

    for (i = 0; i < 8; i++) p[i] = (v >> (i*8)) & 0xff;
}

static uint32_t
rl_rd32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint64_t
rl_rd64(const unsigned char *p) {
    uint64_t v = 0;
    int i;

    for (i = 7; i >= 0; i--) v = (v << 8) | p[i];
    return v;
}

typedef struct {
    sds *out;
    unsigned char hdr[12];
} rl_dump_emit;

static void
rl_dump_append(void *arg, sds ele, double score) {
    rl_dump_emit *st = arg;
    uint64_t bits;

    memcpy(&bits, &score, sizeof(bits));
    rl_wr64(st->hdr, bits);
    rl_wr32(st->hdr+8, (uint32_t)sdslen(ele));
    *st->out = sdscatlen(*st->out, st->hdr, sizeof(st->hdr));
    *st->out = sdscatlen(*st->out, ele, sdslen(ele));
}

sds rl_dump_zset(robj *zobj) {
    rl_dump_emit st;
    unsigned char count[4];
    sds out = sdsempty();

    st.out = &out;                /* the emit callback moves it as it appends */
    rl_wr32(count, (uint32_t)zsetLength(zobj));
    out = sdscatlen(out, count, sizeof(count));
    rl_zset_walk(zobj, rl_dump_append, &st);
    return out;
}

robj *rl_load_zset(lua_State *L, const char *buf, size_t len) {
    const unsigned char *p = (const unsigned char*)buf;
    const unsigned char *end = p + len;
    robj *zobj = NULL;
    uint32_t count, i;

    if (len < 4) goto bad;
    count = rl_rd32(p);
    p += 4;

    /* Every record takes at least its 12 header bytes, so a count larger than
     * that is already a bad blob. Checked before the object is created because
     * the object sizes its dictionary from this number -- without the check a
     * four-byte "count 4294967295" would ask for a 4G-bucket dict. */
    if (count > (uint32_t)((len - 4) / 12)) goto bad;

    /* The size hint is exact here, which is what a ZADD of this many elements
     * would hand zsetTypeCreate(). */
    zobj = zsetTypeCreate(count, 0);
    for (i = 0; i < count; i++) {
        uint64_t bits;
        uint32_t mlen;
        double score;
        sds ele;
        int out_flags = 0;
        double out_score = 0;

        if ((size_t)(end - p) < 12) goto bad;
        bits = rl_rd64(p);
        memcpy(&score, &bits, sizeof(score));
        p += 8;
        mlen = rl_rd32(p);
        p += 4;
        if ((size_t)(end - p) < mlen) goto bad;
        ele = sdsnewlen(p, mlen);
        p += mlen;
        /* zsetAdd() refuses NaN itself (t_zset.c:4267, the same rule that makes
         * ZADD reject it), so a blob carrying one is not a set we can hand
         * back: its skiplist ordering would be undefined. */
        if (!zsetAdd(zobj, score, ele, ZADD_IN_NONE, &out_flags, &out_score)) {
            sdsfree(ele);
            goto bad;
        }
        sdsfree(ele);                /* the set owns its copy */
    }
    if (p != end) goto bad;           /* trailing bytes */
    return zobj;

bad:
    /* rl: Redis' own text for a serialized payload it cannot parse (the reply
     * for a bad RESTORE payload, cluster.c:290). `zobj` is NULL until the
     * header checks pass, and is released here when it exists -- the handle the
     * caller pushed is still empty, so the raise cannot leave a half-built set
     * behind and the userdata's own __gc has nothing to double-free. */
    if (zobj) rl_free_zset(zobj);
    rl_error(L, "Bad data format");
    return NULL;                      /* not reached */
}

const char *rl_checkblob(lua_State *L, int idx, size_t *len) {
    if (lua_type(L, idx) != LUA_TSTRING)
        rl_error(L, "string expected, got %s", lua_typename(L, lua_type(L, idx)));
    return lua_tolstring(L, idx, len);
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

void rl_pushvalue(lua_State *L, rl_object *o) {
    size_t n = o->str ? sdslen(o->str) : 0;

    lua_pushlstring(L, o->str ? (char*)o->str : "", n);
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
