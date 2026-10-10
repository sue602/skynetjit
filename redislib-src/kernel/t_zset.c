/* t_zset.c -- zset data type implementation.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Copyright (c) 2024-present, Valkey contributors.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 *
 * Portions of this file are available under BSD3 terms; see REDISCONTRIBUTIONS for more information.
 */
/* skynetjit port: command layer removed, see redislib-src/NOTICE */

/*-----------------------------------------------------------------------------
 * Sorted set API
 *----------------------------------------------------------------------------*/

/* ZSETs are ordered sets using two data structures to hold the same elements
 * in order to get O(log(N)) INSERT and REMOVE operations into a sorted
 * data structure.
 *
 * The elements are added to a hash table mapping Redis objects to scores.
 * At the same time the elements are added to a skip list mapping scores
 * to Redis objects (so objects are sorted by scores in this "view").
 *
 * Note that the SDS string representing the element is the same in both
 * the hash table and skiplist in order to save memory. What we do in order
 * to manage the shared SDS string more easily is to free the SDS string
 * only in zslFreeNode(). The dictionary has no value free method set.
 * So we should always remove an element from the dictionary, and later from
 * the skiplist.
 *
 * This skiplist implementation is almost a C translation of the original
 * algorithm described by William Pugh in "Skip Lists: A Probabilistic
 * Alternative to Balanced Trees", modified in three ways:
 * a) this implementation allows for repeated scores.
 * b) the comparison is not just by key (our 'score') but by satellite data.
 * c) there is a back pointer, so it's a doubly linked list with the back
 * pointers being only at "level 1". This allows to traverse the list
 * from tail to head, useful for ZREVRANGE. */
#include "fast_float_strtod.h"
#include "redislib_port.h"
#include "intset.h"  /* Compact integer set structure */
#include <math.h>

#define ZSL_OFFSET_MAX_ELE  UINT16_MAX
#define ZSL_OFFSET_NO_ELE   UINT16_MAX

const void *zslGetNodeElementForDict(const void *node);

/* dictType for zset's dict (maps sds to zskiplistNode*) */
dictType zsetDictType = {
    dictSdsHash,        /* hash function */
    NULL,               /* key dup */
    NULL,               /* val dup */
    dictSdsKeyCompare,  /* compares embedded sds by keyFromStoredKey */
    NULL,               /* key destructor - skiplist owns the node memory */
    NULL,               /* val destructor */
    NULL,               /* allow to expand */
    .no_value = 1,      /* no values stored (only nodes) */
    .keyFromStoredKey = zslGetNodeElementForDict,  /* extract embedded sds from node */
};

/*-----------------------------------------------------------------------------
 * Skiplist implementation of the low level API
 *----------------------------------------------------------------------------*/

int zslLexValueGteMin(sds value, zlexrangespec *spec);
int zslLexValueLteMax(sds value, zlexrangespec *spec);
void zsetConvertAndExpand(robj *zobj, int encoding, unsigned long cap);
static zskiplistNode *zslGetElementByRankFromNode(zskiplistNode *start_node, int start_level, unsigned long rank);

static inline unsigned long zslGetNodeSpanAtLevel(zskiplistNode *x, int level) {
    /* At level 0, span stores node level instead of distance, so return the actual span value:
     * 1 for all nodes except the last node (which has span 0). */
    if (level > 0) return x->level[level].span;
    /* For level 0, if regular node, span is 1. If tail node, span is 0. */
    return x->level[0].forward ? 1 : 0;
}

static inline void zslSetNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long span) {
    /* Skip level 0 since it stores node level, not span. */
    if (level > 0)
        x->level[level].span = span;
}

static inline void zslIncrNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long incr) {
    /* Skip level 0 since it stores node level, not span. */
    if (level > 0)
        x->level[level].span += incr;
}

static inline void zslDecrNodeSpanAtLevel(zskiplistNode *x, int level, unsigned long decr) {
    /* Skip level 0 since it stores node level, not span. */
    if (level > 0)
        x->level[level].span -= decr;
}

/* Get zskiplistNodeInfo from node (stored in level[0].span). */
static_assert(sizeof(zskiplistNodeInfo) <= sizeof(((zskiplistNode *)0)->level[0].span), "Must fit in level[0].span");
static inline zskiplistNodeInfo *zslGetNodeInfo(const zskiplistNode *node) {
    return (zskiplistNodeInfo *)&node->level[0].span;
}

/* Set zskiplistNodeInfo in node (stored in level[0].span) */
static inline void zslSetNodeInfo(zskiplistNode *node, uint8_t levels, uint16_t sdsoffset) {
    union {
        zskiplistNodeInfo info;
        unsigned long span;
    } u = { .info = { .levels = levels, .sdsoffset = sdsoffset } };
    node->level[0].span = u.span;
}

/* Compare {score, ele} with node. Returns: 1=bigger 0=equal -1=smaller
 *
 * Ordering is by score first, then lexicographically by element.
 * NULL is treated as +infinity (comes after any real node). */
int zslCompareWithNode(double score, sds ele, const zskiplistNode *n) {
    if (/*score < */ n == NULL) return -1; /* NULL is +infinity, comes after any real node */
    if (score < n->score) return -1;
    if (score > n->score) return 1;
    /* Scores are equal, compare elements lexicographically */
    return sdscmp(ele, zslGetNodeElement(n));
}

/* Get embedded sds from node. Uses the stored offset to directly access the sds data */
sds zslGetNodeElement(const zskiplistNode *node) {
    zskiplistNodeInfo *info = zslGetNodeInfo(node);
    debugServerAssert(info->sdsoffset != ZSL_OFFSET_NO_ELE);
    return (char*)node + info->sdsoffset;
}

/* Wrapper for dict getKeyId callback - extracts sds from node pointer.
 * This allows the dict to store zskiplistNode* but look them up using sds. */
const void *zslGetNodeElementForDict(const void *node) {
    return zslGetNodeElement((zskiplistNode*)node);
}

/* Create a skiplist header node with ZSKIPLIST_MAXLEVEL levels */
static zskiplistNode *zslCreateHeaderNode(zskiplist *zsl) {
    size_t usable;
    zskiplistNode *zn = zmalloc_usable(sizeof(*zn) + ZSKIPLIST_MAXLEVEL * sizeof(struct zskiplistLevel), &usable);

    /* Initialize all fields */
    zn->score = 0;
    zn->backward = NULL;

    /* Initialize all level pointers and spans */
    for (int j = 0; j < ZSKIPLIST_MAXLEVEL; j++) {
        zn->level[j].forward = NULL;
        zn->level[j].span = 0;  /* Will be overwritten for level[0] below */
    }

    /* Use ZSL_OFFSET_NO_ELE as sentinel to indicate no embedded sds (header node) */
    zslSetNodeInfo(zn, ZSKIPLIST_MAXLEVEL, ZSL_OFFSET_NO_ELE);

    /* Track allocation size */
    zsl->alloc_size += usable;

    return zn;
}

/* Create a skiplist node with the specified number of levels.
 * The SDS string 'ele' is COPIED into an embedded sds within the node allocation.
 * This creates a single allocation containing: node + level[] + embedded sds.
 * The caller is responsible for freeing 'ele' if it's no longer needed. */
static zskiplistNode *zslCreateNode(zskiplist *zsl, int level, double score, sds ele) {
    size_t usable;
    size_t ele_len = sdslen(ele);
    char sds_type = sdsReqType(ele_len);
    size_t sds_hdr_len = sdsHdrSize(sds_type);

    /* Calculate total size: node fixed part + level[] + sds buffer space */
    size_t node_size = sizeof(zskiplistNode) + level * sizeof(struct zskiplistLevel);
    size_t sds_buf_size = sds_hdr_len + ele_len + 1;  /* header + data + null terminator */
    size_t total_size = node_size + sds_buf_size;

    /* Allocate single block for everything */
    zskiplistNode *zn = zmalloc_usable(total_size, &usable);

    /* Initialize node fields */
    zn->score = score;
    zn->backward = NULL;

    /* Calculate offset from node start to sds data (after sds header) */
    size_t sds_offset = node_size + sds_hdr_len;
    debugServerAssert(sds_offset < ZSL_OFFSET_MAX_ELE);

    /* Initialize embedded sds using sdsnewplacement */
    char *sds_buf = (char*)zn + node_size;
    sds embedded_sds = sdsnewplacement(sds_buf, sds_buf_size, sds_type, ele, ele_len);

    /* Store node info in level[0].span */
    zslSetNodeInfo(zn, level, sds_offset);

    /* Verify that embedded_sds matches our calculated offset */
    serverAssert(embedded_sds == (sds)((char*)zn + sds_offset));

    /* Update allocation size tracking */
    zsl->alloc_size += usable;

    return zn;
}

/* Create a new skiplist. */
zskiplist *zslCreate(void) {
    zskiplist *zsl;
    size_t zsl_size;

    zsl = zmalloc_usable(sizeof(*zsl), &zsl_size);
    zsl->level = 1;
    zsl->length = 0;
    zsl->alloc_size = zsl_size;
    zsl->header = zslCreateHeaderNode(zsl);
    zsl->header->backward = NULL;
    zsl->tail = NULL;
    return zsl;
}

/* Free the specified skiplist node. The embedded SDS is freed as part of
 * the single allocation (node + level[] + embedded sds). */
static void zslFreeNode(zskiplist *zsl, zskiplistNode *node) {
    size_t usable;
    /* No separate sdsfree() needed - embedded sds is part of node allocation */
    zfree_usable(node, &usable);
    zsl->alloc_size -= usable;
}

/* Free a whole skiplist. */
void zslFree(zskiplist *zsl) {
    zskiplistNode *node = zsl->header->level[0].forward, *next;
    size_t usable;

    zfree_usable(zsl->header, &usable);
    zsl->alloc_size -= usable;
    while(node) {
        next = node->level[0].forward;
        zslFreeNode(zsl, node);
        node = next;
    }
    debugServerAssert(zsl->alloc_size == zmalloc_usable_size(zsl));
    zfree(zsl);
}

/* Return cached total memory used (in bytes) */
size_t zslAllocSize(const zskiplist *zsl) { return zsl->alloc_size; }

/* Returns a random level for the new skiplist node we are going to create.
 * The return value of this function is between 1 and ZSKIPLIST_MAXLEVEL
 * (both inclusive), with a powerlaw-alike distribution where higher
 * levels are less likely to be returned. */
static int zslRandomLevel(void) {
    static const int threshold = ZSKIPLIST_P*RAND_MAX;
    int level = 1;
    while (random() < threshold)
        level += 1;
    return (level<ZSKIPLIST_MAXLEVEL) ? level : ZSKIPLIST_MAXLEVEL;
}

/* Insert an already-created node, with its score, element set into the skiplist 
 * at the correct position. Updates all forward/backward pointers and spans.
 * The node's level must already be set via zslSetNodeInfo(). */
static void zslInsertNode(zskiplist *zsl, zskiplistNode *node) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL];  /* Nodes that will point to the new node at each level */
    unsigned long rank[ZSKIPLIST_MAXLEVEL];     /* Rank (0-based) at each level during traversal */
    zskiplistNode *x;
    int i, level;
    double score = node->score;
    sds ele = zslGetNodeElement(node);
    level = zslGetNodeInfo(node)->levels;
    serverAssert(!isnan(score));

    /* Find the position where this node should be inserted */
    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        /* store rank that is crossed to reach the insert position */
        rank[i] = i == (zsl->level-1) ? 0 : rank[i+1];
        while (zslCompareWithNode(score, ele, x->level[i].forward) > 0) {
            rank[i] += zslGetNodeSpanAtLevel(x, i);
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Update skiplist level if needed */
    if (level > zsl->level) {
        for (i = zsl->level; i < level; i++) {
            rank[i] = 0;
            update[i] = zsl->header;
            zslSetNodeSpanAtLevel(update[i], i, zsl->length);
        }
        zsl->level = level;
        zslGetNodeInfo(zsl->header)->levels = level;
    }

    /* Insert the node at the found position */
    for (i = 0; i < level; i++) {
        node->level[i].forward = update[i]->level[i].forward;
        update[i]->level[i].forward = node;

        /* update span covered by update[i] as node is inserted here */
        zslSetNodeSpanAtLevel(node, i, zslGetNodeSpanAtLevel(update[i], i) - (rank[0] - rank[i]));
        zslSetNodeSpanAtLevel(update[i], i, (rank[0] - rank[i]) + 1);
    }

    /* increment span for untouched levels */
    for (i = level; i < zsl->level; i++) {
        zslIncrNodeSpanAtLevel(update[i], i, 1);
    }

    /* Update backward pointers */
    node->backward = (update[0] == zsl->header) ? NULL : update[0];
    if (node->level[0].forward)
        node->level[0].forward->backward = node;
    else
        zsl->tail = node;

    zsl->length++;
}

/* Insert a new node in the skiplist. Assumes the element does not already
 * exist (up to the caller to enforce that). The element 'ele' is COPIED
 * into the new node, so the caller retains ownership and can free it. */
zskiplistNode *zslInsert(zskiplist *zsl, double score, sds ele) {
    int level;

    serverAssert(!isnan(score));

    /* we assume the element is not already inside, since we allow duplicated
     * scores, reinserting the same element should never happen since the
     * caller of zslInsert() should test in the hash table if the element is
     * already inside or not. */
    level = zslRandomLevel();
    zskiplistNode *node = zslCreateNode(zsl, level, score, ele);
    zslInsertNode(zsl, node);
    return node;
}

/* Internal function used by zslDelete, zslDeleteRangeByScore and
 * zslDeleteRangeByRank.
 * This function only unlinks the node from the skiplist structure but does NOT free it.
 * The caller is responsible for freeing the node with zslFreeNode(). */
static void zslUnlinkNode(zskiplist *zsl, zskiplistNode *x, zskiplistNode **update) {
    int i;
    for (i = 0; i < zsl->level; i++) {
        if (update[i]->level[i].forward == x) {
            zslIncrNodeSpanAtLevel(update[i], i, zslGetNodeSpanAtLevel(x, i) - 1);
            update[i]->level[i].forward = x->level[i].forward;
        } else {
            zslDecrNodeSpanAtLevel(update[i], i, 1);
        }
    }
    if (x->level[0].forward) {
        x->level[0].forward->backward = x->backward;
    } else {
        zsl->tail = x->backward;
    }
    /* Decrease skiplist level if top levels are empty, and clear their spans */
    while(zsl->level > 1 && zsl->header->level[zsl->level-1].forward == NULL) {
        zsl->header->level[zsl->level-1].span = 0;
        zsl->level--;
    }
    zsl->length--;
}

/* Delete the specified node from the skiplist.
 * The node is unlinked from all levels and then freed by zslFreeNode(),
 * which also frees the embedded SDS string. */
static void zslDelete(zskiplist *zsl, zskiplistNode *node) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;
    double score = node->score;
    sds ele = zslGetNodeElement(node);

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (zslCompareWithNode(score, ele, x->level[i].forward) > 0) {
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Verify we truly found the node */
    serverAssert(x->level[0].forward == node);

    zslUnlinkNode(zsl, node, update);
    zslFreeNode(zsl, node);
}

/* Update the score of an element inside the sorted set skiplist.
 * If the new score would keep the node in its current position, updates in-place and returns NULL.
 * Otherwise, unlinks the node, updates score, reinserts at correct position, and returns node.
 * Anyway, the node pointer stays the same (no dict update needed). */
static void zslUpdateScore(zskiplist *zsl, zskiplistNode *node, double newscore) {
    /* Fast path: if the node, after the score update, would be still exactly
     * at the same position, we can just update the score without
     * actually removing and re-inserting the element in the skiplist. */
    if ((node->backward == NULL || node->backward->score < newscore) &&
        (node->level[0].forward == NULL || node->level[0].forward->score > newscore))
    {
        node->score = newscore;
        return;
    }

    /* Slow path: need to reposition the node.
     * Find the update[] array for unlinking. */
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    int i;
    double curscore = node->score;
    sds ele = zslGetNodeElement(node);

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (zslCompareWithNode(curscore, ele, x->level[i].forward) > 0) {
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    /* Verify we found the right node */
    serverAssert(x->level[0].forward == node);

    /* Unlink, update score, and reinsert at new position.
     * We reuse the same node to avoid dict updates. */
    zslUnlinkNode(zsl, node, update);
    node->score = newscore;
    zslInsertNode(zsl, node);
}

int zslValueGteMin(double value, zrangespec *spec) {
    return spec->minex ? (value > spec->min) : (value >= spec->min);
}

int zslValueLteMax(double value, zrangespec *spec) {
    return spec->maxex ? (value < spec->max) : (value <= spec->max);
}

/* Returns if there is a part of the zset is in range. */
static int zslIsInRange(zskiplist *zsl, zrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;
    x = zsl->tail;
    if (x == NULL || !zslValueGteMin(x->score,range))
        return 0;
    x = zsl->header->level[0].forward;
    if (x == NULL || !zslValueLteMax(x->score,range))
        return 0;
    return 1;
}

/* Find the Nth element within the specified score range.
 *
 * Parameters:
 *   - N is 0-based for forward direction (0 = first element in range)
 *   - N can be negative for reverse direction (-1 = last element in range)
 *
 * Returns:
 *   - The skiplist node at position N within the range, or NULL if:
 *     * N is out of bounds for the range
 *     * The range contains no elements
 *   - If out_rank!=NULL, it receives the 1-based absolute rank of the returned node 
 */
zskiplistNode *zslNthInRange(zskiplist *zsl, zrangespec *range, long n, unsigned long *out_rank) {
    zskiplistNode *x;
    int i;
    long edge_rank = 0; /* 0-based rank of the last element smaller than the range. */
    long last_highest_level_rank = 0;
    zskiplistNode *last_highest_level_node = NULL;
    unsigned long rank_diff;

    /* If everything is out of range, return early. */
    if (!zslIsInRange(zsl,range)) return NULL;

    /* Go forward while *OUT* of range at level of zsl->level-1. */
    x = zsl->header;
    i = zsl->level - 1;
    while (x->level[i].forward && !zslValueGteMin(x->level[i].forward->score, range)) {
        edge_rank += zslGetNodeSpanAtLevel(x, i);
        x = x->level[i].forward;
    }
    /* Remember the last node which has zsl->level-1 levels and its rank. */
    last_highest_level_node = x;
    last_highest_level_rank = edge_rank;

    if (n >= 0) {
        for (i = zsl->level - 2; i >= 0; i--) {
            /* Go forward while *OUT* of range. */
            while (x->level[i].forward && !zslValueGteMin(x->level[i].forward->score, range)) {
                /* Count the rank of the last element smaller than the range. */
                edge_rank += zslGetNodeSpanAtLevel(x, i);
                x = x->level[i].forward;
            }
        }
        /* Check if zsl is long enough. */
        if ((unsigned long)(edge_rank + n) >= zsl->length) return NULL;
        if (n < ZSKIPLIST_MAX_SEARCH) {
            /* If offset is small, we can just jump node by node */
            /* rank+1 is the first element in range, so we need n+1 steps to reach target. */
            for (i = 0; i < n + 1; i++) { 
                x = x->level[0].forward;
            }
        } else {
            /* If offset is big, we can jump from the last zsl->level-1 node. */
            rank_diff = edge_rank + 1 + n - last_highest_level_rank;
            x = zslGetElementByRankFromNode(last_highest_level_node, zsl->level - 1, rank_diff);
        }
        /* Check if score <= max. */
        if (x && !zslValueLteMax(x->score,range)) return NULL;
        /* Store rank if requested. For n >= 0, the returned node is at rank edge_rank + n + 1. */
        if (x && out_rank) *out_rank = edge_rank + n + 1;
    } else  {
        for (i = zsl->level - 1; i >= 0; i--) {
            /* Go forward while *IN* range. */
            while (x->level[i].forward && zslValueLteMax(x->level[i].forward->score, range)) {
                /* Count the rank of the last element in range. */
                edge_rank += zslGetNodeSpanAtLevel(x, i);
                x = x->level[i].forward;
            }
        }
        /* Check if the range is big enough. */
        if (edge_rank < -n) return NULL;
        if (n + 1 > -ZSKIPLIST_MAX_SEARCH) {
            /* If offset is small, we can just jump node by node */
            /* rank is the -1th element in range, so we need -n-1 steps to reach target. */
            for (i = 0; i < -n - 1; i++) {
                x = x->backward;
            }
        } else {
            /* If offset is big, we can jump from the last zsl->level-1 node. */
            /* rank is the last element in range, n is -1-based, so we need n+1 to count backwards. */
            rank_diff = edge_rank + 1 + n - last_highest_level_rank;
            x = zslGetElementByRankFromNode(last_highest_level_node, zsl->level - 1, rank_diff);
        }
        /* Check if score >= min. */
        if (x && !zslValueGteMin(x->score, range)) return NULL;
        /* Store rank if requested. For n < 0, the returned node is at rank edge_rank + n + 1. */
        if (x && out_rank) *out_rank = edge_rank + n + 1;
    }

    return x;
}

/* Delete all the elements with score between min and max from the skiplist.
 * Both min and max can be inclusive or exclusive (see range->minex and
 * range->maxex). When inclusive a score >= min && score <= max is deleted.
 * Note that this function takes the reference to the hash table view of the
 * sorted set, in order to remove the elements from the hash table too. */
static unsigned long zslDeleteRangeByScore(zskiplist *zsl, zrangespec *range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            !zslValueGteMin(x->level[i].forward->score, range))
                x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && zslValueLteMax(x->score, range)) {
        zskiplistNode *next = x->level[0].forward;
        zslUnlinkNode(zsl,x,update);
        dictDelete(dict,zslGetNodeElement(x));
        zslFreeNode(zsl, x); /* Here is where x->ele is actually released. */
        removed++;
        x = next;
    }
    return removed;
}

static unsigned long zslDeleteRangeByLex(zskiplist *zsl, zlexrangespec *range, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long removed = 0;
    int i;


    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward &&
            !zslLexValueGteMin(zslGetNodeElement(x->level[i].forward),range))
                x = x->level[i].forward;
        update[i] = x;
    }

    /* Current node is the last with score < or <= min. */
    x = x->level[0].forward;

    /* Delete nodes while in range. */
    while (x && zslLexValueLteMax(zslGetNodeElement(x),range)) {
        zskiplistNode *next = x->level[0].forward;
        zslUnlinkNode(zsl,x,update);
        dictDelete(dict,zslGetNodeElement(x));
        zslFreeNode(zsl, x); /* Here is where x->ele is actually released. */
        removed++;
        x = next;
    }
    return removed;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
static unsigned long zslDeleteRangeByRank(zskiplist *zsl, unsigned int start, unsigned int end, dict *dict) {
    zskiplistNode *update[ZSKIPLIST_MAXLEVEL], *x;
    unsigned long traversed = 0, removed = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (x->level[i].forward && (traversed + zslGetNodeSpanAtLevel(x, i)) < start) {
            traversed += zslGetNodeSpanAtLevel(x, i);
            x = x->level[i].forward;
        }
        update[i] = x;
    }

    traversed++;
    x = x->level[0].forward;
    while (x && traversed <= end) {
        zskiplistNode *next = x->level[0].forward;
        zslUnlinkNode(zsl,x,update);
        dictDelete(dict,zslGetNodeElement(x));
        zslFreeNode(zsl, x);
        removed++;
        traversed++;
        x = next;
    }
    return removed;
}

/* Find the rank for an element by both score and key.
 * Returns 0 when the element cannot be found, rank otherwise.
 * Note that the rank is 1-based due to the span of zsl->header to the
 * first element. */
unsigned long zslGetRank(zskiplist *zsl, double score, sds ele) {
    zskiplistNode *x;
    unsigned long rank = 0;
    int i;

    x = zsl->header;
    for (i = zsl->level-1; i >= 0; i--) {
        while (zslCompareWithNode(score, ele, x->level[i].forward) >= 0) {
            rank += zslGetNodeSpanAtLevel(x, i);
            x = x->level[i].forward;
        }

        if (x != zsl->header && zslCompareWithNode(score, ele, x) == 0) {
            return rank;
        }
    }
    return 0;
}

/* Find the rank for a skiplist node by walking forward from the node to the end.
 * This avoids expensive string comparisons during traversal. The algorithm:
 * 1. Start at the given node's top level
 * 2. Walk forward to the tail, jumping at each node's top level
 * 3. Sum the spans to get distance from node to end
 * 4. Calculate rank as (list_length - distance_to_end)
 * Time complexity: O(log N) on average, same as traditional approach but faster
 * due to avoiding string comparisons. */
unsigned long zslGetRankByNode(zskiplist *zsl, zskiplistNode *x) {
    unsigned long distance_to_end = 0;
    int level;
    
    /* Walk forward from x to the end, using top level of each node for fast jumps */
    while (x) {
        level = zslGetNodeInfo(x)->levels - 1;
        distance_to_end += zslGetNodeSpanAtLevel(x, level);
        x = x->level[level].forward;
    }
    
    /* Rank = total nodes - nodes after this one */
    return zsl->length - distance_to_end;
}

/* Finds an element by its rank from start node. The rank argument needs to be 1-based. */
static zskiplistNode *zslGetElementByRankFromNode(zskiplistNode *start_node, int start_level, unsigned long rank) {
    zskiplistNode *x;
    unsigned long traversed = 0;
    int i;

    x = start_node;
    for (i = start_level; i >= 0; i--) {
        while (x->level[i].forward && (traversed + zslGetNodeSpanAtLevel(x, i)) <= rank)
        {
            traversed += zslGetNodeSpanAtLevel(x, i);
            x = x->level[i].forward;
        }
        if (traversed == rank) {
            return x;
        }
    }
    return NULL;
}

/* Finds an element by its rank. The rank argument needs to be 1-based. */
zskiplistNode *zslGetElementByRank(zskiplist *zsl, unsigned long rank) {
    return zslGetElementByRankFromNode(zsl->header, zsl->level - 1, rank);
}

/* Populate the rangespec according to the objects min and max. */
static int zslParseRange(robj *min, robj *max, zrangespec *spec) {
    char *eptr;
    spec->minex = spec->maxex = 0;

    /* Parse the min-max interval. If one of the values is prefixed
     * by the "(" character, it's considered "open". For instance
     * ZRANGEBYSCORE zset (1.5 (2.5 will match min < x < max
     * ZRANGEBYSCORE zset 1.5 2.5 will instead match min <= x <= max */
    if (min->encoding == OBJ_ENCODING_INT) {
        spec->min = (long)min->ptr;
    } else {
        size_t len = sdslen(min->ptr);
        if (((char*)min->ptr)[0] == '(') {
            spec->min = fast_float_strtod((char*)min->ptr+1,len-1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;
            spec->minex = 1;
        } else {
            spec->min = fast_float_strtod((char*)min->ptr,len,&eptr);
            if (eptr[0] != '\0' || isnan(spec->min)) return C_ERR;
        }
    }
    if (max->encoding == OBJ_ENCODING_INT) {
        spec->max = (long)max->ptr;
    } else {
        size_t len = sdslen(max->ptr);
        if (((char*)max->ptr)[0] == '(') {
            spec->max = fast_float_strtod((char*)max->ptr+1,len-1,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;
            spec->maxex = 1;
        } else {
            spec->max = fast_float_strtod((char*)max->ptr,len,&eptr);
            if (eptr[0] != '\0' || isnan(spec->max)) return C_ERR;
        }
    }

    return C_OK;
}

/* ------------------------ Lexicographic ranges ---------------------------- */

/* Parse max or min argument of ZRANGEBYLEX.
  * (foo means foo (open interval)
  * [foo means foo (closed interval)
  * - means the min string possible
  * + means the max string possible
  *
  * If the string is valid the *dest pointer is set to the redis object
  * that will be used for the comparison, and ex will be set to 0 or 1
  * respectively if the item is exclusive or inclusive. C_OK will be
  * returned.
  *
  * If the string is not a valid range C_ERR is returned, and the value
  * of *dest and *ex is undefined. */
static int zslParseLexRangeItem(robj *item, sds *dest, int *ex) {
    char *c = item->ptr;

    switch(c[0]) {
    case '+':
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.maxstring;
        return C_OK;
    case '-':
        if (c[1] != '\0') return C_ERR;
        *ex = 1;
        *dest = shared.minstring;
        return C_OK;
    case '(':
        *ex = 1;
        *dest = sdsnewlen(c+1,sdslen(c)-1);
        return C_OK;
    case '[':
        *ex = 0;
        *dest = sdsnewlen(c+1,sdslen(c)-1);
        return C_OK;
    default:
        return C_ERR;
    }
}

/* Free a lex range structure, must be called only after zslParseLexRange()
 * populated the structure with success (C_OK returned). */
void zslFreeLexRange(zlexrangespec *spec) {
    if (spec->min != shared.minstring &&
        spec->min != shared.maxstring) sdsfree(spec->min);
    if (spec->max != shared.minstring &&
        spec->max != shared.maxstring) sdsfree(spec->max);
}

/* Populate the lex rangespec according to the objects min and max.
 *
 * Return C_OK on success. On error C_ERR is returned.
 * When OK is returned the structure must be freed with zslFreeLexRange(),
 * otherwise no release is needed. */
int zslParseLexRange(robj *min, robj *max, zlexrangespec *spec) {
    /* The range can't be valid if objects are integer encoded.
     * Every item must start with ( or [. */
    if (min->encoding == OBJ_ENCODING_INT ||
        max->encoding == OBJ_ENCODING_INT) return C_ERR;

    spec->min = spec->max = NULL;
    if (zslParseLexRangeItem(min, &spec->min, &spec->minex) == C_ERR ||
        zslParseLexRangeItem(max, &spec->max, &spec->maxex) == C_ERR) {
        zslFreeLexRange(spec);
        return C_ERR;
    } else {
        return C_OK;
    }
}

/* This is just a wrapper to sdscmp() that is able to
 * handle shared.minstring and shared.maxstring as the equivalent of
 * -inf and +inf for strings */
static int sdscmplex(sds a, sds b) {
    if (a == b) return 0;
    if (a == shared.minstring || b == shared.maxstring) return -1;
    if (a == shared.maxstring || b == shared.minstring) return 1;
    return sdscmp(a,b);
}

int zslLexValueGteMin(sds value, zlexrangespec *spec) {
    return spec->minex ?
        (sdscmplex(value,spec->min) > 0) :
        (sdscmplex(value,spec->min) >= 0);
}

int zslLexValueLteMax(sds value, zlexrangespec *spec) {
    return spec->maxex ?
        (sdscmplex(value,spec->max) < 0) :
        (sdscmplex(value,spec->max) <= 0);
}

/* Returns if there is a part of the zset is in the lex range. */
static int zslIsInLexRange(zskiplist *zsl, zlexrangespec *range) {
    zskiplistNode *x;

    /* Test for ranges that will always be empty. */
    int cmp = sdscmplex(range->min,range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
        return 0;
    x = zsl->tail;
    if ((x == NULL) || (!zslLexValueGteMin(zslGetNodeElement(x),range)))
        return 0;
    x = zsl->header->level[0].forward;
    if ((x == NULL) || (!zslLexValueLteMax(zslGetNodeElement(x),range)))
        return 0;
    return 1;
}

/* Find the Nth node that is contained in the specified range. N should be 0-based.
 * Negative N works for reversed order (-1 represents the last element). Returns
 * NULL when no element is contained in the range.
 * If out_rank is not NULL, stores the 1-based rank of the returned node. */
zskiplistNode *zslNthInLexRange(zskiplist *zsl, zlexrangespec *range, long n, unsigned long *out_rank) {
    zskiplistNode *x;
    int i;
    long edge_rank = 0;
    long last_highest_level_rank = 0;
    zskiplistNode *last_highest_level_node = NULL;
    unsigned long rank_diff;

    /* If everything is out of range, return early. */
    if (!zslIsInLexRange(zsl,range)) return NULL;

    /* Go forward while *OUT* of range at level of zsl->level-1. */
    x = zsl->header;
    i = zsl->level - 1;
    while (x->level[i].forward && !zslLexValueGteMin(zslGetNodeElement(x->level[i].forward), range)) {
        edge_rank += zslGetNodeSpanAtLevel(x, i);
        x = x->level[i].forward;
    }
    /* Remember the last node which has zsl->level-1 levels and its rank. */
    last_highest_level_node = x;
    last_highest_level_rank = edge_rank;

    if (n >= 0) {
        for (i = zsl->level - 2; i >= 0; i--) {
            /* Go forward while *OUT* of range. */
            while (x->level[i].forward && !zslLexValueGteMin(zslGetNodeElement(x->level[i].forward), range)) {
                /* Count the rank of the last element smaller than the range. */
                edge_rank += zslGetNodeSpanAtLevel(x, i);
                x = x->level[i].forward;
            }
        }
        /* Check if zsl is long enough. */
        if ((unsigned long)(edge_rank + n) >= zsl->length) return NULL; 
        if (n < ZSKIPLIST_MAX_SEARCH) {
            /* If offset is small, we can just jump node by node */
            /* rank+1 is the first element in range, so we need n+1 steps to reach target. */
            for (i = 0; i < n + 1; i++) { 
                x = x->level[0].forward;
            }
        } else {
            /* If offset is big, we can jump from the last zsl->level-1 node. */
            rank_diff = edge_rank + 1 + n - last_highest_level_rank;
            x = zslGetElementByRankFromNode(last_highest_level_node, zsl->level - 1, rank_diff);
        }
        /* Check if score <= max. */
        if (x && !zslLexValueLteMax(zslGetNodeElement(x),range)) return NULL;
        /* Store rank if requested. For n >= 0, the returned node is at rank edge_rank + n + 1. */
        if (x && out_rank) *out_rank = edge_rank + n + 1;
    } else {
        for (i = zsl->level - 1; i >= 0; i--) {
            /* Go forward while *IN* range. */
            while (x->level[i].forward && zslLexValueLteMax(zslGetNodeElement(x->level[i].forward), range)) {
                /* Count the rank of the last element in range. */
                edge_rank += zslGetNodeSpanAtLevel(x, i);
                x = x->level[i].forward;
            }
        }
        /* Check if the range is big enough. */
        if (edge_rank < -n) return NULL;
        if (n + 1 > -ZSKIPLIST_MAX_SEARCH) {
            /* If offset is small, we can just jump node by node */
            for (i = 0; i < -n - 1; i++) {
                x = x->backward;
            }
        } else {
            /* If offset is big, we can jump from the last zsl->level-1 node. */
            /* rank is the last element in range, n is -1-based, so we need n+1 to count backwards. */
            rank_diff = edge_rank + 1 + n - last_highest_level_rank;
            x = zslGetElementByRankFromNode(last_highest_level_node, zsl->level - 1, rank_diff);
        }
        /* Check if score >= min. */
        if (x && !zslLexValueGteMin(zslGetNodeElement(x), range)) return NULL;
        /* Store rank if requested. For n < 0, the returned node is at rank edge_rank + n + 1. */
        if (x && out_rank) *out_rank = edge_rank + n + 1;
    }

    return x;
}

/*-----------------------------------------------------------------------------
 * Listpack-backed sorted set API
 *----------------------------------------------------------------------------*/

static double zzlStrtod(unsigned char *vstr, unsigned int vlen) {
    return fast_float_strtod((char*)vstr, vlen, NULL);
}

double zzlGetScore(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    double score;

    serverAssert(sptr != NULL);
    vstr = lpGetValue(sptr,&vlen,&vlong);

    if (vstr) {
        score = zzlStrtod(vstr,vlen);
    } else {
        score = vlong;
    }

    return score;
}

/* Return a listpack element as an SDS string. */
sds lpGetObject(unsigned char *sptr) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;

    serverAssert(sptr != NULL);
    vstr = lpGetValue(sptr,&vlen,&vlong);

    if (vstr) {
        return sdsnewlen((char*)vstr,vlen);
    } else {
        return sdsfromlonglong(vlong);
    }
}

/* Compare element in sorted set with given element. */
static int zzlCompareElements(unsigned char *eptr, unsigned char *cstr, unsigned int clen) {
    unsigned char *vstr;
    unsigned int vlen;
    long long vlong;
    unsigned char vbuf[32];
    int minlen, cmp;

    vstr = lpGetValue(eptr,&vlen,&vlong);
    if (vstr == NULL) {
        /* Store string representation of long long in buf. */
        vlen = ll2string((char*)vbuf,sizeof(vbuf),vlong);
        vstr = vbuf;
    }

    minlen = (vlen < clen) ? vlen : clen;
    cmp = memcmp(vstr,cstr,minlen);
    if (cmp == 0) return vlen-clen;
    return cmp;
}

static unsigned int zzlLength(unsigned char *zl) {
    return lpLength(zl)/2;
}

/* Move to next entry based on the values in eptr and sptr. Both are set to
 * NULL when there is no next entry. */
void zzlNext(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    _eptr = lpNext(zl,*sptr);
    if (_eptr != NULL) {
        _sptr = lpNext(zl,_eptr);
        serverAssert(_sptr != NULL);
    } else {
        /* No next entry. */
        _sptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/* Move to the previous entry based on the values in eptr and sptr. Both are
 * set to NULL when there is no prev entry. */
void zzlPrev(unsigned char *zl, unsigned char **eptr, unsigned char **sptr) {
    unsigned char *_eptr, *_sptr;
    serverAssert(*eptr != NULL && *sptr != NULL);

    _sptr = lpPrev(zl,*eptr);
    if (_sptr != NULL) {
        _eptr = lpPrev(zl,_sptr);
        serverAssert(_eptr != NULL);
    } else {
        /* No previous entry. */
        _eptr = NULL;
    }

    *eptr = _eptr;
    *sptr = _sptr;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInRange and zzlLastInRange. */
static int zzlIsInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *p;
    double score;

    /* Test for ranges that will always be empty. */
    if (range->min > range->max ||
            (range->min == range->max && (range->minex || range->maxex)))
        return 0;

    p = lpSeek(zl,-1); /* Last score. */
    if (p == NULL) return 0; /* Empty sorted set */
    score = zzlGetScore(p);
    if (!zslValueGteMin(score,range))
        return 0;

    p = lpSeek(zl,1); /* First score. */
    serverAssert(p != NULL);
    score = zzlGetScore(p);
    if (!zslValueLteMax(score,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = lpSeek(zl,0), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl,range)) return NULL;

    while (eptr != NULL) {
        sptr = lpNext(zl,eptr);
        serverAssert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zslValueGteMin(score,range)) {
            /* Check if score <= max. */
            if (zslValueLteMax(score,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        eptr = lpNext(zl,sptr);
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlLastInRange(unsigned char *zl, zrangespec *range) {
    unsigned char *eptr = lpSeek(zl,-2), *sptr;
    double score;

    /* If everything is out of range, return early. */
    if (!zzlIsInRange(zl,range)) return NULL;

    while (eptr != NULL) {
        sptr = lpNext(zl,eptr);
        serverAssert(sptr != NULL);

        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,range)) {
            /* Check if score >= min. */
            if (zslValueGteMin(score,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = lpPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = lpPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

int zzlLexValueGteMin(unsigned char *p, zlexrangespec *spec) {
    sds value = lpGetObject(p);
    int res = zslLexValueGteMin(value,spec);
    sdsfree(value);
    return res;
}

int zzlLexValueLteMax(unsigned char *p, zlexrangespec *spec) {
    sds value = lpGetObject(p);
    int res = zslLexValueLteMax(value,spec);
    sdsfree(value);
    return res;
}

/* Returns if there is a part of the zset is in range. Should only be used
 * internally by zzlFirstInLexRange and zzlLastInLexRange. */
static int zzlIsInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *p;

    /* Test for ranges that will always be empty. */
    int cmp = sdscmplex(range->min,range->max);
    if (cmp > 0 || (cmp == 0 && (range->minex || range->maxex)))
        return 0;

    p = lpSeek(zl,-2); /* Last element. */
    if (p == NULL) return 0;
    if (!zzlLexValueGteMin(p,range))
        return 0;

    p = lpSeek(zl,0); /* First element. */
    serverAssert(p != NULL);
    if (!zzlLexValueLteMax(p,range))
        return 0;

    return 1;
}

/* Find pointer to the first element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlFirstInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = lpSeek(zl,0), *sptr;

    /* If everything is out of range, return early. */
    if (!zzlIsInLexRange(zl,range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueGteMin(eptr,range)) {
            /* Check if score <= max. */
            if (zzlLexValueLteMax(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to next element. */
        sptr = lpNext(zl,eptr); /* This element score. Skip it. */
        serverAssert(sptr != NULL);
        eptr = lpNext(zl,sptr); /* Next element. */
    }

    return NULL;
}

/* Find pointer to the last element contained in the specified lex range.
 * Returns NULL when no element is contained in the range. */
unsigned char *zzlLastInLexRange(unsigned char *zl, zlexrangespec *range) {
    unsigned char *eptr = lpSeek(zl,-2), *sptr;

    /* If everything is out of range, return early. */
    if (!zzlIsInLexRange(zl,range)) return NULL;

    while (eptr != NULL) {
        if (zzlLexValueLteMax(eptr,range)) {
            /* Check if score >= min. */
            if (zzlLexValueGteMin(eptr,range))
                return eptr;
            return NULL;
        }

        /* Move to previous element by moving to the score of previous element.
         * When this returns NULL, we know there also is no element. */
        sptr = lpPrev(zl,eptr);
        if (sptr != NULL)
            serverAssert((eptr = lpPrev(zl,sptr)) != NULL);
        else
            eptr = NULL;
    }

    return NULL;
}

static unsigned char *zzlFind(unsigned char *lp, sds ele, double *score) {
    unsigned char *eptr, *sptr;

    if ((eptr = lpFirst(lp)) == NULL) return NULL;
    eptr = lpFind(lp, eptr, (unsigned char*)ele, sdslen(ele), 1);
    if (eptr) {
        sptr = lpNext(lp,eptr);
        serverAssert(sptr != NULL);

        /* Matching element, pull out score. */
        if (score != NULL) *score = zzlGetScore(sptr);
        return eptr;
    }

    return NULL;
}

/* Delete (element,score) pair from listpack. Use local copy of eptr because we
 * don't want to modify the one given as argument. */
static unsigned char *zzlDelete(unsigned char *zl, unsigned char *eptr) {
    return lpDeleteRangeWithEntry(zl,&eptr,2);
}

static unsigned char *zzlInsertAt(unsigned char *zl, unsigned char *eptr, sds ele, double score) {
    char scorebuf[MAX_D2STRING_CHARS];
    int scorelen = 0;
    long long lscore;
    int score_is_long = double2ll(score, &lscore);
    if (!score_is_long)
        scorelen = d2string(scorebuf,sizeof(scorebuf),score);

    listpackEntry entries[2];
    entries[0].sval = (unsigned char*)ele;
    entries[0].slen = sdslen(ele);
    if (score_is_long) {
        entries[1].sval = NULL;
        entries[1].lval = lscore;
    } else {
        entries[1].sval = (unsigned char*)scorebuf;
        entries[1].slen = scorelen;
    }

    if (eptr == NULL)
        zl = lpBatchAppend(zl, entries, 2);
    else
        zl = lpBatchInsert(zl, eptr, LP_BEFORE, entries, 2, NULL);

    return zl;
}

/* Insert (element,score) pair in listpack. This function assumes the element is
 * not yet present in the list. */
unsigned char *zzlInsert(unsigned char *zl, sds ele, double score) {
    unsigned char *eptr = lpSeek(zl,0), *sptr;
    double s;

    while (eptr != NULL) {
        sptr = lpNext(zl,eptr);
        serverAssert(sptr != NULL);
        s = zzlGetScore(sptr);

        if (s > score) {
            /* First element with score larger than score for element to be
             * inserted. This means we should take its spot in the list to
             * maintain ordering. */
            zl = zzlInsertAt(zl,eptr,ele,score);
            break;
        } else if (s == score) {
            /* Ensure lexicographical ordering for elements. */
            if (zzlCompareElements(eptr,(unsigned char*)ele,sdslen(ele)) > 0) {
                zl = zzlInsertAt(zl,eptr,ele,score);
                break;
            }
        }

        /* Move to next element. */
        eptr = lpNext(zl,sptr);
    }

    /* Push on tail of list when it was not yet inserted. */
    if (eptr == NULL)
        zl = zzlInsertAt(zl,NULL,ele,score);
    return zl;
}

static unsigned char *zzlDeleteRangeByScore(unsigned char *zl, zrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    double score;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the listpack is deleted, eptr will be NULL. */
    while (eptr && (sptr = lpNext(zl,eptr)) != NULL) {
        score = zzlGetScore(sptr);
        if (zslValueLteMax(score,range)) {
            /* Delete both the element and the score. */
            zl = lpDeleteRangeWithEntry(zl,&eptr,2);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

static unsigned char *zzlDeleteRangeByLex(unsigned char *zl, zlexrangespec *range, unsigned long *deleted) {
    unsigned char *eptr, *sptr;
    unsigned long num = 0;

    if (deleted != NULL) *deleted = 0;

    eptr = zzlFirstInLexRange(zl,range);
    if (eptr == NULL) return zl;

    /* When the tail of the listpack is deleted, eptr will be NULL. */
    while (eptr && (sptr = lpNext(zl,eptr)) != NULL) {
        if (zzlLexValueLteMax(eptr,range)) {
            /* Delete both the element and the score. */
            zl = lpDeleteRangeWithEntry(zl,&eptr,2);
            num++;
        } else {
            /* No longer in range. */
            break;
        }
    }

    if (deleted != NULL) *deleted = num;
    return zl;
}

/* Delete all the elements with rank between start and end from the skiplist.
 * Start and end are inclusive. Note that start and end need to be 1-based */
static unsigned char *zzlDeleteRangeByRank(unsigned char *zl, unsigned int start, unsigned int end, unsigned long *deleted) {
    unsigned int num = (end-start)+1;
    if (deleted) *deleted = num;
    zl = lpDeleteRange(zl,2*(start-1),2*num);
    return zl;
}

/*-----------------------------------------------------------------------------
 * Common sorted set API
 *----------------------------------------------------------------------------*/

unsigned long zsetLength(const robj *zobj) {
    unsigned long length = 0;
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        length = zzlLength(zobj->ptr);
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        length = ((const zset*)zobj->ptr)->zsl->length;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return length;
}

size_t zsetAllocSize(const robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_ZSET);
    size_t size = 0;
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        size = lpBytes(o->ptr);
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        dict *d = ((zset*)o->ptr)->dict;
        zskiplist *zsl = ((zset*)o->ptr)->zsl;
        size = sizeof(zset) + zslAllocSize(zsl) +
            sizeof(dict) + dictMemUsage(d);
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return size;
}

/* Factory method to return a zset.
 *
 * The size hint indicates approximately how many items will be added,
 * and the value len hint indicates the approximate individual size of the added elements,
 * they are used to determine the initial representation.
 *
 * If the hints are not known, and underestimation or 0 is suitable. 
 * We should never pass a negative value because it will convert to a very large unsigned number. */
robj *zsetTypeCreate(size_t size_hint, size_t val_len_hint) {
    if (size_hint <= server.zset_max_listpack_entries &&
        val_len_hint <= server.zset_max_listpack_value)
    {
        return createZsetListpackObject();
    }

    robj *zobj = createZsetObject();
    zset *zs = zobj->ptr;
    dictExpand(zs->dict, size_hint);
    return zobj;
}

/* Check if the existing zset should be converted to another encoding based off the
 * the size hint. */
void zsetTypeMaybeConvert(robj *zobj, size_t size_hint) {
    if (zobj->encoding == OBJ_ENCODING_LISTPACK &&
        size_hint > server.zset_max_listpack_entries)
    {
        zsetConvertAndExpand(zobj, OBJ_ENCODING_SKIPLIST, size_hint);
    }
}

/* Convert the zset to specified encoding. The zset dict (when converting
 * to a skiplist) is presized to hold the number of elements in the original
 * zset. */
void zsetConvert(robj *zobj, int encoding) {
    zsetConvertAndExpand(zobj, encoding, zsetLength(zobj));
}

/* Converts a zset to the specified encoding, pre-sizing it for 'cap' elements. */
void zsetConvertAndExpand(robj *zobj, int encoding, unsigned long cap) {
    zset *zs;
    zskiplistNode *node, *next;
    sds ele;
    double score;

    if (zobj->encoding == encoding) return;
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr;
        unsigned int vlen;
        long long vlong;

        if (encoding != OBJ_ENCODING_SKIPLIST)
            serverPanic("Unknown target encoding");

        zs = zmalloc(sizeof(*zs));
        zs->dict = dictCreate(&zsetDictType);
        zs->zsl = zslCreate();

        /* Presize the dict to avoid rehashing */
        dictExpand(zs->dict, cap);

        eptr = lpSeek(zl,0);
        if (eptr != NULL) {
            sptr = lpNext(zl,eptr);
            serverAssertWithInfo(NULL,zobj,sptr != NULL);
        }

        while (eptr != NULL) {
            score = zzlGetScore(sptr);
            vstr = lpGetValue(eptr,&vlen,&vlong);
            if (vstr == NULL)
                ele = sdsfromlonglong(vlong);
            else
                ele = sdsnewlen((char*)vstr,vlen);

            node = zslInsert(zs->zsl,score,ele);
            serverAssert(dictAdd(zs->dict, node, NULL) == DICT_OK);
            sdsfree(ele); /* zslInsert copied it, we can free our copy */
            zzlNext(zl,&eptr,&sptr);
        }

        zfree(zobj->ptr);
        zobj->ptr = zs;
        zobj->encoding = OBJ_ENCODING_SKIPLIST;
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        unsigned char *zl = lpNew(0);

        if (encoding != OBJ_ENCODING_LISTPACK)
            serverPanic("Unknown target encoding");

        /* Approach similar to zslFree(), since we want to free the skiplist at
         * the same time as creating the listpack. */
        zs = zobj->ptr;
        dictRelease(zs->dict);
        node = zs->zsl->header->level[0].forward;
        zfree(zs->zsl->header);

        while (node) {
            zl = zzlInsertAt(zl,NULL,zslGetNodeElement(node),node->score);
            next = node->level[0].forward;
            zslFreeNode(zs->zsl, node);
            node = next;
        }

        zfree(zs->zsl);
        zfree(zs);
        zobj->ptr = zl;
        zobj->encoding = OBJ_ENCODING_LISTPACK;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/* Convert the sorted set object into a listpack if it is not already a listpack
 * and if the number of elements and the maximum element size and total elements size
 * are within the expected ranges. */
void zsetConvertToListpackIfNeeded(robj *zobj, size_t maxelelen, size_t totelelen) {
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) return;
    zset *zset = zobj->ptr;

    if (zset->zsl->length <= server.zset_max_listpack_entries &&
        maxelelen <= server.zset_max_listpack_value &&
        lpSafeToAdd(NULL, totelelen))
    {
        zsetConvert(zobj,OBJ_ENCODING_LISTPACK);
    }
}

/* Return (by reference) the score of the specified member of the sorted set
 * storing it into *score. If the element does not exist C_ERR is returned
 * otherwise C_OK is returned and *score is correctly populated.
 * If 'zobj' or 'member' is NULL, C_ERR is returned. */
int zsetScore(robj *zobj, sds member, double *score) {
    if (!zobj || !member) return C_ERR;

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        if (zzlFind(zobj->ptr, member, score) == NULL) return C_ERR;
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        dictEntry *de = dictFind(zs->dict, member);
        if (de == NULL) return C_ERR;
        zskiplistNode *znode = dictGetKey(de);
        *score = znode->score;
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return C_OK;
}

/* Add a new element or update the score of an existing element in a sorted
 * set, regardless of its encoding.
 *
 * The set of flags change the command behavior. 
 *
 * The input flags are the following:
 *
 * ZADD_INCR: Increment the current element score by 'score' instead of updating
 *            the current element score. If the element does not exist, we
 *            assume 0 as previous score.
 * ZADD_NX:   Perform the operation only if the element does not exist.
 * ZADD_XX:   Perform the operation only if the element already exist.
 * ZADD_GT:   Perform the operation on existing elements only if the new score is 
 *            greater than the current score.
 * ZADD_LT:   Perform the operation on existing elements only if the new score is 
 *            less than the current score.
 *
 * When ZADD_INCR is used, the new score of the element is stored in
 * '*newscore' if 'newscore' is not NULL.
 *
 * The returned flags are the following:
 *
 * ZADD_NAN:     The resulting score is not a number.
 * ZADD_ADDED:   The element was added (not present before the call).
 * ZADD_UPDATED: The element score was updated.
 * ZADD_NOP:     No operation was performed because of NX or XX.
 *
 * Return value:
 *
 * The function returns 1 on success, and sets the appropriate flags
 * ADDED or UPDATED to signal what happened during the operation (note that
 * none could be set if we re-added an element using the same score it used
 * to have, or in the case a zero increment is used).
 *
 * The function returns 0 on error, currently only when the increment
 * produces a NAN condition, or when the 'score' value is NAN since the
 * start.
 *
 * The command as a side effect of adding a new element may convert the sorted
 * set internal encoding from listpack to hashtable+skiplist.
 *
 * Memory management of 'ele':
 *
 * The function does not take ownership of the 'ele' SDS string, but copies
 * it if needed. */
int zsetAdd(robj *zobj, double score, sds ele, int in_flags, int *out_flags, double *newscore) {
    /* Turn options into simple to check vars. */
    int incr = (in_flags & ZADD_IN_INCR) != 0;
    int nx = (in_flags & ZADD_IN_NX) != 0;
    int xx = (in_flags & ZADD_IN_XX) != 0;
    int gt = (in_flags & ZADD_IN_GT) != 0;
    int lt = (in_flags & ZADD_IN_LT) != 0;
    *out_flags = 0; /* We'll return our response flags. */
    double curscore;

    /* NaN as input is an error regardless of all the other parameters. */
    if (isnan(score)) {
        *out_flags = ZADD_OUT_NAN;
        return 0;
    }

    /* Update the sorted set according to its encoding. */
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,&curscore)) != NULL) {
            /* NX? Return, same element already exists. */
            if (nx) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            /* Prepare the score for the increment if needed. */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *out_flags |= ZADD_OUT_NAN;
                    return 0;
                }
            }

            /* GT/LT? Only update if score is greater/less than current. */
            if ((lt && score >= curscore) || (gt && score <= curscore)) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            if (newscore) *newscore = score;

            /* Remove and re-insert when score changed. */
            if (score != curscore) {
                zobj->ptr = zzlDelete(zobj->ptr,eptr);
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                *out_flags |= ZADD_OUT_UPDATED;
            }
            return 1;
        } else if (!xx) {
            /* check if the element is too large or the list
             * becomes too long *before* executing zzlInsert. */
            if (zzlLength(zobj->ptr)+1 > server.zset_max_listpack_entries ||
                sdslen(ele) > server.zset_max_listpack_value ||
                !lpSafeToAdd(zobj->ptr, sdslen(ele)))
            {
                zsetConvertAndExpand(zobj, OBJ_ENCODING_SKIPLIST, zsetLength(zobj) + 1);
            } else {
                zobj->ptr = zzlInsert(zobj->ptr,ele,score);
                if (newscore) *newscore = score;
                *out_flags |= ZADD_OUT_ADDED;
                return 1;
            }
        } else {
            *out_flags |= ZADD_OUT_NOP;
            return 1;
        }
    }

    /* Note that the above block handling listpack would have either returned or
     * converted the key to skiplist. */
    if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplistNode *znode;
        dictEntry *de;
        dictEntryLink bucket, link;

        /* Use dictFindLink to find the element and get the bucket for potential insertion.
         * This avoids a second lookup in dictAdd() if the element doesn't exist. */
        link = dictFindLink(zs->dict, ele, &bucket);

        if (link != NULL) {
            /* Element exists - get the dictEntry from the link */
            de = *link;

            /* NX? Return, same element already exists. */
            if (nx) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            /* Get the node pointer from dict entry */
            znode = dictGetKey(de);
            curscore = znode->score;

            /* Prepare the score for the increment if needed. */
            if (incr) {
                score += curscore;
                if (isnan(score)) {
                    *out_flags |= ZADD_OUT_NAN;
                    return 0;
                }
            }

            /* GT/LT? Only update if score is greater/less than current. */
            if ((lt && score >= curscore) || (gt && score <= curscore)) {
                *out_flags |= ZADD_OUT_NOP;
                return 1;
            }

            if (newscore) *newscore = score;

            /* Remove and re-insert when score changes. */
            if (score != curscore) {
                zslUpdateScore(zs->zsl, znode, score);
                /* Note that we did not remove the original element from
                 * the hash table representing the sorted set, so we don't
                 * need to update the dict - the node pointer stays the same. */
                *out_flags |= ZADD_OUT_UPDATED;
            }
            return 1;
        } else if (!xx) {
            /* Element doesn't exist - create node with embedded sds and add to skiplist */
            znode = zslInsert(zs->zsl, score, ele);

            /* Add node pointer to dict using the bucket we already found */
            dictSetKeyAtLink(zs->dict, znode, &bucket, 1);

            *out_flags |= ZADD_OUT_ADDED;
            if (newscore) *newscore = score;
            return 1;
        } else {
            *out_flags |= ZADD_OUT_NOP;
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* Never reached. */
}

/* Deletes the element 'ele' from the sorted set encoded as a skiplist+dict,
 * returning 1 if the element existed and was deleted, 0 otherwise (the
 * element was not there). It does not resize the dict after deleting the
 * element. */
static int zsetRemoveFromSkiplist(zset *zs, sds ele) {
    dictEntry *de;

    de = dictUnlink(zs->dict,ele);
    if (de != NULL) {
        /* Get the node and score in order to delete from the skiplist later. */
        zskiplistNode *znode = dictGetKey(de);

        /* Delete from the hash table and later from the skiplist.
         * Note that the order is important: deleting from the skiplist
         * actually releases the SDS string representing the element,
         * which is shared between the skiplist and the hash table, so
         * we need to delete from the skiplist as the final step. */
        dictFreeUnlinkedEntry(zs->dict,de);

        /* Delete from skiplist. */
        zslDelete(zs->zsl, znode);

        return 1;
    }

    return 0;
}

/* Delete the element 'ele' from the sorted set, returning 1 if the element
 * existed and was deleted, 0 otherwise (the element was not there). */
int zsetDel(robj *zobj, sds ele) {
    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *eptr;

        if ((eptr = zzlFind(zobj->ptr,ele,NULL)) != NULL) {
            zobj->ptr = zzlDelete(zobj->ptr,eptr);
            return 1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        if (zsetRemoveFromSkiplist(zs, ele)) {
            return 1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return 0; /* No such element found. */
}

/* Given a sorted set object returns the 0-based rank of the object or
 * -1 if the object does not exist.
 *
 * For rank we mean the position of the element in the sorted collection
 * of elements. So the first element has rank 0, the second rank 1, and so
 * forth up to length-1 elements.
 *
 * If 'reverse' is false, the rank is returned considering as first element
 * the one with the lowest score. Otherwise if 'reverse' is non-zero
 * the rank is computed considering as element with rank 0 the one with
 * the highest score. */
long zsetRank(robj *zobj, sds ele, int reverse, double *output_score) {
    unsigned long llen;
    unsigned long rank;

    llen = zsetLength(zobj);

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;

        eptr = lpSeek(zl,0);
        serverAssert(eptr != NULL);
        sptr = lpNext(zl,eptr);
        serverAssert(sptr != NULL);
        const size_t ele_len = sdslen(ele);
        long long cached_val = 0;
        int cached_valid = 0;
        rank = 1;
        while(eptr != NULL) {
            if (lpCompare(eptr,(unsigned char*)ele,ele_len,&cached_val,&cached_valid))
                break;
            rank++;
            zzlNext(zl,&eptr,&sptr);
        }

        if (eptr != NULL) {
            if (output_score) 
                *output_score = zzlGetScore(sptr);
            if (reverse)
                return llen-rank;
            else
                return rank-1;
        } else {
            return -1;
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        dictEntry *de;

        de = dictFind(zs->dict,ele);
        if (de != NULL) {
            zskiplistNode *n = dictGetKey(de);
            rank = zslGetRankByNode(zsl, n);
            /* Existing elements always have a rank. */
            serverAssert(rank != 0);
            if (output_score)
                *output_score = n->score;
            if (reverse)
                return llen-rank;
            else
                return rank-1;
        } else {
            return -1;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
}

/* This is a helper function for the COPY command.
 * Duplicate a sorted set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *zsetDup(robj *o) {
    robj *zobj;
    zset *zs;
    zset *new_zs;

    serverAssert(o->type == OBJ_ZSET);

    /* Create a new sorted set object that have the same encoding as the original object's encoding */
    if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = o->ptr;
        size_t sz = lpBytes(zl);
        unsigned char *new_zl = zmalloc(sz);
        memcpy(new_zl, zl, sz);
        zobj = createObject(OBJ_ZSET, new_zl);
        zobj->encoding = OBJ_ENCODING_LISTPACK;
    } else if (o->encoding == OBJ_ENCODING_SKIPLIST) {
        zobj = createZsetObject();
        zs = o->ptr;
        new_zs = zobj->ptr;
        dictExpand(new_zs->dict,dictSize(zs->dict));
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;
        sds ele;
        long llen = zsetLength(o);

        /* We copy the skiplist elements from the greatest to the
         * smallest (that's trivial since the elements are already ordered in
         * the skiplist): this improves the load process, since the next loaded
         * element will always be the smaller, so adding to the skiplist
         * will always immediately stop at the head, making the insertion
         * O(1) instead of O(log(N)). */
        ln = zsl->tail;
        while (llen--) {
            ele = zslGetNodeElement(ln);
            zskiplistNode *znode = zslInsert(new_zs->zsl,ln->score,ele);
            dictAdd(new_zs->dict, znode, NULL);
            ln = ln->backward;
        }
    } else {
        serverPanic("Unknown sorted set encoding");
    }
    return zobj;
}

/* Create a new sds string from the listpack entry. */
sds zsetSdsFromListpackEntry(listpackEntry *e) {
    return e->sval ? sdsnewlen(e->sval, e->slen) : sdsfromlonglong(e->lval);
}


/* Return random element from a non empty zset.
 * 'key' and 'val' will be set to hold the element.
 * The memory in `key` is not to be freed or modified by the caller.
 * 'score' can be NULL in which case it's not extracted. */
void zsetTypeRandomElement(robj *zsetobj, unsigned long zsetsize, listpackEntry *key, double *score) {
    if (zsetobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zsetobj->ptr;
        dictEntry *de = dictGetFairRandomKey(zs->dict);
        zskiplistNode *znode = dictGetKey(de);
        sds s = zslGetNodeElement(znode);
        key->sval = (unsigned char*)s;
        key->slen = sdslen(s);
        if (score) {
            *score = znode->score;
        }
    } else if (zsetobj->encoding == OBJ_ENCODING_LISTPACK) {
        listpackEntry val;
        lpRandomPair(zsetobj->ptr, zsetsize, key, &val, 2);
        if (score) {
            if (val.sval) {
                *score = zzlStrtod(val.sval,val.slen);
            } else {
                *score = (double)val.lval;
            }
        }
    } else {
        serverPanic("Unknown zset encoding");
    }
}

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

void zuiInitIterator(zsetopsrc *op) {
    if (op->subject == NULL)
        return;

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            it->is.is = op->subject->ptr;
            it->is.ii = 0;
        } else if (op->encoding == OBJ_ENCODING_HT) {
            it->ht.dict = op->subject->ptr;
            it->ht.di = dictGetIterator(op->subject->ptr);
            it->ht.de = dictNext(it->ht.di);
        } else if (op->encoding == OBJ_ENCODING_LISTPACK) {
            it->lp.lp = op->subject->ptr;
            it->lp.p = lpFirst(it->lp.lp);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        /* Sorted sets are traversed in reverse order to optimize for
         * the insertion of the elements in a new list as in
         * ZDIFF/ZINTER/ZUNION */
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            it->zl.zl = op->subject->ptr;
            it->zl.eptr = lpSeek(it->zl.zl,-2);
            if (it->zl.eptr != NULL) {
                it->zl.sptr = lpNext(it->zl.zl,it->zl.eptr);
                serverAssert(it->zl.sptr != NULL);
            }
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            it->sl.zs = op->subject->ptr;
            it->sl.node = it->sl.zs->zsl->tail;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

void zuiClearIterator(zsetopsrc *op) {
    if (op->subject == NULL)
        return;

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            UNUSED(it); /* skip */
        } else if (op->encoding == OBJ_ENCODING_HT) {
            dictReleaseIterator(it->ht.di);
        } else if (op->encoding == OBJ_ENCODING_LISTPACK) {
            UNUSED(it);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            UNUSED(it); /* skip */
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            UNUSED(it); /* skip */
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

void zuiDiscardDirtyValue(zsetopval *val) {
    if (val->flags & OPVAL_DIRTY_SDS) {
        sdsfree(val->ele);
        val->ele = NULL;
        val->flags &= ~OPVAL_DIRTY_SDS;
    }
}

unsigned long zuiLength(zsetopsrc *op) {
    if (op->subject == NULL)
        return 0;

    if (op->type == OBJ_SET) {
        return setTypeSize(op->subject);
    } else if (op->type == OBJ_ZSET) {
        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            return zzlLength(op->subject->ptr);
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = op->subject->ptr;
            return zs->zsl->length;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

/* Check if the current value is valid. If so, store it in the passed structure
 * and move to the next element. If not valid, this means we have reached the
 * end of the structure and can abort. */
int zuiNext(zsetopsrc *op, zsetopval *val) {
    if (op->subject == NULL)
        return 0;

    zuiDiscardDirtyValue(val);

    memset(val,0,sizeof(zsetopval));

    if (op->type == OBJ_SET) {
        iterset *it = &op->iter.set;
        if (op->encoding == OBJ_ENCODING_INTSET) {
            int64_t ell;

            if (!intsetGet(it->is.is,it->is.ii,&ell))
                return 0;
            val->ell = ell;
            val->score = 1.0;

            /* Move to next element. */
            it->is.ii++;
        } else if (op->encoding == OBJ_ENCODING_HT) {
            if (it->ht.de == NULL)
                return 0;
            val->ele = dictGetKey(it->ht.de);
            val->score = 1.0;

            /* Move to next element. */
            it->ht.de = dictNext(it->ht.di);
        } else if (op->encoding == OBJ_ENCODING_LISTPACK) {
            if (it->lp.p == NULL)
                return 0;
            val->estr = lpGetValue(it->lp.p, &val->elen, &val->ell);
            val->score = 1.0;

            /* Move to next element. */
            it->lp.p = lpNext(it->lp.lp, it->lp.p);
        } else {
            serverPanic("Unknown set encoding");
        }
    } else if (op->type == OBJ_ZSET) {
        iterzset *it = &op->iter.zset;
        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            /* No need to check both, but better be explicit. */
            if (it->zl.eptr == NULL || it->zl.sptr == NULL)
                return 0;
            val->estr = lpGetValue(it->zl.eptr,&val->elen,&val->ell);
            val->score = zzlGetScore(it->zl.sptr);

            /* Move to next element (going backwards, see zuiInitIterator). */
            zzlPrev(it->zl.zl,&it->zl.eptr,&it->zl.sptr);
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            if (it->sl.node == NULL)
                return 0;
            val->ele = zslGetNodeElement(it->sl.node);
            val->score = it->sl.node->score;

            /* Move to next element. (going backwards, see zuiInitIterator) */
            it->sl.node = it->sl.node->backward;
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
    return 1;
}

int zuiLongLongFromValue(zsetopval *val) {
    if (!(val->flags & OPVAL_DIRTY_LL)) {
        val->flags |= OPVAL_DIRTY_LL;

        if (val->ele != NULL) {
            if (string2ll(val->ele,sdslen(val->ele),&val->ell))
                val->flags |= OPVAL_VALID_LL;
        } else if (val->estr != NULL) {
            if (string2ll((char*)val->estr,val->elen,&val->ell))
                val->flags |= OPVAL_VALID_LL;
        } else {
            /* The long long was already set, flag as valid. */
            val->flags |= OPVAL_VALID_LL;
        }
    }
    return val->flags & OPVAL_VALID_LL;
}

sds zuiSdsFromValue(zsetopval *val) {
    if (val->ele == NULL) {
        if (val->estr != NULL) {
            val->ele = sdsnewlen((char*)val->estr,val->elen);
        } else {
            val->ele = sdsfromlonglong(val->ell);
        }
        val->flags |= OPVAL_DIRTY_SDS;
    }
    return val->ele;
}

/* This is different from zuiSdsFromValue since returns a new SDS string
 * which is up to the caller to free. */
sds zuiNewSdsFromValue(zsetopval *val) {
    if (val->flags & OPVAL_DIRTY_SDS) {
        /* We have already one to return! */
        sds ele = val->ele;
        val->flags &= ~OPVAL_DIRTY_SDS;
        val->ele = NULL;
        return ele;
    } else if (val->ele) {
        return sdsdup(val->ele);
    } else if (val->estr) {
        return sdsnewlen((char*)val->estr,val->elen);
    } else {
        return sdsfromlonglong(val->ell);
    }
}

int zuiBufferFromValue(zsetopval *val) {
    if (val->estr == NULL) {
        if (val->ele != NULL) {
            val->elen = sdslen(val->ele);
            val->estr = (unsigned char*)val->ele;
        } else {
            val->elen = ll2string((char*)val->_buf,sizeof(val->_buf),val->ell);
            val->estr = val->_buf;
        }
    }
    return 1;
}

/* Find value pointed to by val in the source pointer to by op. When found,
 * return 1 and store its score in target. Return 0 otherwise. */
int zuiFind(zsetopsrc *op, zsetopval *val, double *score) {
    if (op->subject == NULL)
        return 0;

    if (op->type == OBJ_SET) {
        char *str = val->ele ? val->ele : (char *)val->estr;
        size_t len = val->ele ? sdslen(val->ele) : val->elen;
        if (setTypeIsMemberAux(op->subject, str, len, val->ell, val->ele != NULL)) {
            *score = 1.0;
            return 1;
        } else {
            return 0;
        }
    } else if (op->type == OBJ_ZSET) {
        zuiSdsFromValue(val);

        if (op->encoding == OBJ_ENCODING_LISTPACK) {
            if (zzlFind(op->subject->ptr,val->ele,score) != NULL) {
                /* Score is already set by zzlFind. */
                return 1;
            } else {
                return 0;
            }
        } else if (op->encoding == OBJ_ENCODING_SKIPLIST) {
            zset *zs = op->subject->ptr;
            dictEntry *de;
            if ((de = dictFind(zs->dict,val->ele)) != NULL) {
                zskiplistNode *znode = dictGetKey(de);
                *score = znode->score;
                return 1;
            } else {
                return 0;
            }
        } else {
            serverPanic("Unknown sorted set encoding");
        }
    } else {
        serverPanic("Unsupported type");
    }
}

int zuiCompareByCardinality(const void *s1, const void *s2) {
    unsigned long first = zuiLength((zsetopsrc*)s1);
    unsigned long second = zuiLength((zsetopsrc*)s2);
    if (first > second) return 1;
    if (first < second) return -1;
    return 0;
}

static int zuiCompareByRevCardinality(const void *s1, const void *s2) {
    return zuiCompareByCardinality(s1, s2) * -1;
}

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

static size_t zsetDictGetMaxElementLength(dict *d, size_t *totallen) {
    dictIterator di;
    dictEntry *de;
    size_t maxelelen = 0;

    dictInitIterator(&di, d);

    while((de = dictNext(&di)) != NULL) {
        /* Extract sds from the node (key is zskiplistNode*) */
        zskiplistNode *znode = dictGetKey(de);
        sds ele = zslGetNodeElement(znode);
        if (sdslen(ele) > maxelelen) maxelelen = sdslen(ele);
        if (totallen)
            (*totallen) += sdslen(ele);
    }

    dictResetIterator(&di);

    return maxelelen;
}

static void zdiffAlgorithm1(zsetopsrc *src, long setnum, zset *dstzset, size_t *maxelelen, size_t *totelelen) {
    /* DIFF Algorithm 1:
     *
     * We perform the diff by iterating all the elements of the first set,
     * and only adding it to the target set if the element does not exist
     * into all the other sets.
     *
     * This way we perform at max N*M operations, where N is the size of
     * the first set, and M the number of sets.
     *
     * There is also a O(K*log(K)) cost for adding the resulting elements
     * to the target set, where K is the final size of the target set.
     *
     * The final complexity of this algorithm is O(N*M + K*log(K)). */
    int j;
    zsetopval zval;
    zskiplistNode *znode;
    sds tmp;

    /* With algorithm 1 it is better to order the sets to subtract
     * by decreasing size, so that we are more likely to find
     * duplicated elements ASAP. */
    qsort(src+1,setnum-1,sizeof(zsetopsrc),zuiCompareByRevCardinality);

    memset(&zval, 0, sizeof(zval));
    zuiInitIterator(&src[0]);
    while (zuiNext(&src[0],&zval)) {
        double value;
        int exists = 0;

        for (j = 1; j < setnum; j++) {
            /* It is not safe to access the zset we are
             * iterating, so explicitly check for equal object.
             * This check isn't really needed anymore since we already
             * check for a duplicate set in the zsetChooseDiffAlgorithm
             * function, but we're leaving it for future-proofing. */
            if (src[j].subject == src[0].subject ||
                zuiFind(&src[j],&zval,&value)) {
                exists = 1;
                break;
            }
        }

        if (!exists) {
            tmp = zuiNewSdsFromValue(&zval);
            znode = zslInsert(dstzset->zsl,zval.score,tmp);
            dictAdd(dstzset->dict, znode, NULL);
            if (sdslen(tmp) > *maxelelen) *maxelelen = sdslen(tmp);
            (*totelelen) += sdslen(tmp);
            sdsfree(tmp); /* zslInsert copied it, we can free our copy */
        }
    }
    zuiClearIterator(&src[0]);
}


static void zdiffAlgorithm2(zsetopsrc *src, long setnum, zset *dstzset, size_t *maxelelen, size_t *totelelen) {
    /* DIFF Algorithm 2:
     *
     * Add all the elements of the first set to the auxiliary set.
     * Then remove all the elements of all the next sets from it.
     *

     * This is O(L + (N-K)log(N)) where L is the sum of all the elements in every
     * set, N is the size of the first set, and K is the size of the result set.
     *
     * Note that from the (L-N) dict searches, (N-K) got to the zsetRemoveFromSkiplist
     * which costs log(N)
     *
     * There is also a O(K) cost at the end for finding the largest element
     * size, but this doesn't change the algorithm complexity since K < L, and
     * O(2L) is the same as O(L). */
    int j;
    int cardinality = 0;
    zsetopval zval;
    zskiplistNode *znode;
    sds tmp;

    for (j = 0; j < setnum; j++) {
        if (zuiLength(&src[j]) == 0) continue;

        memset(&zval, 0, sizeof(zval));
        zuiInitIterator(&src[j]);
        while (zuiNext(&src[j],&zval)) {
            if (j == 0) {
                tmp = zuiNewSdsFromValue(&zval);
                znode = zslInsert(dstzset->zsl,zval.score,tmp);
                dictAdd(dstzset->dict, znode, NULL);
                cardinality++;
                sdsfree(tmp); /* zslInsert copied it, we can free our copy */
            } else {
                dictPauseAutoResize(dstzset->dict);
                tmp = zuiSdsFromValue(&zval);
                if (zsetRemoveFromSkiplist(dstzset, tmp)) {
                    cardinality--;
                }
                dictResumeAutoResize(dstzset->dict);
            }

            /* Exit if result set is empty as any additional removal
                * of elements will have no effect. */
            if (cardinality == 0) {
                zuiDiscardDirtyValue(&zval);
                break;
            }
        }
        zuiClearIterator(&src[j]);

        if (cardinality == 0) break;
    }

    /* Resize dict if needed after removing multiple elements */
    dictShrinkIfNeeded(dstzset->dict);

    /* Using this algorithm, we can't calculate the max element as we go,
     * we have to iterate through all elements to find the max one after. */
    *maxelelen = zsetDictGetMaxElementLength(dstzset->dict, totelelen);
}

static int zsetChooseDiffAlgorithm(zsetopsrc *src, long setnum) {
    int j;

    /* Select what DIFF algorithm to use.
     *
     * Algorithm 1 is O(N*M + K*log(K)) where N is the size of the
     * first set, M the total number of sets, and K is the size of the
     * result set.
     *
     * Algorithm 2 is O(L + (N-K)log(N)) where L is the total number of elements
     * in all the sets, N is the size of the first set, and K is the size of the
     * result set.
     *
     * We compute what is the best bet with the current input here. */
    long long algo_one_work = 0;
    long long algo_two_work = 0;

    for (j = 0; j < setnum; j++) {
        /* If any other set is equal to the first set, there is nothing to be
         * done, since we would remove all elements anyway. */
        if (j > 0 && src[0].subject == src[j].subject) {
            return 0;
        }

        algo_one_work += zuiLength(&src[0]);
        algo_two_work += zuiLength(&src[j]);
    }

    /* Algorithm 1 has better constant times and performs less operations
     * if there are elements in common. Give it some advantage. */
    algo_one_work /= 2;
    return (algo_one_work <= algo_two_work) ? 1 : 2;
}

static void zdiff(zsetopsrc *src, long setnum, zset *dstzset, size_t *maxelelen, size_t *totelelen) {
    /* Skip everything if the smallest input is empty. */
    if (zuiLength(&src[0]) > 0) {
        int diff_algo = zsetChooseDiffAlgorithm(src, setnum);
        if (diff_algo == 1) {
            zdiffAlgorithm1(src, setnum, dstzset, maxelelen, totelelen);
        } else if (diff_algo == 2) {
            zdiffAlgorithm2(src, setnum, dstzset, maxelelen, totelelen);
        } else if (diff_algo != 0) {
            serverPanic("Unknown algorithm");
        }
    }
}

#ifdef REDIS_TEST
#include <assert.h>
#include "testhelp.h"

/* Verify the entire skiplist structure for debugging purposes:
 * - Header node has correct structure
 * - Level is correct (highest non-NULL level)
 * - Forward and backward pointers are correct
 * - Scores are in sorted order (with lexicographic tie-breaking)
 * - Node levels stored in level[0].span are correct
 * - Span values across all levels sum to zsl->length
 * - Length matches actual node count
 * - Tail pointer is correct
 *
 * Panics with detailed error message if any invariant is violated. */
static void zslDebugVerifyStruct(zskiplist *zsl) {
    zskiplistNode *x;
    unsigned long length = 0;
    int i;

    /* Verify header node */
    serverAssert(zsl->header != NULL);
    serverAssert(zslGetNodeInfo(zsl->header)->sdsoffset == ZSL_OFFSET_NO_ELE);
    serverAssert(zsl->header->backward == NULL);

    /* Verify level is in valid range */
    serverAssert(zsl->level >= 1 && zsl->level <= ZSKIPLIST_MAXLEVEL);

    /* Verify that all levels >= zsl->level in header are NULL */
    for (i = zsl->level; i < ZSKIPLIST_MAXLEVEL; i++) {
        serverAssert(zsl->header->level[i].forward == NULL);
        serverAssert(zsl->header->level[i].span == 0);
    }

    /* Verify that level zsl->level-1 has at least one node (if list is not empty) */
    if (zsl->length > 0) {
        serverAssert(zsl->header->level[zsl->level-1].forward != NULL);
    }

    /* Single pass: verify forward/backward pointers, scores, node levels, and accumulate spans */
    x = zsl->header->level[0].forward;
    zskiplistNode *prev = NULL;

    while (x) {
        length++;

        /* Verify backward pointer */
        serverAssert(x->backward == prev);

        /* Verify node has valid element */
        serverAssert(zslGetNodeInfo(x)->sdsoffset != ZSL_OFFSET_NO_ELE);

        /* Verify node level is in valid range */
        unsigned long node_level = zslGetNodeInfo(x)->levels;
        serverAssert(node_level >= 1 && node_level <= ZSKIPLIST_MAXLEVEL);

        /* Verify score ordering */
        if (x->level[0].forward) {
            zskiplistNode *next = x->level[0].forward;
            serverAssert(next->score > x->score ||
                       (next->score == x->score && sdscmp(zslGetNodeElement(next), zslGetNodeElement(x)) > 0));
        }

        /* Verify spans are correct for all levels this node participates in.
         * Note: level 0 doesn't store span (it stores node level), so start from level 1.
         *
         * Span semantics:
         * - If forward != NULL: span represents distance to next node at this level (must be > 0)
         * - If forward == NULL: span represents number of nodes after this node at level 0
         *   (needed for zslGetRankByNode optimization) */
        for (i = 1; i < (int)node_level; i++) {
            if (x->level[i].forward) {
                /* Verify span is positive when there's a next node */
                serverAssert(x->level[i].span > 0);
            } else {
                /* When forward is NULL, span should equal the number of nodes after this node.
                 * We can verify this by counting remaining nodes at level 0. */
                unsigned long nodes_after = 0;
                zskiplistNode *temp = x->level[0].forward;
                while (temp) {
                    nodes_after++;
                    temp = temp->level[0].forward;
                }
                serverAssert(x->level[i].span == nodes_after);
            }
        }

        prev = x;
        x = x->level[0].forward;
    }

    /* Verify length matches actual count */
    serverAssert(length == zsl->length);

    /* Verify tail pointer */
    if (zsl->length == 0) {
        serverAssert(zsl->tail == NULL);
    } else {
        serverAssert(zsl->tail == prev);
        serverAssert(zsl->tail->level[0].forward == NULL);
    }

    /* Verify that the sum of spans at each level is consistent.
     * At each level, we traverse from header following forward pointers and sum all spans.
     * The sum should equal the rank of the last node at that level.
     * If the last node at a level is the tail, the sum should equal zsl->length. */
    for (i = 1; i < zsl->level; i++) {
        unsigned long span_sum = 0;
        zskiplistNode *last_at_level = zsl->header;
        x = zsl->header;
        while (x->level[i].forward) {
            span_sum += x->level[i].span;
            x = x->level[i].forward;
            last_at_level = x;
        }
        /* If the last node at this level is the tail, span sum should equal length */
        if (last_at_level == zsl->tail) {
            serverAssert(span_sum == zsl->length);
        } else {
            /* Otherwise, span sum should be less than length */
            serverAssert(span_sum < zsl->length);
        }
    }
}

int zsetTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    printf("Testing skiplist operations with structure verification\n");

    const int N = 1000;
    zskiplist *zsl = zslCreate();

    /* Store inserted elements for later deletion */
    typedef struct {
        double score;
        sds ele;
        zskiplistNode *node;
    } InsertedElement;

    InsertedElement *elements = zmalloc(sizeof(InsertedElement) * N);

    /* Seed random number generator for reproducible tests */
    srand(12345);

    printf("Inserting %d elements with scores 0-100 (with duplicates)...\n", N);

    /* Insert N elements with random scores between 0 and 100 */
    for (int i = 0; i < N; i++) {
        double score = (double)(rand() % 101); /* 0 to 100 */
        char buf[32];
        snprintf(buf, sizeof(buf), "elem%d", i);
        sds ele = sdsnew(buf);

        zskiplistNode *node = zslInsert(zsl, score, ele);

        /* Store for later deletion - keep a copy of the element name */
        elements[i].score = score;
        elements[i].ele = ele;
        elements[i].node = node;

        /* Verify structure after each insertion */
        zslDebugVerifyStruct(zsl);

        /* Query the inserted element */
        unsigned long rank = zslGetRank(zsl, score, ele);
        assert(rank != 0);

        /* Verify we can get the element by rank */
        zskiplistNode *found = zslGetElementByRank(zsl, rank);
        assert(found != NULL && found == node);

        /* Verify rank by node */
        unsigned long node_rank = zslGetRankByNode(zsl, node);
        assert(node_rank == rank);
    }

    test_cond("Insert N elements with verification",
        zsl->length == (unsigned long)N);

    printf("Deleting %d elements...\n", N);

    /* Delete all elements in reverse order */
    for (int i = N - 1; i >= 0; i--) {
        double score = elements[i].score;
        sds ele = elements[i].ele;

        /* Verify element exists before deletion with valid rank */
        unsigned long rank = zslGetRank(zsl, score, ele);
        assert(rank >= 1 && rank <= (unsigned long)(i + 1));

        /* Delete the element - zslDelete frees the node's SDS string */
        zslDelete(zsl, elements[i].node);

        /* Verify structure after each deletion */
        zslDebugVerifyStruct(zsl);
        sdsfree(elements[i].ele);
    }

    test_cond("Delete N elements with verification",
        zsl->length == 0 && zsl->tail == NULL);

    zfree(elements);
    zslFree(zsl);
    
    return 0;
}
#endif
