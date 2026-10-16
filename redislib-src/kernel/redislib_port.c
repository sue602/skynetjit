/* redislib_port.c -- skynetjit redislib: implementation side of the port shim.
 *
 * Companion of redislib_port.h; not derived from Redis sources. Everything here
 * is listed in redislib-src/NOTICE ("shim 清单").
 */

/* monotonic.h first: it pulls fmacros.h, which must be seen before any system
 * header (that is the ordering every Redis kernel file uses). */
#include "monotonic.h"
#include "redislib_port.h"
#include "intset.h"  /* intsetNew(), for the createIntsetObject() factory below */

#include <stdarg.h>

/* server.* globals read by the kernels; defaults taken from src/config.c. */
redislibServerConfig server = {
    3000, /* hll_sparse_max_bytes      -- config.c:3497 */
    512,  /* set_max_intset_entries    -- config.c:3489 */
    128,  /* set_max_listpack_entries  -- config.c:3490 */
    64,   /* set_max_listpack_value    -- config.c:3491 */
    128,  /* zset_max_listpack_entries -- config.c:3492 */
    64,   /* zset_max_listpack_value   -- config.c:3496 */
    0,    /* memory_tracking_enabled   -- off, as in a stock Redis build */
};

/* Created by redislib_init(), mirroring createSharedObjects() in server.c. */
redislibSharedStruct shared = { NULL, NULL };

/* ---------------- allocator wrappers (see redislib_port.h) -------------- */

void *zcalloc(size_t size) {
    void *p = skynet_malloc(size);
    if (p) memset(p, 0, size);
    return p;
}

void *ztrycalloc(size_t size) { return zcalloc(size); }

void *ztrymalloc(size_t size) { return skynet_malloc(size); }

void *ztryrealloc(void *ptr, size_t size) { return skynet_realloc(ptr, size); }

/* The skynet allocator API exposes no usable-size introspection, so the
 * requested size is reported as the usable size. SDS only concludes that there
 * is no slack, i.e. behaviour stays correct with a few more reallocs. */
void *zmalloc_usable(size_t size, size_t *usable) {
    void *p = skynet_malloc(size);
    if (usable) *usable = p ? size : 0;
    return p;
}

void *zcalloc_usable(size_t size, size_t *usable) {
    void *p = zcalloc(size);
    if (usable) *usable = p ? size : 0;
    return p;
}

void *ztrymalloc_usable(size_t size, size_t *usable) {
    return zmalloc_usable(size, usable);
}

void *ztrycalloc_usable(size_t size, size_t *usable) {
    return zcalloc_usable(size, usable);
}

void *zrealloc_usable(void *ptr, size_t size, size_t *usable, size_t *old_usable) {
    void *p = skynet_realloc(ptr, size);
    if (usable) *usable = p ? size : 0;
    if (old_usable) *old_usable = 0;
    return p;
}

void *ztryrealloc_usable(void *ptr, size_t size, size_t *usable, size_t *old_usable) {
    return zrealloc_usable(ptr, size, usable, old_usable);
}

void zfree_usable(void *ptr, size_t *usable) {
    if (usable) *usable = 0;
    skynet_free(ptr);
}

void zfree_with_size(void *ptr, size_t size) {
    (void)size;
    skynet_free(ptr);
}

size_t zmalloc_usable_size(void *ptr) {
    (void)ptr;
    return 0;
}

/* ---------------- object, dict helpers, panic, init ---------------------- */

/* Minimal stand-in for object.c:107 createObject(): the ported kernels use the
 * returned object through o->ptr, o->type and o->encoding only (see NOTICE).
 * Upstream also initialises refcount/lru/metabits, which this port has no
 * equivalent for -- object ownership stays with the facade. */
robj *createObject(int type, void *ptr) {
    robj *o = skynet_malloc(sizeof(*o));
    if (o == NULL) return NULL;
    o->type = type;
    o->encoding = OBJ_ENCODING_RAW;
    o->ptr = ptr;
    return o;
}

/* src/object.c:492 -- verbatim. */
robj *createZsetObject(void) {
    zset *zs = zmalloc(sizeof(*zs));
    robj *o;

    zs->dict = dictCreate(&zsetDictType);
    zs->zsl = zslCreate();
    o = createObject(OBJ_ZSET,zs);
    o->encoding = OBJ_ENCODING_SKIPLIST;
    return o;
}

/* src/object.c:505 -- verbatim. */
robj *createZsetListpackObject(void) {
    unsigned char *lp = lpNew(0);
    robj *o = createObject(OBJ_ZSET,lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

/* src/object.c:464, :471, :478 -- verbatim. The three set payload factories
 * t_set.c's setTypeCreate() chooses between. */
robj *createSetObject(void) {
    dict *d = dictCreate(&setDictType);
    robj *o = createObject(OBJ_SET,d);
    o->encoding = OBJ_ENCODING_HT;
    return o;
}

robj *createIntsetObject(void) {
    intset *is = intsetNew();
    robj *o = createObject(OBJ_SET,is);
    o->encoding = OBJ_ENCODING_INTSET;
    return o;
}

robj *createSetListpackObject(void) {
    unsigned char *lp = lpNew(0);
    robj *o = createObject(OBJ_SET, lp);
    o->encoding = OBJ_ENCODING_LISTPACK;
    return o;
}

/* src/object.c:566 freeSetObject() -- verbatim, less the DEBUG_ASSERTIONS block
 * (this port is a non-DEBUG_ASSERTIONS build, which is what the shim's
 * debugServerAssert() mapping already says; see redislib_port.h). It releases
 * the payload only, never the robj: t_set.c relies on replacing a set in place. */
void freeSetObject(robj *o) {
    switch (o->encoding) {
    case OBJ_ENCODING_HT:
        dictRelease((dict*) o->ptr);
        break;
    case OBJ_ENCODING_INTSET:
    case OBJ_ENCODING_LISTPACK:
        zfree(o->ptr);
        break;
    default:
        serverPanic("Unknown set encoding type");
    }
}

/* src/object.c:906 -- verbatim. */
int isSdsRepresentableAsLongLong(sds s, long long *llval) {
    return string2ll(s,sdslen(s),llval) ? C_OK : C_ERR;
}

/* src/server.c:435 -- verbatim. */
uint64_t dictSdsHash(const void *key) {
    return dictGenHashFunction((unsigned char*)key, sdslen((char*)key));
}

/* src/server.c:439 -- verbatim. */
uint64_t dictSdsCaseHash(const void *key) {
    return dictGenCaseHashFunction((unsigned char*)key, sdslen((char*)key));
}

/* src/server.c:370 -- verbatim. */
int dictSdsKeyCompare(dictCmpCache *cache, const void *key1, const void *key2)
{
    int l1,l2;
    UNUSED(cache);

    l1 = sdslen((sds)key1);
    l2 = sdslen((sds)key2);
    if (l1 != l2) return 0;
    return memcmp(key1, key2, l1) == 0;
}

/* src/server.c:398 -- verbatim. (setSdsDestructor, which also updates the dict
 * metadata size, is below with setDictType: from this file on, a ported dictType
 * does install a metadata size -- the set one does.) */
void dictSdsDestructor(dict *d, void *val)
{
    UNUSED(d);
    sdsfree(val);
}

/* src/server.c:418 -- verbatim. */
void *dictSdsDup(dict *d, const void *key) {
    UNUSED(d);
    return sdsdup((const sds) key);
}

/* src/server.c:404 and :409 -- verbatim. The two callbacks of setDictType: a set
 * dict keeps the sum of its element bytes in the dict's metadata slot, which is
 * what t_set.c's htGetMetadataSize() writes at its 151 / 182 / 228 / 500 / 544
 * and what setTypeAllocSize() reports. */
void setSdsDestructor(dict *d, void *val) {
    *htGetMetadataSize(d) -= sdsAllocSize(val);
    sdsfree(val);
}

size_t setDictMetadataBytes(dict *d) {
    UNUSED(d);
    return sizeof(size_t);
}

/* src/server.c:633 -- verbatim, and the reason a ported dictType now installs a
 * metadata size. Unlike hashDictType / sdsReplyDictType this one is NOT static:
 * t_set.c:530 (kept, verbatim) does `dictCreate(&setDictType)`, and a kernel
 * file may not be edited beyond the three mechanical kinds, so the symbol has to
 * be visible to it. That is safe because t_set.c and this file are both inside
 * rlcore -- the data symbol never crosses a DLL boundary. A facade must not take
 * its address either way: it creates sets through createSetObject() and friends,
 * which are functions. Same shape as zsetDictType, which the ported t_zset.c
 * defines and createZsetObject() uses. */
dictType setDictType = {
    dictSdsHash,               /* hash function */
    NULL,                      /* key dup */
    NULL,                      /* val dup */
    dictSdsKeyCompare,         /* key compare */
    setSdsDestructor,          /* key destructor */
    NULL,                      /* val destructor */
    NULL,                      /* allow to expand */
    .no_value = 1,             /* no values in this dict */
    .keys_are_odd = 1,         /* an SDS string is always an odd pointer */
    .dictMetadataBytes = setDictMetadataBytes,
};

/* src/server.c:690 -- verbatim. ZRANDMEMBER's CASE 4 staging dict owns the
 * copies of the sampled members, so both destructors are the sds ones.
 * static: reached from facade/sortedset.c through redislib_hash_dict_type(). */
static dictType hashDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    dictSdsDestructor,          /* key destructor */
    dictSdsDestructor,          /* val destructor */
    NULL,                       /* allow to expand */
};

/* src/server.c:700 -- verbatim. ZRANDMEMBER's CASE 3 staging dict hands its
 * keys straight to the reply, so it must not free them itself; the facade that
 * uses it here keeps them alive until it has pushed each one to Lua and frees
 * them itself (see facade/sortedset.c).
 * static: reached through redislib_sds_reply_dict_type() below. */
static dictType sdsReplyDictType = {
    dictSdsHash,                /* hash function */
    NULL,                       /* key dup */
    NULL,                       /* val dup */
    dictSdsKeyCompare,          /* key compare */
    NULL,                       /* key destructor */
    NULL,                       /* val destructor */
    NULL                        /* allow to expand */
};

/* The accessors above (redislib_port.h): the facades are separate shared
 * objects from this library, and a data symbol imported across a Windows DLL
 * boundary is resolved by the linker's auto-import rewriting while a function
 * call is resolved by the import table itself. Nothing here changes a value --
 * they return the address of the same static object, or read the same global. */
dictType *redislib_hash_dict_type(void) { return &hashDictType; }

dictType *redislib_sds_reply_dict_type(void) { return &sdsReplyDictType; }

size_t redislib_hll_sparse_max_bytes(void) { return server.hll_sparse_max_bytes; }

REDISLIB_NORETURN void redislib_panic(const char *msg, ...) {
    va_list ap;
    va_start(ap, msg);
    fprintf(stderr, "redislib panic: ");
    vfprintf(stderr, msg, ap);
    fprintf(stderr, "\n");
    va_end(ap);
    abort();
}

void redislib_init(void) {
    static int initialized = 0;
    if (initialized) return;
    initialized = 1;

    /* monotonic.h hands out time through the getMonotonicUs function pointer,
     * which dict.c's latency stats use through elapsedStart()/elapsedUs(). */
    monotonicInit();

    /* Same as server.c main(): a 16 byte siphash key for dict. getRandomBytes()
     * reads /dev/urandom and falls back to clock/pid/pointer entropy, so the
     * seed is unpredictable without ever blocking (dict is not crypto). */
    if (shared.minstring == NULL) {
        shared.minstring = sdsnew("minstring");
        shared.maxstring = sdsnew("maxstring");
    }
    uint8_t hashseed[16];
    getRandomBytes(hashseed, sizeof(hashseed));
    dictSetHashFunctionSeed(hashseed);
}

#if defined(__GNUC__)
__attribute__((constructor)) static void redislib_constructor(void) {
    redislib_init();
}
#endif

/* ---------------- random()/srandom() for MinGW-w64 --------------------- */

#if defined(_WIN32)
/* xorshift64* (V6a), non-cryptographic: MSVCRT has no random()/srandom() and
 * Redis only uses them for skiplist levels and dict sampling. */
static unsigned long long redislib_rng = 0x853c49e6748fea9bULL;

long redislib_random(void) {
    unsigned long long x = redislib_rng;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    redislib_rng = x;
    return (long)((x * 2685821657736338717ULL) %
                  ((unsigned long long)RAND_MAX + 1));
}

void redislib_srandom(unsigned int seed) {
    redislib_rng = seed ?
        ((unsigned long long)seed * 6364136223846793005ULL + 1) :
        0x853c49e6748fea9bULL;
}
#endif
