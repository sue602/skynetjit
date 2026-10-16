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
    "redis.set",
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

void rl_free_set(robj *sobj) {
    /* object.c's freeSetObject() for all three set encodings, plus the robj it
     * leaves alone (t_set.c uses that half to replace a payload in place; here
     * the whole value goes). */
    if (sobj == NULL) return;
    freeSetObject(sobj);
    zfree(sobj);
}

void rl_checkmember(lua_State *L, int idx) {
    if (!lua_isstring(L, idx))
        rl_error(L, "string or number expected, got %s",
                 lua_typename(L, lua_type(L, idx)));
}

/* An argument a Redis command gets from its argv, so "not passed" is a different
 * failure from "passed the wrong kind": a server rejects the first with its
 * command table's arity check and the second inside the command. This port has
 * no command table (NOTICE §6), so both texts are produced here -- the arity
 * line verbatim from server.c:2225, the kind line the facade's own, since a RESP
 * argument cannot be a table at all (NOTICE §7). */
void rl_checklist(lua_State *L, int idx, const char *cmdname) {
    if (lua_gettop(L) < idx)
        rl_error(L, "wrong number of arguments for '%s' command", cmdname);
    if (!lua_istable(L, idx))
        rl_error(L, "table expected, got %s",
                 lua_typename(L, lua_type(L, idx)));
}

const char *rl_argstring(lua_State *L, int idx, const char *cmdname) {
    if (lua_gettop(L) < idx)
        rl_error(L, "wrong number of arguments for '%s' command", cmdname);
    rl_checkmember(L, idx);
    return lua_tostring(L, idx);
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

/* ---- the set family blob (spec §4) -------------------------------------- */

/* Ascending byte order, ties broken by length: a total order on distinct member
 * byte strings, which is what makes one set write one blob. */
static int rl_cmp_members(const void *pa, const void *pb) {
    sds a = *(const sds*)pa, b = *(const sds*)pb;
    size_t la = sdslen(a), lb = sdslen(b);
    int r = memcmp(a, b, la < lb ? la : lb);

    if (r) return r;
    return la < lb ? -1 : (la > lb ? 1 : 0);
}

/* Every member, as a fresh sds per element -- setTypeNextObject()'s documented
 * contract ("if you don't retain a pointer to this object you should call
 * sdsfree() against it"), which is why it is the entry point a copy-out walk
 * uses. This is upstream's own loop shape (t_set.c:536 setTypeConvertAndExpand).
 * An integer member comes back as its decimal text, the bytes SMEMBERS replies. */
static sds *
rl_set_members(robj *sobj, unsigned long *out) {
    sds *members = NULL;
    unsigned long n = 0, cap = 0;

    if (sobj != NULL) {
        setTypeIterator si;
        sds ele;

        setTypeInitIterator(&si, sobj);
        while ((ele = setTypeNextObject(&si)) != NULL) {
            if (n == cap) {
                cap = cap ? cap * 2 : 16;
                members = zrealloc(members, sizeof(sds) * cap);
            }
            members[n++] = ele;
        }
        setTypeResetIterator(&si);
    }
    *out = n;
    return members;
}

sds rl_dump_set(robj *sobj) {
    sds *members;
    unsigned long n = 0, i;
    unsigned char hdr[4];
    sds out;

    members = rl_set_members(sobj, &n);
    if (n > 1) qsort(members, (size_t)n, sizeof(sds), rl_cmp_members);

    out = sdsempty();
    rl_wr32(hdr, (uint32_t)n);
    out = sdscatlen(out, hdr, sizeof(hdr));
    for (i = 0; i < n; i++) {
        rl_wr32(hdr, (uint32_t)sdslen(members[i]));
        out = sdscatlen(out, hdr, sizeof(hdr));
        out = sdscatlen(out, members[i], sdslen(members[i]));
        sdsfree(members[i]);
    }
    if (members) zfree(members);
    return out;
}

/* Where each record's bytes start, so the whole shape can be validated before a
 * single byte is copied into the object being built. */
typedef struct {
    const unsigned char *p;
    uint32_t len;
} rl_member_ref;

robj *rl_load_set(lua_State *L, const char *buf, size_t len) {
    const unsigned char *p = (const unsigned char*)buf;
    const unsigned char *end = p + len;
    rl_member_ref *rec = NULL;
    robj *sobj = NULL;
    uint32_t count, i;

    if (len < 4) goto bad;
    count = rl_rd32(p);
    p += 4;

    /* Every record costs at least its 4-byte length, so a count that cannot be
     * filled from what is left is a bad blob. Checked before anything is sized
     * from it -- the sorted-set loader has the same rule and the same reason: a
     * four-byte "count 4294967295" must not ask for a 4G array. */
    if (count > (uint32_t)((len - 4) / 4)) goto bad;

    /* An empty blob has no records but still needs a non-NULL block: zmalloc(0)
     * may hand back NULL, which would look like an allocation failure. */
    rec = zmalloc(sizeof(*rec) * (count ? count : 1));
    if (rec == NULL) goto bad;

    for (i = 0; i < count; i++) {
        if ((size_t)(end - p) < 4) goto bad;
        rec[i].len = rl_rd32(p);
        p += 4;
        if ((size_t)(end - p) < rec[i].len) goto bad;
        rec[i].p = p;
        p += rec[i].len;
    }
    if (p != end) goto bad;                 /* trailing bytes */

    /* The shape is validated: nothing between here and the return can fail, so a
     * raise never leaves a half-built set behind and the handle the caller pushed
     * stays empty. Redis picks a new key's payload from the first member and from
     * how many members the call brings (saddCommand's
     * setTypeCreate(argv[2]->ptr, c->argc-2)), and the blob has both, so the same
     * factory decision is made here: a loaded set is indistinguishable from one
     * that was SADDed as this list, encoding included. */
    for (i = 0; i < count; i++) {
        sds ele = sdsnewlen(rec[i].p, rec[i].len);

        if (sobj == NULL) sobj = setTypeCreate(ele, count);
        setTypeAdd(sobj, ele);
        sdsfree(ele);                       /* the set owns its copy */
    }
    zfree(rec);
    return sobj;

bad:
    /* rl: Redis' own text for a serialized payload it cannot parse (the reply
     * for a bad RESTORE payload, cluster.c:290), same as rl_load_zset(). */
    if (rec) zfree(rec);
    if (sobj) rl_free_set(sobj);
    rl_error(L, "Bad data format");
    return NULL;                            /* not reached */
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
    case RL_SET:
        rl_free_set(o->sobj);
        o->sobj = NULL;
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
    luaL_getmetatable(L, class_names[kind]);
    lua_setmetatable(L, -2);
}
