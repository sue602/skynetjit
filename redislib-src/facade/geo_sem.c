/* geo_sem.c -- skynetjit redislib: the geoset semantics layer (see geo_sem.h).
 *
 * Transcribed from Redis 8.10.0 src/geo.c's non-command part. The search keeps
 * upstream's shape exactly: a GeoShape is turned into the geohash boxes that
 * cover it, each box becomes a score range of the backing zset
 * (min <= score < max), the range is walked with the exported zset iterators,
 * and every candidate is then checked against the real shape. The collector is
 * upstream's geoArray; only the reply and the argument checking differ, because
 * there is no client here.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <math.h>

#include "geo_sem.h"
#include "pqsort.h"

/* Kernel entry points used here that facade-side t_zset.h does not cover. */
unsigned char *zzlFirstInRange(unsigned char *zl, zrangespec *range);
int zslValueLteMax(double value, zrangespec *spec);

/* ---- geoArray (geo.c:53-87) --------------------------------------------- */

geoArray *
rl_geoArrayCreate(void) {
    geoArray *ga = zmalloc(sizeof(*ga));

    /* It gets allocated on first rl_geoArrayAppend() call. */
    ga->array = NULL;
    ga->buckets = 0;
    ga->used = 0;
    return ga;
}

geoPoint *
rl_geoArrayAppend(geoArray *ga, double *xy, double dist, double score,
                  char *member) {
    geoPoint *gp;

    if (ga->used == ga->buckets) {
        ga->buckets = (ga->buckets == 0) ? 8 : ga->buckets*2;
        ga->array = zrealloc(ga->array, sizeof(geoPoint)*ga->buckets);
    }
    gp = ga->array + ga->used;
    gp->longitude = xy[0];
    gp->latitude = xy[1];
    gp->dist = dist;
    gp->member = member;
    gp->score = score;
    ga->used++;
    return gp;
}

void
rl_geoArrayFree(geoArray *ga) {
    size_t i;

    for (i = 0; i < ga->used; i++) sdsfree(ga->array[i].member);
    zfree(ga->array);
    zfree(ga);
}

/* ---- decoding and argument checking ------------------------------------- */

int
rl_geoDecodeGeohash(double bits, double *xy) {
    GeoHashBits hash = { .bits = (uint64_t)bits, .step = GEO_STEP_MAX };

    return geohashDecodeToLongLatWGS84(hash, xy);
}

/* extractLongLatOrReply() -- geo.c:100, the two arguments at idx and idx+1. */
int
rl_geoLongLatArg(lua_State *L, int idx, double *xy) {
    int i;

    for (i = 0; i < 2; i++) {
        if (rl_todouble(L, idx+i, xy + i) != C_OK)
            rl_error(L, "value is not a valid float");
    }
    if (xy[0] < GEO_LONG_MIN || xy[0] > GEO_LONG_MAX ||
        xy[1] < GEO_LAT_MIN  || xy[1] > GEO_LAT_MAX) {
        /* addReplyErrorFormat() of geo.c:110, same string and same %f. */
        rl_error(L, "invalid longitude,latitude pair %f,%f", xy[0], xy[1]);
    }
    return C_OK;
}

int
rl_geoLongLatFromMember(robj *zobj, sds member, double *xy) {
    double score = 0;

    if (zsetScore(zobj, member, &score) == C_ERR) return C_ERR;
    if (!rl_geoDecodeGeohash(score, xy)) return C_ERR;
    return C_OK;
}

/* extractUnitOrReply() -- geo.c:134. */
double
rl_geoUnitArg(lua_State *L, int idx) {
    const char *u;

    /* A caller that handed something that is not a word (a table, a nil) gets
     * the same answer Redis gives for a word it does not know: there is no
     * Lua-side "string expected" in a client's vocabulary, and numbers coerce,
     * so 5 lands here as "5" and is refused exactly as the server refuses it. */
    if (!lua_isstring(L, idx))
        return rl_error(L,
            "unsupported unit provided. please use M, KM, FT, MI"), -1;
    u = lua_tostring(L, idx);

    if (!strcasecmp(u, "m"))  return 1;
    if (!strcasecmp(u, "km")) return 1000;
    if (!strcasecmp(u, "ft")) return 0.3048;
    if (!strcasecmp(u, "mi")) return 1609.34;
    return rl_error(L,
        "unsupported unit provided. please use M, KM, FT, MI"), -1;
}

/* extractDistanceOrReply() -- geo.c:156: "<number> <unit>". */
int
rl_geoDistanceArg(lua_State *L, int idx, double *conversion, double *radius) {
    double distance;

    if (rl_todouble(L, idx, &distance) != C_OK)
        return rl_error(L, "need numeric radius"), C_ERR;
    if (distance < 0) {
        return rl_error(L, "radius cannot be negative"), C_ERR;
    }
    if (radius) *radius = distance;

    {
        double to_meters = rl_geoUnitArg(L, idx+1);

        if (to_meters < 0) return C_ERR;
        if (conversion) *conversion = to_meters;
    }
    return C_OK;
}

/* extractBoxOrReply() -- geo.c:183: "<width> <height> <unit>". */
int
rl_geoBoxArg(lua_State *L, int idx, double *conversion, double *width,
             double *height) {
    double h, w, to_meters;

    if (rl_todouble(L, idx, &w) != C_OK)
        return rl_error(L, "need numeric width"), C_ERR;
    if (rl_todouble(L, idx+1, &h) != C_OK)
        return rl_error(L, "need numeric height"), C_ERR;
    if (h < 0 || w < 0) {
        return rl_error(L, "height or width cannot be negative"), C_ERR;
    }
    if (height) *height = h;
    if (width) *width = w;

    to_meters = rl_geoUnitArg(L, idx+2);
    if (to_meters < 0) return C_ERR;
    if (conversion) *conversion = to_meters;
    return C_OK;
}

/* addReplyDoubleDistance() -- geo.c:212. Redis renders four decimals into a
 * bulk string; the client reads that text back as a number, so the facade
 * returns the same rounded value rather than the raw double. */
double
rl_geoReplyDistance(double d) {
    char dbuf[128];
    const int dlen = fixedpoint_d2string(dbuf, sizeof(dbuf), d, 4);
    double v;

    if (dlen <= 0 || string2d(dbuf, (size_t)dlen, &v) == 0) return d;
    return v;
}

/* ---- the search (geo.c:232-422) ----------------------------------------- */

int
rl_geoWithinShape(GeoShape *shape, double score, double *xy, double *distance) {
    if (!rl_geoDecodeGeohash(score, xy)) return C_ERR; /* Can't decode. */

    /* Note that geohashGetDistanceIfInRadiusWGS84() takes arguments in
     * reverse order: longitude first, latitude later. */
    if (shape->type == CIRCULAR_TYPE) {
        if (!geohashGetDistanceIfInRadiusWGS84(shape->xy[0], shape->xy[1],
                                               xy[0], xy[1],
                                               shape->t.radius*shape->conversion,
                                               distance))
            return C_ERR;
    } else if (shape->type == RECTANGLE_TYPE) {
        if (!geohashGetDistanceIfInRectangle(shape->t.r.width * shape->conversion,
                                             shape->t.r.height * shape->conversion,
                                             shape->xy[0], shape->xy[1],
                                             xy[0], xy[1], distance))
            return C_ERR;
    }
    return C_OK;
}

long
rl_geoGetPointsInRange(robj *zobj, double min, double max, GeoShape *shape,
                       geoArray *ga, unsigned long limit) {
    /* minex 0 = include min in range; maxex 1 = exclude max in range */
    /* That's: min <= val < max */
    zrangespec range = { .min = min, .max = max, .minex = 0, .maxex = 1 };
    size_t origincount = ga->used;

    if (zobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *zl = zobj->ptr;
        unsigned char *eptr, *sptr;
        unsigned char *vstr = NULL;
        unsigned int vlen = 0;
        long long vlong = 0;
        double score = 0;

        if ((eptr = zzlFirstInRange(zl, &range)) == NULL) {
            /* Nothing exists starting at our min.  No results. */
            return 0;
        }

        sptr = lpNext(zl, eptr);
        while (eptr) {
            double xy[2];
            double distance = 0;
            score = zzlGetScore(sptr);

            /* If we fell out of range, break. */
            if (!zslValueLteMax(score, &range))
                break;

            vstr = lpGetValue(eptr, &vlen, &vlong);
            if (rl_geoWithinShape(shape, score, xy, &distance) == C_OK) {
                /* Append the new element. */
                char *member = (vstr == NULL) ? sdsfromlonglong(vlong)
                                               : sdsnewlen(vstr, vlen);
                rl_geoArrayAppend(ga, xy, distance, score, member);
            }
            if (ga->used && limit && ga->used >= limit) break;
            zzlNext(zl, &eptr, &sptr);
        }
    } else if (zobj->encoding == OBJ_ENCODING_SKIPLIST) {
        zset *zs = zobj->ptr;
        zskiplist *zsl = zs->zsl;
        zskiplistNode *ln;

        if ((ln = zslNthInRange(zsl, &range, 0, NULL)) == NULL) {
            /* Nothing exists starting at our min.  No results. */
            return 0;
        }

        while (ln) {
            double xy[2];
            double distance = 0;

            /* Abort when the node is no longer in range. */
            if (!zslValueLteMax(ln->score, &range))
                break;
            if (rl_geoWithinShape(shape, ln->score, xy, &distance) == C_OK) {
                /* Append the new element. */
                sds ele = zslGetNodeElement(ln);
                rl_geoArrayAppend(ga, xy, distance, ln->score, sdsdup(ele));
            }
            if (ga->used && limit && ga->used >= limit) break;
            ln = ln->level[0].forward;
        }
    }
    return (long)(ga->used - origincount);
}

/* scoresOfGeoHashBox() -- geo.c:329, verbatim apart from the name. */
static void
rl_geoScoresOfGeoHashBox(GeoHashBits hash, GeoHashFix52Bits *min,
                         GeoHashFix52Bits *max) {
    *min = geohashAlign52Bits(hash);
    hash.bits++;
    *max = geohashAlign52Bits(hash);
}

/* membersOfGeoHashBox() -- geo.c:358. */
static long
rl_geoMembersOfGeoHashBox(robj *zobj, GeoHashBits hash, geoArray *ga,
                          GeoShape *shape, unsigned long limit) {
    GeoHashFix52Bits min, max;

    rl_geoScoresOfGeoHashBox(hash, &min, &max);
    return rl_geoGetPointsInRange(zobj, min, max, shape, ga, limit);
}

/* membersOfAllNeighbors() -- geo.c:366. The debug tracing of the upstream
 * function is left out: `debugmsg` is a local that is never set to 1, so it
 * only ever compiled out there as well. */
long
rl_geoMembersOfAllNeighbors(robj *zobj, const GeoHashRadius *n,
                            GeoShape *shape, geoArray *ga,
                            unsigned long limit) {
    GeoHashBits neighbors[9];
    unsigned int i, count = 0, last_processed = 0;

    neighbors[0] = n->hash;
    neighbors[1] = n->neighbors.north;
    neighbors[2] = n->neighbors.south;
    neighbors[3] = n->neighbors.east;
    neighbors[4] = n->neighbors.west;
    neighbors[5] = n->neighbors.north_east;
    neighbors[6] = n->neighbors.north_west;
    neighbors[7] = n->neighbors.south_east;
    neighbors[8] = n->neighbors.south_west;

    /* For each neighbor (*and* our own hashbox), get all the matching
     * members and add them to the potential result list. */
    for (i = 0; i < sizeof(neighbors) / sizeof(*neighbors); i++) {
        if (HASHISZERO(neighbors[i])) continue;

        /* When a huge Radius (in the 5000 km range or more) is used,
         * adjacent neighbors can be the same, leading to duplicated
         * elements. Skip every range which is the same as the one
         * processed previously. */
        if (last_processed &&
            neighbors[i].bits == neighbors[last_processed].bits &&
            neighbors[i].step == neighbors[last_processed].step)
        {
            continue;
        }
        if (ga->used && limit && ga->used >= limit) break;
        count += rl_geoMembersOfGeoHashBox(zobj, neighbors[i], ga, shape, limit);
        last_processed = i;
    }
    return count;
}

/* ---- sorting (geo.c:425-439) -------------------------------------------- */

int
rl_geoSortAsc(const void *a, const void *b) {
    const geoPoint *gpa = a, *gpb = b;

    /* We can't do adist - bdist because they are doubles and
     * the comparator returns an int. */
    if (gpa->dist > gpb->dist) return 1;
    if (gpa->dist == gpb->dist) return 0;
    return -1;
}

int
rl_geoSortDesc(const void *a, const void *b) {
    return -rl_geoSortAsc(a, b);
}

/* Sort the collected points, taking the first `returned_items` of
 * `result_length` -- pqsort() for a partial selection, like geo.c:752. */
void
rl_geoSortResult(geoArray *ga, long result_length, long returned_items,
                 int descending) {
    int (*cmp)(const void *, const void *) = descending ? rl_geoSortDesc
                                                        : rl_geoSortAsc;

    if (returned_items == result_length) {
        qsort(ga->array, (size_t)result_length, sizeof(geoPoint), cmp);
    } else {
        pqsort(ga->array, (size_t)result_length, sizeof(geoPoint), cmp,
               0, (int)(returned_items - 1));
    }
}
