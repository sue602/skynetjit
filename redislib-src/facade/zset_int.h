/* zset_int.h -- skynetjit redislib: the sorted-set iterator types and the score
 * aggregation helpers, copied VERBATIM from this port's own kernel file
 * redislib-src/kernel/t_zset.c (upstream Redis 8.10.0 src/t_zset.c).
 *
 * Why a copy: ZUNION / ZINTER / ZDIFF iterate their operands through the zui*()
 * functions, which take the zsetopsrc / zsetopval types, and they aggregate
 * scores through REDIS_AGGR_* + zuiWeightedScore() + zunionInterAggregate().
 * All of those are file-scope in t_zset.c (upstream declares the types and
 * macros in server.h, which is not part of this port), so a facade cannot reach
 * them. Copying them here keeps the kernel identical to upstream and gives one
 * live implementation per translation unit, at the price of a second textual
 * copy: see redislib-src/NOTICE §3.1, and tools/redis-diff (Task 10) for the
 * behavioral check.
 *
 * Deliberately NOT copied: the ZDIFF algorithm block (zdiffAlgorithm1/2,
 * zsetChooseDiffAlgorithm, zdiff) and the zsetRemoveFromSkiplist() it calls --
 * that chain reaches the file-local skiplist internals (zslDelete() and
 * friends), and copying those would move the skiplist itself into the facade.
 * facade/sortedset.c implements ZDIFF with the exported dict and zslInsert()
 * API instead: upstream's algorithm 2 (stage the first operand, delete the
 * rest), the same result.
 *
 * Ranges copied (line numbers in the ported kernel file):
 *   1950-2014  zsetopsrc / zsetopval and the iterator unions
 *   2301-2334  REDIS_AGGR_*, zuiWeightedScore, zunionInterAggregate
 *
 * Nothing in those ranges is edited: no rename, no signature change, no logic
 * change. The intset.h include and the zui* declarations below are this file's
 * own additions (the types and prototypes upstream got from server.h).
 */

#ifndef REDISLIB_ZSET_INT_H
#define REDISLIB_ZSET_INT_H

/* The set iterator branch of zsetopsrc below names intset, which lives in the
 * ported kernel header (upstream: server.h pulls it in). */
#include "intset.h"

/* Unified iterator source for set operations (ZUNION/ZINTER/ZDIFF).
 * Provides polymorphic iteration over sets and sorted sets with different encodings. */
typedef struct {
    robj *subject;
    int type; /* Set, sorted set */
    int encoding;
    double weight;
    size_t oldsize;

    union {
        /* Set iterators. */
        union _iterset {
            struct {
                intset *is;
                int ii;
            } is;
            struct {
                dict *dict;
                dictIterator *di;
                dictEntry *de;
            } ht;
            struct {
                unsigned char *lp;
                unsigned char *p;
            } lp;
        } set;

        /* Sorted set iterators. */
        union _iterzset {
            struct {
                unsigned char *zl;
                unsigned char *eptr, *sptr;
            } zl;
            struct {
                zset *zs;
                zskiplistNode *node;
            } sl;
        } zset;
    } iter;
} zsetopsrc;


/* Use dirty flags for pointers that need to be cleaned up in the next
 * iteration over the zsetopval. The dirty flag for the long long value is
 * special, since long long values don't need cleanup. Instead, it means that
 * we already checked that "ell" holds a long long, or tried to convert another
 * representation into a long long value. When this was successful,
 * OPVAL_VALID_LL is set as well. */
#define OPVAL_DIRTY_SDS 1
#define OPVAL_DIRTY_LL 2
#define OPVAL_VALID_LL 4

/* Store value retrieved from the iterator. */
typedef struct {
    int flags;
    unsigned char _buf[32]; /* Private buffer. */
    sds ele;
    unsigned char *estr;
    unsigned int elen;
    long long ell;
    double score;
} zsetopval;

typedef union _iterset iterset;
typedef union _iterzset iterzset;

/* The exported half of that iterator API (t_zset.c); upstream declares them in
 * server.h. Declared between the two copied ranges: they take the types above
 * and are called by the helpers below. */
void zuiInitIterator(zsetopsrc *op);
void zuiClearIterator(zsetopsrc *op);
void zuiDiscardDirtyValue(zsetopval *val);
unsigned long zuiLength(zsetopsrc *op);
int zuiNext(zsetopsrc *op, zsetopval *val);
sds zuiSdsFromValue(zsetopval *val);
sds zuiNewSdsFromValue(zsetopval *val);
int zuiFind(zsetopsrc *op, zsetopval *val, double *score);
int zuiCompareByCardinality(const void *s1, const void *s2);

#define REDIS_AGGR_SUM 1
#define REDIS_AGGR_MIN 2
#define REDIS_AGGR_MAX 3
#define REDIS_AGGR_COUNT 4

/* Return the weighted contribution of a single sorted set member.
 * For COUNT aggregation the actual score is irrelevant — each member
 * contributes its set's weight (i.e. "one occurrence worth <weight>").
 * For all other aggregation modes the contribution is weight * score. */
inline static double zuiWeightedScore(double score, double weight, int aggregate) {
    return (aggregate == REDIS_AGGR_COUNT) ? weight : weight * score;
}

inline static void zunionInterAggregate(double *target, double val, int aggregate) {
    if (aggregate == REDIS_AGGR_SUM) {
        *target = *target + val;
        /* The result of adding two doubles is NaN when one variable
         * is +inf and the other is -inf. When these numbers are added,
         * we maintain the convention of the result being 0.0. */
        if (isnan(*target)) *target = 0.0;
    } else if (aggregate == REDIS_AGGR_COUNT) {
        *target += val;
        /* The val is zuiWeightedScore(…) == weight, which can be +inf/-inf,
         * so the NaN guard applies here. */
        if (isnan(*target)) *target = 0.0;
    } else if (aggregate == REDIS_AGGR_MIN) {
        *target = val < *target ? val : *target;
    } else if (aggregate == REDIS_AGGR_MAX) {
        *target = val > *target ? val : *target;
    } else {
        /* safety net */
        serverPanic("Unknown ZUNION/INTER aggregate type");
    }
}
#endif /* REDISLIB_ZSET_INT_H */
