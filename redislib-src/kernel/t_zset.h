/* t_zset.h -- skynetjit redislib: sorted-set types and kernel API.
 *
 * Extracted verbatim from Redis 8.10.0 src/server.h (the block that t_zset.c
 * needs no longer lives in server.h, which is not part of this port).
 * See redislib-src/NOTICE for provenance and the list of mechanical edits.
 */

#ifndef REDISLIB_T_ZSET_H
#define REDISLIB_T_ZSET_H

#include <stdint.h>
#include "sds.h"
#include "dict.h"

/* ZSETs use a specialized version of Skiplists */

/* Node info placed in level[0].span since it's unused at level 0 (static assert verified) */
typedef struct zskiplistNodeInfo {
    uint16_t sdsoffset;  /* Offset from node start to sds data (after sds header) */
    uint8_t levels;      /* Number of levels in this node (1-32) */
    uint8_t reserved;
} zskiplistNodeInfo;

typedef struct zskiplistNode {
    double score;
    struct zskiplistNode *backward;
    struct zskiplistLevel {
        struct zskiplistNode *forward;
        /* Span is the number of elements between this node and the next node at this level.
         * At level 0, span is repurposed to store zskiplistNodeInfo for regular nodes, */
        unsigned long span;
    } level[];
    /* sds ele is embedded after level[] array (assist zslGetNodeElement(node) to access it) */
} zskiplistNode;

typedef struct zskiplist {
    struct zskiplistNode *header, *tail;
    unsigned long length;
    int level;
    size_t alloc_size;
} zskiplist;

typedef struct zset {
    dict *dict;
    zskiplist *zsl;
} zset;


/* Sorted sets data type */

/* Input flags. */
#define ZADD_IN_NONE 0
#define ZADD_IN_INCR (1<<0)    /* Increment the score instead of setting it. */
#define ZADD_IN_NX (1<<1)      /* Don't touch elements not already existing. */
#define ZADD_IN_XX (1<<2)      /* Only touch elements already existing. */
#define ZADD_IN_GT (1<<3)      /* Only update existing when new scores are higher. */
#define ZADD_IN_LT (1<<4)      /* Only update existing when new scores are lower. */

/* Output flags. */
#define ZADD_OUT_NOP (1<<0)     /* Operation not performed because of conditionals.*/
#define ZADD_OUT_NAN (1<<1)     /* Only touch elements already existing. */
#define ZADD_OUT_ADDED (1<<2)   /* The element was new and was added. */
#define ZADD_OUT_UPDATED (1<<3) /* The element already existed, score updated. */

/* Struct to hold an inclusive/exclusive range spec by score comparison. */
typedef struct {
    double min, max;
    int minex, maxex; /* are min or max exclusive? */
} zrangespec;

/* Struct to hold an inclusive/exclusive range spec by lexicographic comparison. */
typedef struct {
    sds min, max;     /* May be set to shared.(minstring|maxstring) */
    int minex, maxex; /* are min or max exclusive? */
} zlexrangespec;

/* --- zsl / zzl / zset kernel API (from src/server.h) --- */
zskiplist *zslCreate(void);
void zslFree(zskiplist *zsl);
size_t zslAllocSize(const zskiplist *zsl);
sds zslGetNodeElement(const zskiplistNode *node);
int zslCompareWithNode(double score, sds ele, const zskiplistNode *n);
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele);
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score);
zskiplistNode *zslNthInRange(zskiplist *zsl, zrangespec *range, long n, unsigned long *out_rank);
double zzlGetScore(unsigned char *sptr);
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr);
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range);
unsigned long zsetLength(const robj *zobj);
size_t zsetAllocSize(const robj *o);
void zsetConvert(robj *zobj, int encoding);
void zsetConvertToListpackIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen);
int zsetScore(robj *zobj, sds member, double *score);
unsigned long zslGetRank(zskiplist *zsl, double score, sds o);
int zsetAdd(robj *zobj, double score, sds ele, int in_flags, int *out_flags, double *newscore);
long zsetRank(robj *zobj, sds ele, int reverse, double *score);
int zsetDel(robj *zobj, sds ele);
robj *zsetDup(robj *o);
sds lpGetObject(unsigned char *sptr);
int zslValueGteMin(double value, zrangespec *spec);
int zslValueLteMax(double value, zrangespec *spec);
void zslFreeLexRange(zlexrangespec *spec);
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec);
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range);
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range);
zskiplistNode *zslNthInLexRange(zskiplist *zsl, zlexrangespec *range, long n, unsigned long *out_rank);
int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec);
int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec);
int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);

#endif /* REDISLIB_T_ZSET_H */
