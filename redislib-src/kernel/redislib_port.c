/* redislib_port.c -- skynetjit redislib: implementation side of the port shim.
 *
 * Companion of redislib_port.h; not derived from Redis sources. Everything here
 * is listed in redislib-src/NOTICE ("shim 清单").
 */

/* monotonic.h first: it pulls fmacros.h, which must be seen before any system
 * header (that is the ordering every Redis kernel file uses). */
#include "monotonic.h"
#include "redislib_port.h"

#include <stdarg.h>

/* server.* globals read by the kernels; defaults taken from src/config.c. */
redislibServerConfig server = {
    3000, /* hll_sparse_max_bytes      -- config.c:3497 */
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

/* Abort stubs for the SET type API (src/t_set.c). The t_zset.c zunionstore /
 * zdiffstore helper block reaches them only when a command source object has
 * type OBJ_SET, which no redislib facade can produce: rl_object kinds are
 * bitmap / HLL / zset / geo. A stub that returns a plausible value would hide a
 * wiring bug, so these panic with the reason instead. See NOTICE. */
unsigned long setTypeSize(const robj *subject) {
    (void)subject;
    redislib_panic("redislib: SET type objects are not supported by the ported kernel");
}

int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    (void)set; (void)str; (void)len; (void)llval; (void)str_is_sds;
    redislib_panic("redislib: SET type objects are not supported by the ported kernel");
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

/* src/server.c:398 -- verbatim (setSdsDestructor, which also updates the dict
 * metadata size, is not ported: no ported dictType installs a metadata size). */
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

/* The three accessors above (redislib_port.h): the facades are separate shared
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
