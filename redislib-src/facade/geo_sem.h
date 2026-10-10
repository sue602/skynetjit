/* geo_sem.h -- skynetjit redislib: the geoset semantics layer, i.e. everything
 * Redis 8.10.0 src/geo.c has outside its command functions, rewritten so that
 * argument checking raises Lua errors with Redis' own texts instead of replying
 * to a client. facade/geo.c (the ten GEO* commands) is the only consumer.
 *
 * The geohash machinery itself is NOT here: kernel/geohash.c and
 * kernel/geohash_helper.c are the verbatim upstream files, so a coordinate that
 * encodes, decodes or distance-checks differently from a real Redis would be a
 * kernel bug, not a facade one. What is here is the zset-walking search
 * (geoGetPointsInRange / membersOfAllNeighbors), the argument grammar and the
 * result collector.
 */

#ifndef REDISLIB_GEO_SEM_H
#define REDISLIB_GEO_SEM_H

#include <lua.h>
#include <lauxlib.h>

#include "rl_common.h"
#include "geo.h"              /* geoPoint / geoArray */
#include "geohash_helper.h"   /* GeoShape, GeoHashRadius, GEO_STEP_MAX, ... */

/* Argument errors are raised, so these return C_OK / never return, except
 * rl_geoLongLatFromMember() and the search functions, which report C_ERR
 * because Redis turns those into a reply rather than an error. */

/* geo.c:53-87 -- the result collector, verbatim apart from the names. */
geoArray *rl_geoArrayCreate(void);
geoPoint *rl_geoArrayAppend(geoArray *ga, double *xy, double dist, double score,
                            char *member);
void rl_geoArrayFree(geoArray *ga);

/* geo.c:92 */
int rl_geoDecodeGeohash(double bits, double *xy);

/* Two consecutive arguments hold <longitude,latitude>; geo.c:100
 * extractLongLatOrReply(). Raises on a non-numeric argument or a pair outside
 * the valid range. */
int rl_geoLongLatArg(lua_State *L, int idx, double *xy);

/* geo.c:120 -- the score of a member IS the encoded position. */
int rl_geoLongLatFromMember(robj *zobj, sds member, double *xy);

/* geo.c:134 -- the unit word and its meters factor; raises the Redis error. */
double rl_geoUnitArg(lua_State *L, int idx);

/* geo.c:156 / :183 -- "<number> <unit>" and "<width> <height> <unit>". */
int rl_geoDistanceArg(lua_State *L, int idx, double *conversion, double *radius);
int rl_geoBoxArg(lua_State *L, int idx, double *conversion, double *width,
                 double *height);

/* geo.c:212 addReplyDoubleDistance() renders the distance with four decimals,
 * and that text is what a client then reads back as a number. A redislib
 * command replies a number directly (spec §3.2), so the value is the same one a
 * client would get from Redis, not the full-precision double. */
double rl_geoReplyDistance(double d);

/* geo.c:232 / :261 / :329 / :358 / :366 -- the search itself. */
int rl_geoWithinShape(GeoShape *shape, double score, double *xy, double *distance);
long rl_geoGetPointsInRange(robj *zobj, double min, double max, GeoShape *shape,
                            geoArray *ga, unsigned long limit);
long rl_geoMembersOfAllNeighbors(robj *zobj, const GeoHashRadius *n,
                                 GeoShape *shape, geoArray *ga,
                                 unsigned long limit);

/* geo.c:425 / :437 -- the ASC / DESC comparators on the collected points. */
int rl_geoSortAsc(const void *a, const void *b);
int rl_geoSortDesc(const void *a, const void *b);

/* geo.c:741-755 -- sort the collector, with pqsort() when only the first
 * `returned_items` of `result_length` are wanted. */
void rl_geoSortResult(geoArray *ga, long result_length, long returned_items,
                      int descending);

#endif /* REDISLIB_GEO_SEM_H */
