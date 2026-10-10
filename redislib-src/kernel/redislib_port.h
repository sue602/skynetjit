/* redislib_port.h -- skynetjit redislib: minimal Redis portability shim.
 *
 * This is the only "kernel" file that is not derived from Redis sources: it
 * replaces the includes of Redis' server.h / zmalloc.h / redisassert.h /
 * atomicvar.h / object.h with the smallest set of definitions that the ported
 * algorithm kernels of this directory reference.
 *
 * Every symbol defined here is listed in redislib-src/NOTICE ("shim 清单").
 * Nothing in this file changes an algorithm: it only names the globals the
 * kernels already expected to come from server.h.
 */

#ifndef REDISLIB_PORT_H
#define REDISLIB_PORT_H

#include <assert.h>
#include <limits.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---------------------------------------------------------------------------
 * Allocator: Redis zmalloc family -> skynet memory API.
 * Same convention the repo already uses for luaclib modules
 * (luaclib-src/shared_header.h maps my_malloc to skynet_malloc).
 * ------------------------------------------------------------------------- */
#include "skynet_malloc.h"

#define zmalloc skynet_malloc
#define zrealloc skynet_realloc
#define zfree skynet_free

/* skynet_malloc.h has no size-only calloc, so zcalloc is malloc+memset; the
 * remaining zmalloc.h entry points the kernels call are thin wrappers
 * implemented in redislib_port.c. */
void *zcalloc(size_t size);
void *ztrycalloc(size_t size);
void *ztrymalloc(size_t size);
void *ztryrealloc(void *ptr, size_t size);
void *zmalloc_usable(size_t size, size_t *usable);
void *zcalloc_usable(size_t size, size_t *usable);
void *ztrymalloc_usable(size_t size, size_t *usable);
void *ztrycalloc_usable(size_t size, size_t *usable);
void *zrealloc_usable(void *ptr, size_t size, size_t *usable, size_t *old_usable);
void *ztryrealloc_usable(void *ptr, size_t size, size_t *usable, size_t *old_usable);
void zfree_usable(void *ptr, size_t *usable);
void zfree_with_size(void *ptr, size_t size);
size_t zmalloc_usable_size(void *ptr);

/* ---------------------------------------------------------------------------
 * Assertions, panic and logging (src/server.h:839-851, src/redisassert.h).
 * Debug assertions stay compiled out exactly like a non DEBUG_ASSERTIONS build.
 * ------------------------------------------------------------------------- */
#define serverAssert(_e) assert(_e)
#define serverAssertWithInfo(_c,_o,_e) assert(_e)
#define serverPanic(...) redislib_panic(__VA_ARGS__)
#define serverLog(...) ((void)0)
#define debugAssert(_e) ((void)0)
#define debugServerAssert(...) ((void)0)
#define debugServerAssertWithInfo(...) ((void)0)

/* serverPanic() never returns upstream (redisassert.h declares it _Noreturn),
 * and several kernel functions rely on that to end their last branch with a
 * panic instead of a return, so the attribute is part of the contract. */
#if defined(__GNUC__)
#define REDISLIB_NORETURN __attribute__((noreturn))
#elif defined(_MSC_VER)
#define REDISLIB_NORETURN __declspec(noreturn)
#else
#define REDISLIB_NORETURN
#endif

REDISLIB_NORETURN void redislib_panic(const char *msg, ...);

/* ---------------------------------------------------------------------------
 * Atomic counters (verbatim from src/atomicvar.h, "atomic-builtin" branch).
 * src/atomicvar.h itself is not part of the port because it includes
 * <pthread.h> unconditionally.
 * ------------------------------------------------------------------------- */
#define redisAtomic
#define atomicIncr(var,count) __atomic_add_fetch(&(var),(count),__ATOMIC_RELAXED)
#define atomicIncrGet(var, newvalue_var, count) \
    newvalue_var = __atomic_add_fetch(&var,(count),__ATOMIC_RELAXED)
#define atomicGetIncr(var,oldvalue_var,count) do { \
    oldvalue_var = __atomic_fetch_add(&var,(count),__ATOMIC_RELAXED); \
} while(0)
#define atomicDecr(var,count) __atomic_sub_fetch(&var,(count),__ATOMIC_RELAXED)
#define atomicGet(var,dstvar) do { \
    dstvar = __atomic_load_n(&var,__ATOMIC_RELAXED); \
} while(0)
#define atomicSet(var,value) __atomic_store_n(&var,value,__ATOMIC_RELAXED)
#define atomicGetWithSync(var,dstvar) do { \
    dstvar = __atomic_load_n(&var,__ATOMIC_SEQ_CST); \
} while(0)
#define atomicSetWithSync(var,value) \
    __atomic_store_n(&var,value,__ATOMIC_SEQ_CST)

/* ---------------------------------------------------------------------------
 * Include-compatibility layer: the leaf headers server.h used to pull in for
 * these kernels. All of them are part of the port (see redislib-src/NOTICE).
 * fmacros.h and monotonic.h are deliberately NOT included here: kernel files
 * that need them include them first (fmacros.h must precede the system
 * headers, and pulling monotonic.h in late would re-define _DEFAULT_SOURCE).
 * ------------------------------------------------------------------------- */
#include "config.h"
#include "endianconv.h"
#include "sds.h"
#include "dict.h"
#include "adlist.h"
#include "util.h"
#include "listpack.h"

/* ---------------------------------------------------------------------------
 * Minimal robj. Upstream declares it in src/object.h:102 with type/encoding
 * nibbles, a refcount, kvobj metadata and the payload pointer. The ported
 * kernels read o->ptr, and t_zset.c / bitops.c additionally branch on
 * o->type and o->encoding, so those three members are ported; the refcount and
 * the metadata bits are not: a redislib object is owned by the Lua facade that
 * created it (see redislib-src/NOTICE).
 * ------------------------------------------------------------------------- */
struct robj {
    unsigned type:4;      /* OBJ_* constant, src/server.h */
    unsigned encoding:4;  /* OBJ_ENCODING_* constant */
    void *ptr;            /* sds / zset* / listpack payload */
};
typedef struct robj robj;

/* src/object.c:107 createObject() stand-in: allocates a robj wrapping 'ptr',
 * with the same type/encoding defaults as upstream.
 * Declared here, implemented in redislib_port.c. */
robj *createObject(int type, void *ptr);

/* src/object.c:492 and :505: the zset object factories t_zset.c calls. They
 * live in object.c upstream (which is server core, not part of this port), so
 * their bodies are ported verbatim into redislib_port.c. */
robj *createZsetObject(void);
robj *createZsetListpackObject(void);

/* src/server.h:3195: the dictType of a skiplist-encoded zset. Defined by the
 * ported t_zset.c, referenced by the object factories above. */
extern dictType zsetDictType;

/* src/t_set.c:483 and :303: the SET type API. SET objects are not part of this
 * port (a redislib object is a bitmap, an HLL, or a zset), but the zunion/
 * zdiff helper block of t_zset.c still references them, so redislib_port.c
 * provides deliberate abort stubs. */
unsigned long setTypeSize(const robj *subject);
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);

/* Object type and encoding constants, copied verbatim from
 * src/server.h:875-901 and src/object.h:75-90 (values must not drift). */
#define OBJ_STRING 0    /* String object. */
#define OBJ_LIST 1      /* List object. */
#define OBJ_SET 2       /* Set object. */
#define OBJ_ZSET 3      /* Sorted set object. */
#define OBJ_HASH 4      /* Hash object. */
#define OBJ_TYPE_BASIC_MAX 5 /* Max number of basic object types. */
#define OBJ_MODULE 5    /* Module object. */
#define OBJ_STREAM 6    /* Stream object. */
#define OBJ_ARRAY 7     /* Array object. */
#define OBJ_GCRA 8      /* GCRA object. */

#define OBJ_ENCODING_RAW 0     /* Raw representation */
#define OBJ_ENCODING_INT 1     /* Encoded as integer */
#define OBJ_ENCODING_HT 2      /* Encoded as hash table */
#define OBJ_ENCODING_ZIPMAP 3  /* No longer used: old hash encoding. */
#define OBJ_ENCODING_LINKEDLIST 4 /* No longer used: old list encoding. */
#define OBJ_ENCODING_ZIPLIST 5 /* No longer used: old list/hash/zset encoding. */
#define OBJ_ENCODING_INTSET 6  /* Encoded as intset */
#define OBJ_ENCODING_SKIPLIST 7  /* Encoded as skiplist */
#define OBJ_ENCODING_EMBSTR 8  /* Embedded sds string encoding */
#define OBJ_ENCODING_QUICKLIST 9 /* Encoded as linked list of listpacks */
#define OBJ_ENCODING_STREAM 10 /* Encoded as a radix tree of listpacks */
#define OBJ_ENCODING_LISTPACK 11 /* Encoded as a listpack */
#define OBJ_ENCODING_LISTPACK_EX 12 /* Encoded as listpack, extended with metadata */
#define OBJ_ENCODING_SLICED_ARRAY 13 /* Encoded as sliced array */

/* src/server.h:114-115, :106-107, :654, :656-657. The UNUSED spelling is the
 * one util.c:57 uses, so that file's own definition stays a no-op.
 * min/max are undefined first: the Windows compat headers pull windows.h into
 * the kernel translation units, and minwindef.h already spells them. */
#define C_OK 0
#define C_ERR -1
#ifdef min
#undef min
#endif
#define min(a, b) ((a) < (b) ? (a) : (b))
#ifdef max
#undef max
#endif
#define max(a, b) ((a) > (b) ? (a) : (b))
#define UNUSED(x) ((void)(x))
#define ZSKIPLIST_MAXLEVEL 32 /* Should be enough for 2^64 elements */
#define ZSKIPLIST_P 0.25      /* Skiplist P = 1/4 */
#define ZSKIPLIST_MAX_SEARCH 10 /* server.h:658, read by zslGetRankByNode() */

/* Defined by util.c (getRandomBytes), declared by server.h:3297. */
void getRandomBytes(unsigned char *p, size_t len);

/* ---------------------------------------------------------------------------
 * dictType callbacks that upstream lives in src/server.c (i.e. in the server
 * core, not in a kernel file), so the kernels that install them cannot see
 * them. Bodies are the server.c ones, ported verbatim into redislib_port.c:
 *   dictSdsHash        server.c:435
 *   dictSdsCaseHash    server.c:439
 *   dictSdsKeyCompare  server.c:370
 *   dictSdsDestructor  server.c:398
 *   dictSdsDup         server.c:418
 * ------------------------------------------------------------------------- */
uint64_t dictSdsHash(const void *key);
uint64_t dictSdsCaseHash(const void *key);
int dictSdsKeyCompare(dictCmpCache *cache, const void *key1, const void *key2);
void dictSdsDestructor(dict *d, void *val);
void *dictSdsDup(dict *d, const void *key);

/* src/server.c:690 and :700: the two dictTypes the ZRANDMEMBER sampling
 * strategies need (they live in the server core upstream, so a facade cannot
 * see them). Both are the upstream definitions, verbatim. */
extern dictType hashDictType;
extern dictType sdsReplyDictType;

/* ---------------------------------------------------------------------------
 * Redis configuration globals the kernels read. Defaults are the ones from
 * src/config.c createXXXConfig() lines, so behaviour matches a stock Redis.
 * ------------------------------------------------------------------------- */
typedef struct redislibServerConfig {
    size_t hll_sparse_max_bytes;      /* config.c:3497 "hll-sparse-max-bytes" */
    size_t zset_max_listpack_entries; /* config.c:3492 "zset-max-listpack-entries" */
    size_t zset_max_listpack_value;   /* config.c:3496 "zset-max-listpack-value" */
    int memory_tracking_enabled;      /* slot/keysizes stats: off by default */
} redislibServerConfig;

extern redislibServerConfig server;

/* shared.minstring / shared.maxstring are the sentinel SDS objects that
 * zslParseLexRangeItem() stores for unbounded lexicographic range edges
 * (created by createSharedObjects() in server.c). */
typedef struct redislibSharedStruct {
    sds minstring;
    sds maxstring;
} redislibSharedStruct;

extern redislibSharedStruct shared;

/* ---------------------------------------------------------------------------
 * random()/srandom(): glibc and musl provide them, the MSVCRT used by
 * MinGW-w64 does not. Redis uses them only for skiplist level selection and
 * dict sampling, so a non-cryptographic xorshift64* is an adequate stand-in
 * with the same [0, RAND_MAX] range contract.
 * ------------------------------------------------------------------------- */
#if defined(_WIN32)
long redislib_random(void);
void redislib_srandom(unsigned int seed);
/* compat/win64/compat.h maps random/srandom to rand/srand; the shim above has
 * the same [0, RAND_MAX] contract with a wider state, so it must win. */
#ifdef random
#undef random
#endif
#define random() redislib_random()
#ifdef srandom
#undef srandom
#endif
#define srandom(s) redislib_srandom(s)

/* util.c's directory/file helpers (dirCreateIfMissing, dirRemove, the
 * redis_fsync call sites in the anonTry helpers) are dead code for redislib: no
 * facade and no kernel entry point performs filesystem I/O. These mappings exist
 * purely so the verbatim kernel files compile with MinGW-w64, whose io.h
 * declares 1-arg _mkdir, has no O_NONBLOCK and names fsync _commit.
 * Range/semantic differences vs POSIX are therefore unreachable. */
#include <direct.h>   /* _mkdir on MinGW-w64 */
#define mkdir(path, mode) _mkdir(path)
#ifndef O_NONBLOCK
#define O_NONBLOCK 0
#endif
#define fsync _commit
#endif

/* Sorted-set / skiplist types and kernel API, extracted verbatim from
 * src/server.h (see redislib-src/NOTICE). Needs robj, sds and dict above. */
#include "t_zset.h"

/* One-time process setup, run automatically from a library constructor and
 * callable from the facades: initializes the monotonic clock used by
 * elapsedStart()/elapsedUs(), seeds dict's siphash key, creates the shared
 * sentinel SDS objects. */
void redislib_init(void);

#endif /* REDISLIB_PORT_H */
