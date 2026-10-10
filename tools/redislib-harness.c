/* redislib-harness.c -- pure-C self check of the ported Redis 8.10.0 kernels.
 *
 * Purpose: prove that the "command layer removal" surgery in
 * redislib-src/kernel/ did not break the algorithms. Every assertion below
 * checks a concrete value (Redis' own documented vectors or an invariant), not
 * just that a call returns.
 *
 * Build & run (Linux / WSL):  tools/run-harness-wsl.sh
 *
 * Not linked into any .so; not part of the shipped library.
 */

#include <assert.h>
#include <limits.h>
#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "redislib_port.h"
#include "geohash.h"
#include "geohash_helper.h"
#include "hyperloglog.h"
#include "intset.h"

/* crc16() is declared in Redis' server.h, which is not part of this port. */
uint16_t crc16(const char *buf, int len);

/* hyperloglog.h (as shipped by Redis) exports only createHLLObject/hllAdd/
 * hllCount; the rest of the kernel API lives in server.h. Declare what the
 * harness exercises so the linker resolves them from hyperloglog.c. */
int hllSparseToDense(robj *o);
int hllMerge(uint8_t *max, robj *hll);

/* bitops.c exports the same way (server.h declares them upstream, and server.h
 * is not part of this port). */
long long redisPopcount(void *s, long count);
long long redisBitpos(void *s, unsigned long count, int bit);
void setUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, uint64_t value);
uint64_t getUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits);
void setSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, int64_t value);
int64_t getSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits);
int checkUnsignedBitfieldOverflow(uint64_t value, int64_t incr, uint64_t bits,
                                 int owtype, uint64_t *limit);
int checkSignedBitfieldOverflow(int64_t value, int64_t incr, uint64_t bits,
                               int owtype, int64_t *limit);
/* bitops.c:576-578; not visible through any header. */
#define BFOVERFLOW_WRAP 0
#define BFOVERFLOW_SAT 1

/* struct hllhdr and the encoding constants are defined inside hyperloglog.c
 * (line 183 ff.). Mirrored here so the harness can read the header of the SDS
 * payload; sizeof() is asserted against HLL_HDR_SIZE to catch drift. */
#define HLL_HDR_SIZE 16
#define HLL_DENSE 0  /* Dense encoding. */
#define HLL_SPARSE 1 /* Sparse encoding. */
struct hllhdr {
    char magic[4];      /* "HYLL" */
    uint8_t encoding;   /* HLL_DENSE or HLL_SPARSE. */
    uint8_t notused[3]; /* Reserved for future use, must be zero. */
    uint8_t card[8];    /* Cached cardinality, little endian. */
    uint8_t registers[]; /* Data bytes. */
};

/* Derived exactly like HLL_REGISTERS/HLL_BITS/HLL_DENSE_SIZE in
 * hyperloglog.c: 16384 registers x 6 bits + the 16 byte header. */
#define HLL_REGISTERS 16384
#define HLL_BITS 6
#define HLL_DENSE_SIZE (HLL_HDR_SIZE + ((HLL_REGISTERS * HLL_BITS + 7) / 8))

/* hllAdd()/hllSparseToDense() may move the SDS payload, so the header must be
 * re-read from o->ptr after every mutation instead of being cached. */
#define HLLHDR(o) ((struct hllhdr *)(o)->ptr)

/* ---------------- geohash / geohash_helper ------------------------------ */

static void test_geohash(void) {
    /* The Beijing coordinate used as the sample point for the georadius docs. */
    const double lon = 116.4898586, lat = 39.9167148;
    GeoHashBits gh;
    double xy[2];

    memset(&gh, 0, sizeof(gh));
    /* Redis' geohash encode/decode return 1 on success, 0 on failure. */
    assert(geohashEncodeWGS84(lon, lat, GEO_STEP_MAX, &gh) == 1);
    assert(gh.step == GEO_STEP_MAX);
    assert(gh.bits != 0);

    /* Full 52 bit precision must decode back to within 1e-5 degrees. */
    assert(geohashDecodeToLongLatWGS84(gh, xy) == 1);
    assert(fabs(xy[0] - lon) < 1e-5);
    assert(fabs(xy[1] - lat) < 1e-5);

    /* Coarser step: only 2*step bits are used and the decoded cell centre is
     * still close to the encoded point. */
    GeoHashBits gh5;
    memset(&gh5, 0, sizeof(gh5));
    assert(geohashEncodeWGS84(lon, lat, 5, &gh5) == 1);
    assert(gh5.bits < (1ULL << 10));
    assert(geohashDecodeToLongLatWGS84(gh5, xy) == 1);
    assert(fabs(xy[0] - lon) < 5.0 && fabs(xy[1] - lat) < 5.0);

    /* Area decode reports a cell that contains the original point. */
    GeoHashArea area;
    assert(geohashDecodeWGS84(gh, &area) == 1);
    assert(area.longitude.min <= lon && lon <= area.longitude.max);
    assert(area.latitude.min <= lat && lat <= area.latitude.max);

    /* Distance helper: identical points are 0 m apart and the metric is
     * symmetric (haversine with the WGS84 radii). */
    assert(geohashGetDistance(lon, lat, lon, lat) == 0.0);
    double d1 = geohashGetDistance(lon, lat, 116.49, 39.92);
    double d2 = geohashGetDistance(116.49, 39.92, lon, lat);
    assert(d1 > 100.0 && d1 < 10000.0);
    assert(fabs(d1 - d2) < 1e-9);

    double dist = -1;
    assert(geohashGetDistanceIfInRadiusWGS84(lon, lat, lon, lat, 1000, &dist) == 1);
    assert(dist == 0.0);
    assert(geohashGetDistanceIfInRadiusWGS84(lon, lat, lon + 1.0, lat, 1000, &dist) == 0);

    /* geohashAlign52Bits keeps the 52 bit payload only. */
    assert((geohashAlign52Bits(gh) & ~((1ULL << 52) - 1)) == 0);

    /* Steps chosen from a radius: Redis caps at GEO_STEP_MAX. */
    assert(geohashEstimateStepsByRadius(1000000, lat) <= GEO_STEP_MAX);
    assert(geohashEstimateStepsByRadius(1, lat) >= 1);
}

/* ---------------- hyperloglog ------------------------------------------- */

static robj *mkhll(void) {
    robj *o = createHLLObject();
    assert(o != NULL);
    struct hllhdr *hdr = (struct hllhdr *)o->ptr;
    assert(memcmp(hdr->magic, "HYLL", 4) == 0);
    assert(hdr->encoding == HLL_SPARSE);
    assert(sizeof(struct hllhdr) == HLL_HDR_SIZE);
    return o;
}

static int hllAddRange(robj *o, int from, int to) {
    char buf[32];
    int updates = 0;
    for (int i = from; i < to; i++) {
        int n = (int)snprintf(buf, sizeof buf, "e%d", i);
        int updated = hllAdd(o, (unsigned char *)buf, (size_t)n);
        assert(updated == 0 || updated == 1);   /* -1 == corrupted HLL */
        updates += updated;
    }
    return updates;
}

static void test_hll(void) {
    robj *o = mkhll();

    /* A freshly created HLL counts zero. */
    assert(hllCount(HLLHDR(o), NULL) == 0);

    /* 10000 distinct elements: the estimated cardinality must stay inside the
     * 2% band the plan asks for (Redis' own standard error is ~0.81%). */
    int updates = hllAddRange(o, 0, 10000);
    uint64_t card = hllCount(HLLHDR(o), NULL);
    assert(card > 9800 && card < 10200);
    /* PFADD return semantics: an element that changes no register reports 0, so
     * the number of "1" answers is below the element count, yet most of them
     * still move a register on the way up. */
    assert(updates > 3000);

    /* Re-adding an element that is already in the set reports 0. */
    assert(hllAdd(o, (unsigned char *)"e0", 2) == 0);

    /* 10000 registers blow past hll_sparse_max_bytes (3000), so the kernel must
     * have promoted the object to the dense form, whose payload is exactly
     * HLL_DENSE_SIZE bytes. */
    assert(HLLHDR(o)->encoding == HLL_DENSE);
    assert(sdslen(o->ptr) == HLL_DENSE_SIZE);

    /* hllMerge() of a subset must be element-wise <= hllMerge() of the whole
     * set, and both must touch registers. */
    robj *sub = mkhll();
    hllAddRange(sub, 0, 5000);
    uint8_t max_all[HLL_REGISTERS], max_sub[HLL_REGISTERS];
    memset(max_all, 0, sizeof max_all);
    memset(max_sub, 0, sizeof max_sub);
    assert(hllMerge(max_all, o) == C_OK);
    assert(hllMerge(max_sub, sub) == C_OK);
    int touched = 0;
    for (int i = 0; i < HLL_REGISTERS; i++) {
        assert(max_sub[i] <= max_all[i]);
        assert(max_all[i] <= 63);   /* HLL_BITS wide register */
        touched += max_all[i] > 0;
    }
    assert(touched > 4000);
    zfree(sub);
    zfree(o);

    /* sparse -> dense on a small object: cardinality is invariant under the
     * conversion, and the conversion is idempotent. */
    robj *s = mkhll();
    assert(hllAdd(s, (unsigned char *)"alpha", 5) == 1);
    assert(hllAdd(s, (unsigned char *)"beta", 4) == 1);
    assert(hllAdd(s, (unsigned char *)"gamma", 5) == 1);
    assert(HLLHDR(s)->encoding == HLL_SPARSE);
    assert(sdslen(s->ptr) < HLL_DENSE_SIZE);   /* still run-length encoded */
    uint64_t before = hllCount(HLLHDR(s), NULL);
    assert(before == 3);
    assert(hllSparseToDense(s) == C_OK);
    assert(HLLHDR(s)->encoding == HLL_DENSE);
    assert(sdslen(s->ptr) == HLL_DENSE_SIZE);
    assert(hllCount(HLLHDR(s), NULL) == before);
    assert(hllSparseToDense(s) == C_OK);  /* already dense: no-op, still C_OK */
    zfree(s);
}

/* ---------------- dict (siphash / rehash / chains) ----------------------- */

/* Keys are stored as sds, but dictFind()/dictDelete() below are called with
 * plain C string literals. sds content is NUL terminated and none of these
 * keys embed a NUL, so going through strlen/strcmp gives one canonical form for
 * both spellings of a key. Using sdslen()/sdscmp() here would read the sds
 * header of a string literal, which is undefined memory. */
static uint64_t harnessHash(const void *key) {
    const char *s = (const char *)key;
    return dictGenHashFunction((const unsigned char *)s, strlen(s));
}

static int harnessCompare(dictCmpCache *cache, const void *key1, const void *key2) {
    (void)cache;
    return strcmp((const char *)key1, (const char *)key2) == 0;
}

static void harnessKeyDestructor(dict *d, void *key) {
    (void)d;
    sdsfree((sds)key);
}

/* The first six members of dictType, in declaration order (dict.h:92). */
static dictType harnessDictType = {
    harnessHash,             /* hashFunction  */
    NULL,                    /* keyDup        */
    NULL,                    /* valDup        */
    harnessCompare,          /* keyCompare    */
    harnessKeyDestructor,    /* keyDestructor */
    NULL,                    /* valDestructor */
};

static void test_dict(void) {
    dict *d = dictCreate(&harnessDictType);
    char buf[32];

    for (int i = 0; i < 1000; i++) {
        int n = (int)snprintf(buf, sizeof buf, "key:%d", i);
        sds k = sdsnewlen(buf, (size_t)n);
        assert(dictAdd(d, k, (void *)1) == DICT_OK);
    }
    assert(dictSize(d) == 1000);

    /* siphash-keyed lookups must find every key, including after the growth
     * rehashes triggered on the way up. */
    for (int i = 0; i < 1000; i += 7) {
        snprintf(buf, sizeof buf, "key:%d", i);
        dictEntry *de = dictFind(d, buf);
        assert(de != NULL);
        assert(dictGetVal(de) == (void *)1);
    }
    assert(dictFind(d, "key:1000") == NULL);

    /* Duplicated key is rejected. */
    sds dup = sdsnew("key:9");
    assert(dictAdd(d, dup, (void *)2) == DICT_ERR);
    sdsfree(dup);

    for (int i = 0; i < 500; i++) {
        snprintf(buf, sizeof buf, "key:%d", i);
        assert(dictDelete(d, buf) == DICT_OK);
    }
    assert(dictSize(d) == 500);
    assert(dictFind(d, "key:0") == NULL);
    assert(dictFind(d, "key:999") != NULL);

    dictRelease(d);
}

/* ---------------- sds / util / intset / adlist / crc16 ------------------- */

static void test_sds(void) {
    sds s = sdsnew("PFADD ");
    s = sdscatlen(s, "e1", 2);
    s = sdscatprintf(s, " %d %s", 42, "x");
    assert(strcmp(s, "PFADD e1 42 x") == 0);
    assert(sdslen(s) == 13);

    sdsclear(s);
    assert(sdslen(s) == 0);

    s = sdsgrowzero(s, 8);
    assert(sdslen(s) == 8);
    for (int i = 0; i < 8; i++) assert(s[i] == 0);

    sds b = sdsdup(s);
    assert(sdscmp(s, b) == 0);
    sdsfree(b);
    sdsfree(s);
}

static void test_util(void) {
    long long v = 0;
    assert(string2ll("9223372036854775807", 19, &v) && v == LLONG_MAX);
    assert(string2ll("-9223372036854775808", 20, &v) && v == LLONG_MIN);
    assert(!string2ll("9223372036854775808", 19, &v));
    assert(!string2ll("1x", 2, &v));

    char tmp[32];
    int n = ll2string(tmp, sizeof tmp, -42);
    assert(n == 3 && memcmp(tmp, "-42", 3) == 0);
    assert(string2ll(tmp, (size_t)n, &v) && v == -42);

    double d = 0;
    assert(string2d("3.141592653589793", 17, &d) && d == 3.141592653589793);
    assert(!string2d("not-a-number", 12, &d));

    /* d2string()/string2d() roundtrip (fpconv shortest representation). */
    int blen = d2string(tmp, sizeof tmp, 3.5);
    assert(blen > 0);
    double back = 0;
    assert(string2d(tmp, (size_t)blen, &back) && back == 3.5);
}

static void test_intset(void) {
    intset *is = intsetNew();
    uint8_t success = 0;
    int64_t vals[] = {1, 5, 3, 100, -7};
    for (unsigned i = 0; i < sizeof vals / sizeof vals[0]; i++) {
        is = intsetAdd(is, vals[i], &success);
        assert(success == 1);
    }
    is = intsetAdd(is, 5, &success);
    assert(success == 0);
    assert(intsetLen(is) == 5);
    assert(intsetFind(is, 100) == 1);
    assert(intsetFind(is, 42) == 0);
    assert(intsetMin(is) == -7);
    assert(intsetMax(is) == 100);

    int64_t got = 0;
    assert(intsetGet(is, 0, &got) && got == -7); /* sorted, 16 bit encoding */

    int removed = 0;
    is = intsetRemove(is, 5, &removed);
    assert(removed == 1 && intsetLen(is) == 4);
    assert(intsetFind(is, 5) == 0);

    /* Growing past the 16 bit range forces the 32 bit upgrade. */
    is = intsetAdd(is, 40000, &success);
    assert(success == 1 && intsetLen(is) == 5);
    assert(intsetFind(is, 40000) == 1);
    assert(intsetFind(is, 100) == 1);

    zfree(is);
}

static void test_adlist(void) {
    list *l = listCreate();
    for (int i = 0; i < 10; i++) listAddNodeTail(l, (void *)(long)i);
    assert(listLength(l) == 10);
    assert(listNodeValue(listLast(l)) == (void *)9);
    assert(listNodeValue(listFirst(l)) == (void *)0);

    listIter li;
    listNode *ln;
    int seen = 0;
    listRewind(l, &li);
    while ((ln = listNext(&li)) != NULL) seen++;
    assert(seen == 10);

    listRelease(l);
}

static void test_crc16(void) {
    /* The XMODEM/CCITT vector documented at the top of Redis' crc16.c. */
    assert(crc16("123456789", 9) == 0x31C3);
    assert(crc16("", 0) == 0);
}

/* ---------------- bitops (the kernel part of bitops.c) ------------------- */

static void test_bitops(void) {
    /* popcount against a naive reference over the same bytes: this is what
     * Redis' own bitopsTest() checks, and it covers the 8-byte-at-a-time main
     * loop, the 4-byte step and the byte tail of every CPU dispatch. */
    static const char sample[] = "the quick brown fox jumps over 13 lazy dogs!\xff\x0f";
    for (unsigned len = 0; len <= sizeof(sample); len++) {
        long long ref = 0;
        for (unsigned i = 0; i < len; i++)
            for (int b = 0; b < 8; b++)
                if (((unsigned char *)sample)[i] & (1 << b)) ref++;
        assert(redisPopcount((void *)sample, (long)len) == ref);
    }
    assert(redisPopcount((void *)"\0\0\0\0", 4) == 0);
    assert(redisPopcount((void *)"\xff\xff\xff\xff", 4) == 32);
    assert(redisPopcount((void *)"\xaa\x03", 2) == 6);

    /* bitpos indexes bits MSB-first, i.e. bit 0 is the high bit of byte 0. */
    assert(redisBitpos((void *)"\x00\xff", 2, 1) == 8);
    assert(redisBitpos((void *)"\xff\x7f", 2, 0) == 8);
    assert(redisBitpos((void *)"\x80", 1, 1) == 0);
    assert(redisBitpos((void *)"\x01", 1, 1) == 7);
    /* an all-zero string has no set bit: -1, and bitposCommand() turns that into
     * "length+1" the way Redis does. Right-padded zeros mean the reverse case
     * (first clear bit of an all-ones string) lands on length*8. */
    assert(redisBitpos((void *)"\x00", 1, 1) == -1);
    assert(redisBitpos((void *)"\xff", 1, 0) == 8);
    assert(redisBitpos((void *)"", 0, 1) == -1);

    /* bitfields, including values crossing byte boundaries and the 64 bit
     * full-width special case. */
    unsigned char bf[8];
    memset(bf, 0, sizeof bf);
    setUnsignedBitfield(bf, 0, 8, 0xab);
    assert(getUnsignedBitfield(bf, 0, 8) == 0xab);
    assert(bf[0] == 0xab);

    memset(bf, 0, sizeof bf);
    setUnsignedBitfield(bf, 5, 12, 0xffe);
    assert(getUnsignedBitfield(bf, 5, 12) == 0xffe);

    memset(bf, 0, sizeof bf);
    setSignedBitfield(bf, 3, 12, -2000);
    assert(getSignedBitfield(bf, 3, 12) == -2000);

    memset(bf, 0, sizeof bf);
    setUnsignedBitfield(bf, 0, 64, 0x0123456789abcdefULL);
    assert(getUnsignedBitfield(bf, 0, 64) == 0x0123456789abcdefULL);

    memset(bf, 0, sizeof bf);
    setSignedBitfield(bf, 4, 64, INT64_MIN);
    assert(getSignedBitfield(bf, 4, 64) == INT64_MIN);

    /* neighbouring fields must not clobber each other. */
    memset(bf, 0, sizeof bf);
    setUnsignedBitfield(bf, 0, 7, 0x55);
    setUnsignedBitfield(bf, 9, 7, 0x31);
    assert(getUnsignedBitfield(bf, 0, 7) == 0x55);
    assert(getUnsignedBitfield(bf, 9, 7) == 0x31);

    /* overflow detection: saturate reports the clamped value, wrap computes it,
     * and an in-range increment reports 0 without touching *limit. */
    uint64_t ulim = 0xa5a5;
    assert(checkUnsignedBitfieldOverflow(0xff, 1, 8, BFOVERFLOW_SAT, &ulim) == 1);
    assert(ulim == 0xff);
    ulim = 0xa5a5;
    assert(checkUnsignedBitfieldOverflow(0xff, 1, 8, BFOVERFLOW_WRAP, &ulim) == 1);
    assert(ulim == 0);
    ulim = 0xa5a5;
    assert(checkUnsignedBitfieldOverflow(0xff, -256, 8, BFOVERFLOW_SAT, &ulim) == -1);
    assert(ulim == 0);
    assert(checkUnsignedBitfieldOverflow(10, 5, 8, BFOVERFLOW_WRAP, &ulim) == 0);

    int64_t slim = 0x5a5a;
    assert(checkSignedBitfieldOverflow(127, 1, 8, BFOVERFLOW_SAT, &slim) == 1);
    assert(slim == 127);
    slim = 0x5a5a;
    assert(checkSignedBitfieldOverflow(-128, -1, 8, BFOVERFLOW_SAT, &slim) == -1);
    assert(slim == -128);
    assert(checkSignedBitfieldOverflow(0, 100, 8, BFOVERFLOW_WRAP, &slim) == 0);
}

/* ---------------- t_zset (skiplist + listpack zset kernel) ---------------- */

/* Element "m<i>" gets score (i % 20): 20 score classes of 5 elements each, so
 * every ordering assertion below also pins down the tie break by element, which
 * is the part the skiplist and the listpack representation have to agree on.
 * Note that the element order is a byte comparison, so inside class 3 the order
 * is m23, m3, m43, m63, m83 -- not the numeric index order. */
#define ZSET_NELE 100
#define ZSET_CLASS 20

static void zsetEle(char *buf, size_t buflen, int i) {
    snprintf(buf, buflen, "m%d", i);
}

/* 1 if element 'a' sorts before element 'b' in Redis' (score, element) order. */
static int zsetElementBefore(int a, int b) {
    int sa = a % ZSET_CLASS, sb = b % ZSET_CLASS;
    char ba[16], bb[16];
    if (sa != sb) return sa < sb;
    zsetEle(ba, sizeof ba, a);
    zsetEle(bb, sizeof bb, b);
    return strcmp(ba, bb) < 0;
}

/* Forward (0 based) rank of element 'i' among the elements marked alive. */
static long zsetExpectedRank(int i, const int *alive) {
    long expect = 0;
    for (int j = 0; j < ZSET_NELE; j++)
        if (alive[j]) expect += zsetElementBefore(j, i);
    return expect;
}

/* Every alive element must be findable with its score, and both rank
 * directions must match the (score, element) ordering. */
static void zsetCheckRanks(robj *o, const int *alive) {
    char buf[16];
    unsigned long n = 0;
    for (int i = 0; i < ZSET_NELE; i++) n += alive[i] != 0;
    assert(zsetLength(o) == n);

    for (int i = 0; i < ZSET_NELE; i++) {
        zsetEle(buf, sizeof buf, i);
        sds ele = sdsnew(buf);
        double score = -1;
        if (!alive[i]) {
            assert(zsetScore(o, ele, &score) == C_ERR);
            sdsfree(ele);
            continue;
        }
        assert(zsetScore(o, ele, &score) == C_OK);
        assert(score == (double)(i % ZSET_CLASS));
        long fwd = zsetRank(o, ele, 0, &score);
        assert(fwd == zsetExpectedRank(i, alive));
        /* zsetRank() fills the score out-parameter too */
        assert(score == (double)(i % ZSET_CLASS));
        assert(zsetRank(o, ele, 1, NULL) == (long)zsetLength(o) - 1 - fwd);
        sdsfree(ele);
    }
}

static void zsetFreeKernelObj(robj *o) {
    /* mirrors the freeSortedSet() half of object.c for the encodings used here:
     * the skiplist owns the element strings, the dict does not. */
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        lpFree(o->ptr);
    } else {
        zset *zs = o->ptr;
        dictRelease(zs->dict);
        zslFree(zs->zsl);
        zfree(zs);
    }
    zfree(o);
}

static void test_zset(void) {
    int alive[ZSET_NELE];
    char buf[16];
    int out_flags;
    double newscore, score;

    for (int i = 0; i < ZSET_NELE; i++) alive[i] = 1;

    robj *o = createZsetObject();
    assert(o != NULL && o->type == OBJ_ZSET);
    assert(o->encoding == OBJ_ENCODING_SKIPLIST);

    /* zsetAdd() never keeps the caller's sds: zslInsert()/zzlInsert() copy it
     * (t_zset.c:1485 "zslInsert copied it, we can free our copy"), which is the
     * ownership rule the facades must follow too. */
    for (int i = 0; i < ZSET_NELE; i++) {
        zsetEle(buf, sizeof buf, i);
        sds ele = sdsnew(buf);
        assert(zsetAdd(o, (double)(i % ZSET_CLASS), ele, ZADD_IN_NONE,
                       &out_flags, &newscore) == 1);
        assert(out_flags == ZADD_OUT_ADDED);
        assert(newscore == (double)(i % ZSET_CLASS));
        sdsfree(ele);
    }
    assert(((zset *)o->ptr)->zsl->length == ZSET_NELE);
    zsetCheckRanks(o, alive);

    /* an update moves the element through the skiplist */
    sds m7 = sdsnew("m7");
    assert(zsetAdd(o, 19.5, m7, ZADD_IN_NONE, &out_flags, &newscore) == 1);
    assert(out_flags == ZADD_OUT_UPDATED && newscore == 19.5);
    assert(zsetLength(o) == ZSET_NELE);
    assert(zsetScore(o, m7, &score) == C_OK && score == 19.5);

    /* NX / GT / LT / XX conditionals and INCR */
    assert(zsetAdd(o, 1.0, m7, ZADD_IN_NX, &out_flags, NULL) == 1);
    assert(out_flags == ZADD_OUT_NOP);
    assert(zsetScore(o, m7, &score) == C_OK && score == 19.5);
    assert(zsetAdd(o, 1.0, m7, ZADD_IN_GT, &out_flags, NULL) == 1);
    assert(out_flags == ZADD_OUT_NOP);            /* 1.0 is not > 19.5 */
    assert(zsetAdd(o, 25.0, m7, ZADD_IN_LT, &out_flags, NULL) == 1);
    assert(out_flags == ZADD_OUT_NOP);            /* 25.0 is not < 19.5 */
    assert(zsetAdd(o, 25.0, m7, ZADD_IN_XX, &out_flags, &score) == 1);
    assert(out_flags == ZADD_OUT_UPDATED && score == 25.0);
    assert(zsetAdd(o, 30.0, m7, ZADD_IN_INCR, &out_flags, &score) == 1);
    assert(out_flags == ZADD_OUT_UPDATED && score == 55.0);
    /* NaN input is rejected regardless of the other flags */
    assert(zsetAdd(o, 0.0/0.0, m7, ZADD_IN_NONE, &out_flags, NULL) == 0);
    assert(out_flags == ZADD_OUT_NAN);
    /* put m7 back where the rank expectations need it */
    assert(zsetAdd(o, 7.0, m7, ZADD_IN_NONE, &out_flags, &score) == 1);
    assert(out_flags == ZADD_OUT_UPDATED && score == 7.0);
    zsetCheckRanks(o, alive);

    /* INCR on a missing element adds it with that score */
    sds fresh = sdsnew("m999");
    assert(zsetAdd(o, 3.5, fresh, ZADD_IN_INCR, &out_flags, &score) == 1);
    assert(out_flags == ZADD_OUT_ADDED && score == 3.5);
    assert(zsetLength(o) == ZSET_NELE + 1);
    assert(zsetDel(o, fresh) == 1);
    assert(zsetDel(o, fresh) == 0);
    assert(zsetLength(o) == ZSET_NELE);
    sdsfree(fresh);

    /* delete every third element */
    for (int i = 0; i < ZSET_NELE; i += 3) {
        zsetEle(buf, sizeof buf, i);
        sds ele = sdsnew(buf);
        assert(zsetDel(o, ele) == 1);
        assert(zsetScore(o, ele, &score) == C_ERR);
        sdsfree(ele);
        alive[i] = 0;
    }
    zsetCheckRanks(o, alive);

    /* listpack round trip: members, scores and ordering survive the conversion
     * in both directions (this is the zzl + listpack code path). */
    zsetConvert(o, OBJ_ENCODING_LISTPACK);
    assert(o->encoding == OBJ_ENCODING_LISTPACK);
    zsetCheckRanks(o, alive);
    /* re-adding with the very same score is a no-op that must not touch the
     * listpack layout (m1 survived the deletions above) */
    sds m1 = sdsnew("m1");
    assert(zsetAdd(o, 1.0, m1, ZADD_IN_NONE, &out_flags, &score) == 1);
    assert(out_flags == 0 && score == 1.0);
    assert(zsetLength(o) == ZSET_NELE - (ZSET_NELE + 2) / 3);
    sdsfree(m1);
    zsetConvert(o, OBJ_ENCODING_SKIPLIST);
    assert(o->encoding == OBJ_ENCODING_SKIPLIST);
    zsetCheckRanks(o, alive);

    /* zsetDup(): an independent copy of a skiplist zset */
    robj *copy = zsetDup(o);
    assert(copy != NULL && copy->encoding == OBJ_ENCODING_SKIPLIST);
    assert(zsetLength(copy) == zsetLength(o));
    sds extra = sdsnew("m777");
    assert(zsetAdd(copy, 1.0, extra, ZADD_IN_NONE, &out_flags, NULL) == 1);
    assert(out_flags == ZADD_OUT_ADDED);
    assert(zsetLength(copy) == zsetLength(o) + 1);
    assert(zsetScore(o, extra, &score) == C_ERR);   /* original is untouched */
    sdsfree(extra);
    zsetFreeKernelObj(copy);

    /* direct skiplist API: insertion, rank, range specs, nth in range */
    zskiplist *zsl = zslCreate();
    sds x = sdsnew("x");
    for (int i = 1; i <= 5; i++) zslInsert(zsl, (double)i, x);
    assert(zsl->length == 5);
    assert(zslGetRank(zsl, 3.0, x) == 3);
    assert(zslGetRank(zsl, 6.0, x) == 0);
    zrangespec range = { .min = 2.0, .max = 4.0, .minex = 0, .maxex = 0 };
    assert(zslValueGteMin(2.0, &range) && !zslValueGteMin(1.0, &range));
    assert(zslValueLteMax(4.0, &range) && !zslValueLteMax(5.0, &range));
    unsigned long outrank = 0;
    zskiplistNode *n = zslNthInRange(zsl, &range, 1, &outrank);
    assert(n != NULL && n->score == 3.0 && outrank == 3);
    assert(zslGetNodeElement(n) != NULL && sdscmp(zslGetNodeElement(n), x) == 0);
    sdsfree(x);                   /* zslInsert copied it */
    zslFree(zsl);

    zsetFreeKernelObj(o);
}

int main(void) {
    test_geohash();
    test_hll();
    test_dict();
    test_sds();
    test_util();
    test_intset();
    test_adlist();
    test_bitops();
    test_zset();
    test_crc16();

    /* keep the summary line of the brief: cardinality of a 10k HLL */
    robj *o = mkhll();
    hllAddRange(o, 0, 10000);
    uint64_t c = hllCount((struct hllhdr *)o->ptr, NULL);
    zfree(o);

    printf("redislib harness OK: hll=%llu\n", (unsigned long long)c);
    return 0;
}
