/* hll.c -- the redis.hll facade: PFADD PFCOUNT PFMERGE PFDEBUG PFSELFTEST
 * over an in-memory Redis HyperLogLog string value (sds with the 16 byte
 * "HYLL" header).
 *
 * Faithful transcription of Redis 8.10.0 src/hyperloglog.c pf*Command() with
 * the client / keyspace layer removed: the value the command looks up becomes
 * the rl_object's sds, addReply* becomes the rl_push* mapping (see
 * rl_common.h), and every algorithmic step is a call into the ported kernel
 * (hllAdd / hllCount / hllMerge / hllSparseToDense / hllDenseCompress /
 * hllDenseSet / hllSparseSet / createHLLObject) rather than a rewrite. Error
 * texts are the upstream strings.
 *
 * Because an rl_object holds the sds itself, kernel calls that take a robj*
 * and may replace o->ptr (hllAdd / hllSparseSet / hllSparseToDense all
 * realloc the string) are handed a stack robj view built by rl_hll_view() and
 * written back with o->str = tmp.ptr. That view is the port's stand-in for
 * "the object the key points at", and writing back after every call is what
 * keeps the sds the facade frees identical to the one the kernel last
 * allocated.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>

#include "rl_common.h"
#include "hll_int.h"

/* Kernel entry points (redislib-src/kernel/hyperloglog.c). */
robj *createHLLObject(void);
int hllAdd(robj *o, unsigned char *ele, size_t elesize);
int hllDenseAdd(uint8_t *registers, unsigned char *ele, size_t elesize);
int hllDenseSet(uint8_t *registers, long index, uint8_t count);
int hllSparseSet(robj *o, long index, uint8_t count);
int hllSparseToDense(robj *o);
void hllDenseCompress(uint8_t *reg_dense, const uint8_t *reg_raw);
int hllMerge(uint8_t *max, robj *hll);
uint64_t hllCount(struct hllhdr *hdr, int *invalid);

/* hyperloglog.c:209, the reply text of every corrupted-value error (addReplyError
 * strips the leading '-'). */
static const char *invalid_hll_err = "-INVALIDOBJ Corrupted HLL object detected";

/* isHLLObjectOrReply()'s reply text, hyperloglog.c:1636-1637. */
static const char *not_hll_err =
    "-WRONGTYPE Key is not a valid HyperLogLog string value.";

/* ---- argument helpers --------------------------------------------------- */

/* A kernel call that takes a robj* is handed a stack view of the facade's sds.
 * The two nibbles are filled in because the view stands for a Redis string
 * object, exactly like the one createHLLObject() returns; no ported HLL
 * function reads them, but leaving them uninitialised would make the view
 * observably different from the object upstream passes. */
static void
rl_hll_view(robj *tmp, rl_object *o) {
    tmp->type = OBJ_STRING;
    tmp->encoding = OBJ_ENCODING_RAW;
    tmp->ptr = o->str;
}

/* isHLLObjectOrReply() minus checkType()/sdsEncodedObject(): rl_check() already
 * proved the kind, and an rl_object string is always an sds. */
static void
rl_checkhll(lua_State *L, rl_object *o) {
    struct hllhdr *hdr;

    if (o->str == NULL || sdslen(o->str) < sizeof(*hdr))
        rl_error(L, "%s", not_hll_err);
    hdr = (struct hllhdr*)o->str;
    if (hdr->magic[0] != 'H' || hdr->magic[1] != 'Y' ||
        hdr->magic[2] != 'L' || hdr->magic[3] != 'L')
        rl_error(L, "%s", not_hll_err);
    if (hdr->encoding > HLL_MAX_ENCODING)
        rl_error(L, "%s", not_hll_err);
    if (hdr->encoding == HLL_DENSE && sdslen(o->str) != HLL_DENSE_SIZE)
        rl_error(L, "%s", not_hll_err);
}

/* One PFADD element: the bytes of a Lua string (or of a number's decimal
 * form, which is what lua_tolstring() hands out). Type rules are the shared
 * member check's, including its Lua-side text. */
static unsigned char *
rl_element(lua_State *L, int idx, size_t *len) {
    rl_checkmember(L, idx);
    return (unsigned char*)lua_tolstring(L, idx, len);
}

/* HLL operands from args [from..top]: every argument is either an hll object
 * or an array table of hll objects (spec §3.1 dual-form input).
 *
 * Walking the Lua stack instead of copying the pointers into a zmalloc'd array
 * keeps the two things this list is used for -- the operand count and the
 * per-operand rl_check() -- free of a side allocation, so a WRONGTYPE raised
 * halfway through a PFMERGE cannot leak one. */
typedef struct {
    lua_State *L;
    int argi;         /* argument being walked now */
    int elt;          /* next element inside argi when that is a table */
    int elts;
    int tbl;          /* argi value currently expanded as a table, else 0 */
} rl_argiter;

/* Number of operands without type-checking any of them. */
static int
rl_hll_argcount(lua_State *L, int from) {
    int n = 0, i;

    for (i = from; i <= lua_gettop(L); i++)
        n += lua_istable(L, i) ? (int)lua_objlen(L, i) : 1;
    return n;
}

static void
rl_hll_iter(rl_argiter *it, lua_State *L, int from) {
    it->L = L;
    it->argi = from;
    it->elt = 0;
    it->elts = 0;
    it->tbl = 0;
}

/* Next operand, NULL once the list is exhausted. A table argument is read with
 * its array part only (lua_objlen), like every other redislib list argument. */
static rl_object *
rl_hll_next(rl_argiter *it) {
    lua_State *L = it->L;

    while (it->argi <= lua_gettop(L)) {
        if (!lua_istable(L, it->argi)) {
            rl_object *o = rl_check(L, it->argi, RL_HLL);
            it->argi++;
            return o;
        }
        if (it->tbl != it->argi) {
            it->tbl = it->argi;
            it->elt = 1;
            it->elts = (int)lua_objlen(L, it->argi);
        }
        while (it->elt <= it->elts) {
            rl_object *o;
            int k = it->elt++;

            lua_rawgeti(L, it->argi, k);
            o = rl_check(L, -1, RL_HLL);
            lua_pop(L, 1);
            return o;
        }
        it->tbl = 0;
        it->argi++;
    }
    return NULL;
}

/* ---- PFADD -------------------------------------------------------------- */

/* PFADD var ele ... => :0 or :1 (pfaddCommand, hyperloglog.c:1642) */
static int
lhll_pfadd(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_HLL);
    struct hllhdr *hdr;
    robj tmp;
    int updated = 0, j;
    int top = lua_gettop(L);

    rl_checkhll(L, o);
    rl_hll_view(&tmp, o);

    for (j = 2; j <= top; j++) {
        size_t len;
        unsigned char *ele = rl_element(L, j, &len);
        int retval;

        /* hllAdd() can replace the sds (sparse growth reallocates, and a
         * promote frees the old representation), so write it back before
         * anything else has a chance to raise: until o->str is current, the
         * object would own a pointer the kernel already released. */
        retval = hllAdd(&tmp, ele, len);
        o->str = tmp.ptr;
        if (retval == 1) updated++;
        else if (retval == -1) rl_error(L, "%s", invalid_hll_err);
    }

    hdr = (struct hllhdr*)o->str;
    if (updated) HLL_INVALIDATE_CACHE(hdr);
    rl_pushint(L, updated ? 1 : 0);
    return 1;
}

/* ---- PFCOUNT ------------------------------------------------------------ */

/* PFCOUNT var [var ...] (pfcountCommand, hyperloglog.c:1696) */
static int
lhll_pfcount(lua_State *L) {
    rl_argiter it;
    rl_object *o;
    int n = rl_hll_argcount(L, 1);

    if (n == 0) {
        /* rl: upstream replies :0 for a missing key; here an operand list is
         * built from arguments, so "no operands" is a Lua-side arity error,
         * worded the way the server's own arity check words it for PFCOUNT. */
        return rl_error(L, "wrong number of arguments for '%s' command", "pfcount");
    }

    if (n > 1) {
        /* Case 1: cardinality of the union, computed in a throwaway HLL_RAW
         * value and never written back -- as in Redis. */
        uint8_t max[HLL_HDR_SIZE+HLL_REGISTERS];
        struct hllhdr *hdr;
        uint8_t *registers;

        memset(max,0,sizeof(max));
        hdr = (struct hllhdr*) max;
        hdr->encoding = HLL_RAW;
        registers = max + HLL_HDR_SIZE;

        rl_hll_iter(&it, L, 1);
        while ((o = rl_hll_next(&it)) != NULL) {
            robj tmp;

            rl_checkhll(L, o);
            rl_hll_view(&tmp, o);
            if (hllMerge(registers,&tmp) == C_ERR) {
                rl_error(L, "%s", invalid_hll_err);
            }
        }
        rl_pushint(L,(long long)hllCount(hdr,NULL));
        return 1;
    }

    /* Case 2: single value. The cache lives inside the string, so a PFCOUNT
     * may rewrite the value exactly like it does on a real server.
     * rl: upstream also marks the key dirty so the cache change propagates in
     * replication/AOF; a redislib object has no replica, so only the bytes are
     * written back. */
    rl_hll_iter(&it, L, 1);
    o = rl_hll_next(&it);
    {
        struct hllhdr *hdr;
        uint64_t card;

        rl_checkhll(L, o);
        hdr = (struct hllhdr*)o->str;

        if (HLL_VALID_CACHE(hdr)) {
            card = (uint64_t)hdr->card[0];
            card |= (uint64_t)hdr->card[1] << 8;
            card |= (uint64_t)hdr->card[2] << 16;
            card |= (uint64_t)hdr->card[3] << 24;
            card |= (uint64_t)hdr->card[4] << 32;
            card |= (uint64_t)hdr->card[5] << 40;
            card |= (uint64_t)hdr->card[6] << 48;
            card |= (uint64_t)hdr->card[7] << 56;
        } else {
            int invalid = 0;
            card = hllCount(hdr,&invalid);
            if (invalid) rl_error(L, "%s", invalid_hll_err);
            hdr->card[0] = card & 0xff;
            hdr->card[1] = (card >> 8) & 0xff;
            hdr->card[2] = (card >> 16) & 0xff;
            hdr->card[3] = (card >> 24) & 0xff;
            hdr->card[4] = (card >> 32) & 0xff;
            hdr->card[5] = (card >> 40) & 0xff;
            hdr->card[6] = (card >> 48) & 0xff;
            hdr->card[7] = (card >> 56) & 0xff;
        }
        rl_pushint(L,(long long)card);
        return 1;
    }
}

/* ---- PFMERGE ------------------------------------------------------------ */

/* PFMERGE dest src1 ... srcN => OK (pfmergeCommand, hyperloglog.c:1791).
 * 'dest' is argument 1 in both call shapes: hll.pfmerge(dest, srcs) and
 * dest:pfmerge(srcs). */
static int
lhll_pfmerge(lua_State *L) {
    rl_object *dest = rl_check(L, 1, RL_HLL);
    rl_object *src;
    rl_argiter it;
    uint8_t max[HLL_REGISTERS];
    struct hllhdr *hdr;
    robj tmp;
    int j, use_dense = 0;

    memset(max,0,sizeof(max));
    /* Upstream's merge loop runs from argv[1], which is the destination key, so a
     * PFMERGE carries the destination's own registers into the result as well --
     * PFMERGE dest dest is the documented no-op, and a destination that already
     * holds elements keeps them. Starting the iterator at the receiver is what
     * reproduces that here; the write below happens after every read. */
    rl_hll_iter(&it, L, 1);

    while ((src = rl_hll_next(&it)) != NULL) {
        rl_checkhll(L, src);
        rl_hll_view(&tmp, src);
        hdr = (struct hllhdr*)tmp.ptr;

        /* If at least one involved HLL is dense, use the dense representation
         * as target ASAP to save time and avoid the conversion step. */
        if (hdr->encoding == HLL_DENSE) use_dense = 1;

        if (hllMerge(max,&tmp) == C_ERR) {
            rl_error(L, "%s", invalid_hll_err);
        }
    }

    /* rl: upstream creates the destination key when missing; here the object
     * the command writes to is always the one passed in, and the loop above
     * already proved it is an HLL. */
    rl_hll_view(&tmp, dest);

    if (use_dense && hllSparseToDense(&tmp) == C_ERR) {
        dest->str = tmp.ptr;
        rl_error(L, "%s", invalid_hll_err);
    }

    if (use_dense) {
        dest->str = tmp.ptr;
        hdr = (struct hllhdr*)dest->str;
        hllDenseCompress(hdr->registers, max);
    } else {
        for (j = 0; j < HLL_REGISTERS; j++) {
            if (max[j] == 0) continue;
            hdr = (struct hllhdr*)dest->str;
            switch (hdr->encoding) {
                case HLL_DENSE: hllDenseSet(hdr->registers,j,max[j]); break;
                case HLL_SPARSE:
                    rl_hll_view(&tmp, dest);
                    hllSparseSet(&tmp,j,max[j]);
                    dest->str = tmp.ptr;
                    break;
            }
        }
    }
    hdr = (struct hllhdr*)dest->str; /* may be different now, as a side effect
                                        of the last hllSparseSet() call. */
    HLL_INVALIDATE_CACHE(hdr);

    rl_pushok(L);
    return 1;
}

/* ---- PFDEBUG ------------------------------------------------------------ */

/* PFDEBUG GETREG|DECODE|ENCODING|TODENSE|SIMD (pfdebugCommand,
 * hyperloglog.c:1995). GETREG and TODENSE convert sparse -> dense, like
 * upstream, so they do change the value. */
static int
lhll_pfdebug(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_HLL);
    const char *cmd = luaL_checkstring(L, 2);
    struct hllhdr *hdr;
    robj tmp;
    int top = lua_gettop(L);

    if (!strcasecmp(cmd,"simd")) {
        /* rl: neither HAVE_AVX2 nor HAVE_AARCH64_NEON is defined by this port,
         * so upstream's simd_enabled switch is compiled out and the reply is
         * always "disabled" -- the same answer a scalar Redis build gives.
         * Argument validation stays, so misuse is still reported. */
        if (top != 3)
            return rl_error(L,
                "Wrong number of arguments for the '%s' subcommand", cmd);
        if (!strcasecmp(luaL_checkstring(L, 3),"on")) {
        } else if (!strcasecmp(luaL_checkstring(L, 3),"off")) {
        } else {
            rl_error(L, "Argument must be ON or OFF");
        }
        lua_pushliteral(L, "disabled");
        return 1;
    }

    /* rl: "The specified key does not exist" is unreachable -- the caller must
     * hand over an hll object, and a wrong kind is rl_check()'s WRONGTYPE. */
    rl_checkhll(L, o);
    rl_hll_view(&tmp, o);

    if (!strcasecmp(cmd,"getreg")) {
        int j;
        if (top != 2)
            return rl_error(L,
                "Wrong number of arguments for the '%s' subcommand", cmd);

        hdr = (struct hllhdr*)tmp.ptr;
        if (hdr->encoding == HLL_SPARSE) {
            if (hllSparseToDense(&tmp) == C_ERR) {
                o->str = tmp.ptr;
                rl_error(L, "%s", invalid_hll_err);
            }
            o->str = tmp.ptr; /* Force the encoding change to stick. */
        }

        hdr = (struct hllhdr*)o->str;
        lua_createtable(L, HLL_REGISTERS, 0);
        for (j = 0; j < HLL_REGISTERS; j++) {
            uint8_t val;
            HLL_DENSE_GET_REGISTER(val,hdr->registers,j);
            rl_pushint(L,(long long)val);
            lua_rawseti(L,-2,j+1);
        }
        return 1;
    }
    /* PFDEBUG DECODE <key> */
    else if (!strcasecmp(cmd,"decode")) {
        uint8_t *p, *end;
        sds decoded;

        if (top != 2)
            return rl_error(L,
                "Wrong number of arguments for the '%s' subcommand", cmd);

        p = (uint8_t*)o->str;
        end = p + (size_t)sdslen(o->str);
        decoded = sdsempty();

        hdr = (struct hllhdr*)o->str;
        if (hdr->encoding != HLL_SPARSE) {
            sdsfree(decoded);
            rl_error(L, "HLL encoding is not sparse");
        }

        p += HLL_HDR_SIZE;
        while(p < end) {
            int runlen, regval;

            if (HLL_SPARSE_IS_ZERO(p)) {
                runlen = HLL_SPARSE_ZERO_LEN(p);
                p++;
                decoded = sdscatprintf(decoded,"z:%d ",runlen);
            } else if (HLL_SPARSE_IS_XZERO(p)) {
                runlen = HLL_SPARSE_XZERO_LEN(p);
                p += 2;
                decoded = sdscatprintf(decoded,"Z:%d ",runlen);
            } else {
                runlen = HLL_SPARSE_VAL_LEN(p);
                regval = HLL_SPARSE_VAL_VALUE(p);
                p++;
                decoded = sdscatprintf(decoded,"v:%d,%d ",regval,runlen);
            }
        }
        decoded = sdstrim(decoded," ");
        lua_pushlstring(L, decoded, sdslen(decoded));
        sdsfree(decoded);
        return 1;
    }
    /* PFDEBUG ENCODING <key> */
    else if (!strcasecmp(cmd,"encoding")) {
        static const char *encodingstr[2] = {"dense","sparse"};
        if (top != 2)
            return rl_error(L,
                "Wrong number of arguments for the '%s' subcommand", cmd);
        hdr = (struct hllhdr*)o->str;
        lua_pushstring(L, encodingstr[hdr->encoding]);
        return 1;
    }
    /* PFDEBUG TODENSE <key> */
    else if (!strcasecmp(cmd,"todense")) {
        int conv = 0;
        if (top != 2)
            return rl_error(L,
                "Wrong number of arguments for the '%s' subcommand", cmd);

        hdr = (struct hllhdr*)tmp.ptr;
        if (hdr->encoding == HLL_SPARSE) {
            if (hllSparseToDense(&tmp) == C_ERR) {
                o->str = tmp.ptr;
                rl_error(L, "%s", invalid_hll_err);
            }
            o->str = tmp.ptr;
            conv = 1;
        }
        rl_pushint(L, conv);
        return 1;
    } else {
        return rl_error(L,"Unknown PFDEBUG subcommand '%s'", cmd);
    }
}

/* ---- PFSELFTEST --------------------------------------------------------- */

/* PFSELFTEST => OK (pfselftestCommand, hyperloglog.c:1885). Verbatim apart
 * from the reply calls and the free path: both temporaries are owned by this
 * function, so 'cleanup' releases the sds and the robj view itself instead of
 * calling decrRefCount(). */
#define HLL_TEST_CYCLES 1000

static int
lhll_pfselftest(lua_State *L) {
    unsigned int j, i;
    sds bitcounters = sdsnewlen(NULL,HLL_DENSE_SIZE);
    struct hllhdr *hdr = (struct hllhdr*) bitcounters, *hdr2;
    robj *o = NULL;
    uint8_t bytecounters[HLL_REGISTERS];

    /* Test 1: access registers. */
    for (j = 0; j < HLL_TEST_CYCLES; j++) {
        for (i = 0; i < HLL_REGISTERS; i++) {
            unsigned int r = rand() & HLL_REGISTER_MAX;

            bytecounters[i] = r;
            HLL_DENSE_SET_REGISTER(hdr->registers,i,r);
        }
        for (i = 0; i < HLL_REGISTERS; i++) {
            unsigned int val;

            HLL_DENSE_GET_REGISTER(val,hdr->registers,i);
            if (val != bytecounters[i]) {
                sdsfree(bitcounters);
                /* The casts are upstream's own: addReplyErrorFormat() gets %d
                 * and (int) arguments, and HLL_REGISTER_MAX fits in an int. */
                return rl_error(L,
                    "TESTFAILED Register %d should be %d but is %d",
                    (int)i, (int)bytecounters[i], (int)val);
            }
        }
    }

    /* Test 2: approximation error, dense vs sparse agreement. */
    memset(hdr->registers,0,HLL_DENSE_SIZE-HLL_HDR_SIZE);
    o = createHLLObject();
    double relerr = 1.04/sqrt(HLL_REGISTERS);
    int64_t checkpoint = 1;
    uint64_t seed = (uint64_t)rand() | (uint64_t)rand() << 32;
    uint64_t ele;
    for (j = 1; j <= 10000000; j++) {
        ele = j ^ seed;
        hllDenseAdd(hdr->registers,(unsigned char*)&ele,sizeof(ele));
        hllAdd(o,(unsigned char*)&ele,sizeof(ele));

        if (j == (unsigned int)checkpoint &&
            (int64_t)j < (int64_t)(redislib_hll_sparse_max_bytes()/2)) {
            hdr2 = (struct hllhdr*)o->ptr;
            if (hdr2->encoding != HLL_SPARSE) {
                sdsfree(bitcounters); sdsfree(o->ptr); zfree(o);
                return rl_error(L, "TESTFAILED sparse encoding not used");
            }
        }

        if (j == (unsigned int)checkpoint &&
            hllCount(hdr,NULL) != hllCount((struct hllhdr*)o->ptr,NULL)) {
            sdsfree(bitcounters); sdsfree(o->ptr); zfree(o);
            return rl_error(L, "TESTFAILED dense/sparse disagree");
        }

        if (j == (unsigned int)checkpoint) {
            int64_t abserr = checkpoint - (int64_t)hllCount(hdr,NULL);
            uint64_t maxerr = (uint64_t)ceil(relerr*6*checkpoint);

            if (j == 10) maxerr = 1;

            if (abserr < 0) abserr = -abserr;
            if (abserr > (int64_t)maxerr) {
                sdsfree(bitcounters); sdsfree(o->ptr); zfree(o);
                return rl_error(L,
                    "TESTFAILED Too big error. card:%llu abserr:%llu",
                    (unsigned long long) checkpoint,
                    (unsigned long long) abserr);
            }
            checkpoint *= 10;
        }
    }

    sdsfree(bitcounters);
    sdsfree(o->ptr);
    zfree(o);
    rl_pushok(L);
    return 1;
}

/* ---- module functions --------------------------------------------------- */

/* hll.new() => an empty HLL, exactly the value a first PFADD creates. */
static int
lhll_new(lua_State *L) {
    robj *r = createHLLObject();
    rl_object *o;

    /* rl: the facade owns the sds, so the robj createHLLObject() returns is a
     * view whose payload is moved into the userdata and then discarded. */
    rl_newobject(L, RL_HLL);
    o = (rl_object*)lua_touserdata(L, -1);
    o->str = (sds)r->ptr;
    zfree(r);
    return 1;
}

static int
lhll_free(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_HLL);
    rl_free_ud(L, o);
    return 0;
}

/* ---- dump / load (spec §4, not Redis commands) --------------------------
 *
 * An HLL IS a Redis string whose bytes are the sketch, so dump() is those
 * bytes -- sparse or dense, as GET of such a key returns them -- and load() is
 * SET plus the check every HLL command makes before touching the value
 * (rl_checkhll(), the data side of isHLLObjectOrReply()). The object is built
 * first and validated second, so a rejected blob leaves nothing but a
 * collectible, still-empty handle. */
static int
lhll_dump(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_HLL);

    rl_pushvalue(L, o);
    return 1;
}

/* hll.load(string) => hll */
static int
lhll_load(lua_State *L) {
    rl_object *o;
    size_t len;
    const char *buf = rl_checkblob(L, 1, &len);

    rl_newobject(L, RL_HLL);
    o = (rl_object*)lua_touserdata(L, -1);
    o->str = sdsnewlen(buf, len);
    rl_checkhll(L, o);
    return 1;
}

static const luaL_Reg hll_methods[] = {
    {"pfadd", lhll_pfadd},
    {"pfcount", lhll_pfcount},
    {"pfmerge", lhll_pfmerge},
    {"pfdebug", lhll_pfdebug},
    {"pfselftest", lhll_pfselftest},
    {"dump", lhll_dump},
    {"free", lhll_free},
    {NULL, NULL},
};

static const luaL_Reg hll_module_methods[] = {
    {"new", lhll_new},
    {"load", lhll_load},
    {"pfcount", lhll_pfcount},
    {"pfmerge", lhll_pfmerge},
    {"pfselftest", lhll_pfselftest},
    {NULL, NULL},
};

/* LuaJIT's searcher uses the full module name with dots converted to
 * underscores (same rule as NetBull's luaopen_skiplist_c for "skiplist.c"). */
int luaopen_redis_hll(lua_State *L) {
    rl_register_class(L, RL_HLL, hll_methods, NULL);
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, hll_module_methods, 0);
    return 1;
}
