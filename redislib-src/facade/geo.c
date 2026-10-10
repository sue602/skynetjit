/* geo.c -- the redis.geo facade: the ten GEO* commands of Redis 8.10.0
 * (GEOADD GEODIST GEOHASH GEOPOS GEORADIUS GEORADIUS_RO GEORADIUSBYMEMBER
 * GEORADIUSBYMEMBER_RO GEOSEARCH GEOSEARCHSTORE) over an in-memory geoset.
 *
 * A geoset IS a sorted set in Redis, and here as there: the member is the name
 * as the caller wrote it and the score is the 52-bit geohash of the position
 * (geohashEncodeWGS84 + geohashAlign52Bits). Everything numeric therefore comes
 * from the same two kernel files the server uses -- kernel/geohash.c and
 * kernel/geohash_helper.c, both verbatim -- and the search itself is
 * facade/geo_sem.c, also transcribed from geo.c. This file is the command
 * layer: the client/keyspace parts are removed, addReply* becomes the rl_push*
 * mapping, and STORE / STOREDIST take a handle instead of a key name.
 *
 * Not provided, because this source tree does not have it: the GEOSHAPE /
 * BYPOLYGON form of GEOSEARCH (see the design spec §3.3).
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <math.h>

#include "geo_sem.h"
#include "geo.h"

/* Kernel entry points not covered by facade-side t_zset.h (see NOTICE §4). */
robj *zsetTypeCreate(size_t size_hint, size_t val_len_hint);
void zsetTypeMaybeConvert(robj *zobj, size_t size_hint);

/* geo.c:506-514 */
#define RL_SORT_NONE 0
#define RL_SORT_ASC  1
#define RL_SORT_DESC 2

#define RL_RADIUS_COORDS (1<<0)   /* Search around coordinates. */
#define RL_RADIUS_MEMBER (1<<1)   /* Search around member. */
#define RL_RADIUS_NOSTORE (1<<2)  /* Do not accept STORE/STOREDIST option. */
#define RL_GEOSEARCH     (1<<3)   /* GEOSEARCH command variant */
#define RL_GEOSEARCHSTORE (1<<4)  /* GEOSEARCHSTORE: only STOREDIST */

/* ---- object lifecycle --------------------------------------------------- */

/* geo.new() => an empty geoset, i.e. an empty zset. */
static int
lgeo_new(lua_State *L) {
    rl_object *o;

    rl_newobject(L, RL_GEO);
    o = (rl_object*)lua_touserdata(L, -1);
    o->zobj = zsetTypeCreate(0, 0);
    return 1;
}

static int
lgeo_free(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_GEO);
    rl_free_ud(L, o);
    return 0;
}

/* The geoset of the object at idx. */
static robj *
rl_geo(lua_State *L, int idx) {
    return rl_check(L, idx, RL_GEO)->zobj;
}

/* A STORE / STOREDIST target: a geoset or a plain sorted set, since that is the
 * type Redis writes here. */
static rl_object *
rl_geo_store_target(lua_State *L, int idx) {
    rl_object *o = (rl_object*)lua_touserdata(L, idx);

    if (!o || (o->kind != RL_GEO && o->kind != RL_ZSET))
        rl_error(L, "-WRONGTYPE Operation against a key holding the wrong kind of value");
    return o;
}

/* The score a member gets: the 52-bit geohash, as GEOADD computes it. */
static double
rl_geoencode(double lng, double lat) {
    GeoHashBits hash;

    geohashEncodeWGS84(lng, lat, GEO_STEP_MAX, &hash);
    return (double)geohashAlign52Bits(hash);
}

/* ---- GEOADD ------------------------------------------------------------- */

/* GEOADD key [CH] [NX|XX] long lat name [long2 lat2 name2 ...]  (geo.c:446)
 *
 * Upstream encodes every pair and then hands the vector to zaddCommand(); the
 * loop below is the same ZADD work, so the reply and the option semantics are
 * ZADD's (number added, or added+updated with CH). */
static int
lgeoadd(lua_State *L) {
    robj *zobj = rl_geo(L, 1);
    int longidx = 2, i, elements, added = 0, updated = 0;
    int nx = 0, xx = 0, ch = 0;

    while (lua_type(L, longidx) == LUA_TSTRING) {
        const char *opt = lua_tostring(L, longidx);

        if (!strcasecmp(opt, "nx")) nx = 1;
        else if (!strcasecmp(opt, "xx")) xx = 1;
        /* Upstream leaves CH to zaddCommand() because GEOADD rebuilds a ZADD
         * vector; the facade performs that ZADD inline, so it tracks CH here. */
        else if (!strcasecmp(opt, "ch")) ch = 1;
        else break;
        longidx++;
    }

    if ((lua_gettop(L) - longidx + 1) % 3 || (xx && nx))
        return rl_error(L, "-ERR syntax error");

    elements = (lua_gettop(L) - longidx + 1) / 3;

    /* Upstream builds the whole ZADD vector before the zset is touched, so a bad
     * coordinate aborts with nothing added. Validating in a pass of its own
     * keeps that property without a heap vector that would leak when the second
     * coordinate raises: encoding is pure arithmetic, so pass two simply
     * re-derives it. */
    for (i = 0; i < elements; i++) {
        double xy[2];

        rl_geoLongLatArg(L, longidx + i*3, xy);
        rl_checkmember(L, longidx + i*3 + 2);
    }

    zsetTypeMaybeConvert(zobj, (size_t)elements);

    for (i = 0; i < elements; i++) {
        double xy[2], score, newscore;
        int retflags = 0;
        sds ele = rl_member(L, longidx + i*3 + 2);
        int retval;

        rl_geoLongLatArg(L, longidx + i*3, xy);
        score = rl_geoencode(xy[0], xy[1]);
        retval = zsetAdd(zobj, score, ele,
                         (nx ? ZADD_IN_NX : 0) | (xx ? ZADD_IN_XX : 0),
                         &retflags, &newscore);

        sdsfree(ele);
        if (retval == 0) {
            /* rl: unreachable -- a geohash score is never NaN -- but ZADD's own
             * reply path is kept, so a future caller cannot lose the error. */
            return rl_error(L, "resulting score is not a number (NaN)");
        }
        if (retflags & ZADD_OUT_ADDED) added++;
        if (retflags & ZADD_OUT_UPDATED) updated++;
    }

    rl_pushint(L, ch ? added+updated : added);
    return 1;
}

/* ---- GEODIST / GEOHASH / GEOPOS ----------------------------------------- */

/* GEODIST key ele1 ele2 [unit]                                    (geo.c:974) */
static int
lgeodist(lua_State *L) {
    robj *zobj = rl_geo(L, 1);
    double to_meter = 1, score1, score2, xyxy[4];
    sds m1, m2;
    int top = lua_gettop(L);

    if (top == 4) {
        to_meter = rl_geoUnitArg(L, 4);
        if (to_meter < 0) return 0;             /* rl_geoUnitArg() raised */
    } else if (top > 4) {
        return rl_error(L, "-ERR syntax error");
    }

    m1 = rl_member(L, 2);
    m2 = rl_member(L, 3);
    if (zsetScore(zobj, m1, &score1) == C_ERR ||
        zsetScore(zobj, m2, &score2) == C_ERR)
    {
        sdsfree(m1); sdsfree(m2);
        rl_pushnilreply(L);
        return 1;
    }
    sdsfree(m1); sdsfree(m2);

    if (!rl_geoDecodeGeohash(score1, xyxy) || !rl_geoDecodeGeohash(score2, xyxy+2))
        rl_pushnilreply(L);
    else
        rl_pushdouble(L, rl_geoReplyDistance(
            geohashGetDistance(xyxy[0], xyxy[1], xyxy[2], xyxy[3]) / to_meter));
    return 1;
}

/* GEOHASH key ele1 ... eleN => array of 11-char strings or nil     (geo.c:879) */
static int
lgeohash(lua_State *L) {
    static const char *geoalphabet = "0123456789bcdefghjkmnpqrstuvwxyz";
    robj *zobj = rl_geo(L, 1);
    int top = lua_gettop(L), j;

    lua_createtable(L, top-1, 0);
    for (j = 2; j <= top; j++) {
        double score;
        sds ele = rl_member(L, j);
        int rc = zsetScore(zobj, ele, &score);

        sdsfree(ele);
        if (rc == C_ERR) {
            rl_pushnilreply(L);
        } else {
            /* The internal format we use for geocoding is a bit different
             * than the standard, since we use as initial latitude range
             * -85,85, while the normal geohashing algorithm uses -90,90. So
             * decode, then re-encode with the standard ranges (geo.c:895). */
            double xy[2];
            GeoHashRange r[2];
            GeoHashBits hash;
            char buf[12];
            int i;

            if (!rl_geoDecodeGeohash(score, xy)) {
                rl_pushnilreply(L);
            } else {
                r[0].min = -180; r[0].max = 180;
                r[1].min = -90;  r[1].max = 90;
                geohashEncode(&r[0], &r[1], xy[0], xy[1], 26, &hash);
                for (i = 0; i < 11; i++) {
                    int idx;
                    if (i == 10) idx = 0;   /* only 52 bits, pad for the API */
                    else idx = (int)((hash.bits >> (52-((i+1)*5))) & 0x1f);
                    buf[i] = geoalphabet[idx];
                }
                buf[11] = '\0';
                lua_pushlstring(L, buf, 11);
            }
        }
        lua_rawseti(L, -2, j-1);
    }
    return 1;
}

/* GEOPOS key ele1 ... eleN => array of {longitude,latitude} or nil  (geo.c:941) */
static int
lgeopos(lua_State *L) {
    robj *zobj = rl_geo(L, 1);
    int top = lua_gettop(L), j;

    lua_createtable(L, top-1, 0);
    for (j = 2; j <= top; j++) {
        double score, xy[2];
        sds ele = rl_member(L, j);
        int rc = zsetScore(zobj, ele, &score);

        sdsfree(ele);
        /* rl: addReplyNullArray() is one value here, `false`, like RESP2's null
         * bulk for a missing member. */
        if (rc == C_ERR || !rl_geoDecodeGeohash(score, xy)) {
            rl_pushnilreply(L);
        } else {
            lua_createtable(L, 2, 0);
            rl_pushdouble(L, xy[0]);
            lua_rawseti(L, -2, 1);
            rl_pushdouble(L, xy[1]);
            lua_rawseti(L, -2, 2);
        }
        lua_rawseti(L, -2, j-1);
    }
    return 1;
}

/* ---- the search family -------------------------------------------------- */

/* georadiusGeneric() -- geo.c:524.
 *
 * The option grammar is upstream's, one word per Lua argument, with two
 * differences that come from having no keyspace: STORE / STOREDIST name a
 * handle instead of a key, and GEOSEARCHSTORE's destination is argument 1
 * (so `dst:geosearchstore(src, ...)` and `geo.geosearchstore(dst, src, ...)`
 * are the same call). */
static int
rl_geosearch_generic(lua_State *L, int srcidx, int flags) {
    robj *zobj = rl_check(L, srcidx, RL_GEO)->zobj;
    rl_object *storekey = NULL;
    int storedist = 0;
    int base_args;
    GeoShape shape;
    int withdist = 0, withhash = 0, withcoords = 0;
    int frommember = 0, fromloc = 0, byradius = 0, bybox = 0;
    int sort = RL_SORT_NONE, any = 0;
    long long count = 0;
    int top = lua_gettop(L), i;
    GeoHashRadius georadius;
    geoArray *ga;
    long result_length, returned_items;
    long option_length = 0;

    memset(&shape, 0, sizeof(shape));

    if (flags & RL_RADIUS_COORDS) {
        base_args = 6;
        shape.type = CIRCULAR_TYPE;
        rl_geoLongLatArg(L, 2, shape.xy);
        rl_geoDistanceArg(L, base_args-2, &shape.conversion, &shape.t.radius);
    } else if (flags & RL_RADIUS_MEMBER) {
        sds member = rl_member(L, 2);
        int rc;

        base_args = 5;
        shape.type = CIRCULAR_TYPE;
        rc = rl_geoLongLatFromMember(zobj, member, shape.xy);
        sdsfree(member);
        if (rc == C_ERR)
            return rl_error(L, "could not decode requested zset member");
        rl_geoDistanceArg(L, base_args-2, &shape.conversion, &shape.t.radius);
    } else if (flags & RL_GEOSEARCH) {
        base_args = 2;
        if (flags & RL_GEOSEARCHSTORE) {
            base_args = 3;
            storekey = rl_geo_store_target(L, 1);
        }
    } else {
        return rl_error(L, "Unknown georadius search type");
    }

    /* Discover and populate all optional parameters. */
    {
        int remaining = top - base_args + 1;

        for (i = 0; i < remaining; i++) {
            const char *arg;

            if (lua_type(L, base_args + i) != LUA_TSTRING)
                return rl_error(L, "-ERR syntax error");
            arg = lua_tostring(L, base_args + i);

            if (!strcasecmp(arg, "withdist")) {
                withdist = 1;
            } else if (!strcasecmp(arg, "withhash")) {
                withhash = 1;
            } else if (!strcasecmp(arg, "withcoord")) {
                withcoords = 1;
            } else if (!strcasecmp(arg, "any")) {
                any = 1;
            } else if (!strcasecmp(arg, "asc")) {
                sort = RL_SORT_ASC;
            } else if (!strcasecmp(arg, "desc")) {
                sort = RL_SORT_DESC;
            } else if (!strcasecmp(arg, "count") && (i+1) < remaining) {
                if (rl_tolong(L, base_args+i+1, &count) != C_OK)
                    return rl_error(L, "value is not an integer or out of range");
                if (count <= 0)
                    return rl_error(L, "COUNT must be > 0");
                i++;
            } else if (!strcasecmp(arg, "store") &&
                       (i+1) < remaining &&
                       !(flags & RL_RADIUS_NOSTORE) &&
                       !(flags & RL_GEOSEARCH))
            {
                storekey = rl_geo_store_target(L, base_args+i+1);
                storedist = 0;
                i++;
            } else if (!strcasecmp(arg, "storedist") &&
                       (i+1) < remaining &&
                       !(flags & RL_RADIUS_NOSTORE) &&
                       !(flags & RL_GEOSEARCH))
            {
                storekey = rl_geo_store_target(L, base_args+i+1);
                storedist = 1;
                i++;
            } else if (!strcasecmp(arg, "storedist") &&
                       (flags & RL_GEOSEARCH) && (flags & RL_GEOSEARCHSTORE)) {
                storedist = 1;
            } else if (!strcasecmp(arg, "frommember") &&
                       (i+1) < remaining && flags & RL_GEOSEARCH && !fromloc)
            {
                sds member = rl_member(L, base_args+i+1);
                int rc = rl_geoLongLatFromMember(zobj, member, shape.xy);

                sdsfree(member);
                if (rc == C_ERR)
                    return rl_error(L, "could not decode requested zset member");
                frommember = 1;
                i++;
            } else if (!strcasecmp(arg, "fromlonlat") &&
                       (i+2) < remaining && flags & RL_GEOSEARCH && !frommember)
            {
                rl_geoLongLatArg(L, base_args+i+1, shape.xy);
                fromloc = 1;
                i += 2;
            } else if (!strcasecmp(arg, "byradius") &&
                       (i+2) < remaining && flags & RL_GEOSEARCH && !bybox)
            {
                rl_geoDistanceArg(L, base_args+i+1, &shape.conversion,
                                  &shape.t.radius);
                shape.type = CIRCULAR_TYPE;
                byradius = 1;
                i += 2;
            } else if (!strcasecmp(arg, "bybox") &&
                       (i+3) < remaining && flags & RL_GEOSEARCH && !byradius)
            {
                rl_geoBoxArg(L, base_args+i+1, &shape.conversion,
                             &shape.t.r.width, &shape.t.r.height);
                shape.type = RECTANGLE_TYPE;
                bybox = 1;
                i += 3;
            } else {
                return rl_error(L, "-ERR syntax error");
            }
        }
    }

    /* Trap options not compatible with STORE and STOREDIST. */
    if (storekey && (withdist || withhash || withcoords))
        return rl_error(L,
            "%s is not compatible with WITHDIST, WITHHASH and WITHCOORD options",
            (flags & RL_GEOSEARCHSTORE) ? "GEOSEARCHSTORE"
                                        : "STORE option in GEORADIUS");

    if ((flags & RL_GEOSEARCH) && !(frommember || fromloc))
        return rl_error(L,
            "exactly one of FROMMEMBER or FROMLONLAT can be specified for %s",
            (flags & RL_GEOSEARCHSTORE) ? "geosearchstore" : "geosearch");

    if ((flags & RL_GEOSEARCH) && !(byradius || bybox))
        return rl_error(L,
            "exactly one of BYRADIUS and BYBOX can be specified for %s",
            (flags & RL_GEOSEARCHSTORE) ? "geosearchstore" : "geosearch");

    if (any && !count)
        return rl_error(L, "the ANY argument requires COUNT argument");

    /* COUNT without ordering does not make much sense (we need to sort in
     * order to return the closest N entries), force ASC ordering if COUNT was
     * specified but no sorting was requested. Not needed for ANY. */
    if (count != 0 && sort == RL_SORT_NONE && !any) sort = RL_SORT_ASC;

    /* Get all neighbor geohash boxes for our radius search. */
    georadius = geohashCalculateAreasByShapeWGS84(&shape);

    /* Search the zset for all matching points. */
    ga = rl_geoArrayCreate();
    rl_geoMembersOfAllNeighbors(zobj, &georadius, &shape, ga,
                                (unsigned long)(any ? count : 0));

    /* If no matching results, the user gets an empty reply. */
    if (ga->used == 0 && storekey == NULL) {
        rl_geoArrayFree(ga);
        lua_createtable(L, 0, 0);
        return 1;
    }

    result_length = (long)ga->used;
    returned_items = (count == 0 || result_length < count) ?
                     result_length : (long)count;

    if (sort != RL_SORT_NONE)
        rl_geoSortResult(ga, result_length, returned_items,
                         sort == RL_SORT_DESC);

    if (storekey == NULL) {
        /* Our options are self-contained nested replies, so we only need to
         * track how many of those there are. */
        if (withdist) option_length++;
        if (withcoords) option_length++;
        if (withhash) option_length++;

        lua_createtable(L, (int)returned_items, 0);
        for (i = 0; i < returned_items; i++) {
            geoPoint *gp = ga->array + i;
            int slot = i + 1;

            gp->dist /= shape.conversion; /* Fix according to unit. */

            if (option_length)
                lua_createtable(L, (int)option_length + 1, 0);

            lua_pushlstring(L, gp->member, sdslen(gp->member));
            if (option_length) lua_rawseti(L, -2, 1);

            if (withdist) {
                rl_pushdouble(L, rl_geoReplyDistance(gp->dist));
                lua_rawseti(L, -2, 2);
            }
            if (withhash) {
                rl_pushint(L, (long long)gp->score);
                lua_rawseti(L, -2, 2 + withdist);
            }
            if (withcoords) {
                int base = 2 + withdist + withhash;

                lua_createtable(L, 2, 0);
                rl_pushdouble(L, gp->longitude);
                lua_rawseti(L, -2, 1);
                rl_pushdouble(L, gp->latitude);
                lua_rawseti(L, -2, 2);
                lua_rawseti(L, -2, base);
            }

            lua_rawseti(L, -2, slot);
        }
    } else {
        /* Target: create a sorted set with the results, exactly like the
         * store branch of geo.c:804-842. */
        size_t maxelelen = 0, totelelen = 0;
        robj *result = NULL;

        if (returned_items) {
            result = createZsetObject();
        }

        for (i = 0; i < returned_items; i++) {
            geoPoint *gp = ga->array + i;
            size_t elelen;
            double score;

            gp->dist /= shape.conversion;
            score = storedist ? gp->dist : gp->score;
            elelen = sdslen(gp->member);

            if (maxelelen < elelen) maxelelen = elelen;
            totelelen += elelen;
            if (result) {
                zset *zs = result->ptr;
                zskiplistNode *znode = zslInsert(zs->zsl, score, gp->member);

                serverAssert(dictAdd(zs->dict, znode, NULL) == DICT_OK);
            }
            sdsfree(gp->member);   /* zslInsert copied it, or nothing took it */
            gp->member = NULL;
        }

        if (returned_items) {
            zsetConvertToListpackIfNeeded(result, maxelelen, totelelen);
            rl_object_set_zset(storekey, result);
            rl_pushint(L, returned_items);
        } else {
            /* Redis deletes the destination key in this branch; the handle
             * stays, so it becomes the empty set a missing key reads as. */
            rl_object_set_zset(storekey, zsetTypeCreate(0, 0));
            rl_pushint(L, 0);
        }
    }
    rl_geoArrayFree(ga);
    return 1;
}

/* GEORADIUS key x y radius unit ...                          (geo.c:848) */
static int
lgeoradius(lua_State *L) {
    return rl_geosearch_generic(L, 1, RL_RADIUS_COORDS);
}

/* GEORADIUS_RO -- the same, without STORE / STOREDIST.       (geo.c:858) */
static int
lgeoradius_ro(lua_State *L) {
    return rl_geosearch_generic(L, 1, RL_RADIUS_COORDS|RL_RADIUS_NOSTORE);
}

/* GEORADIUSBYMEMBER key member radius unit ...               (geo.c:853) */
static int
lgeoradiusbymember(lua_State *L) {
    return rl_geosearch_generic(L, 1, RL_RADIUS_MEMBER);
}

/* GEORADIUSBYMEMBER_RO                                       (geo.c:863) */
static int
lgeoradiusbymember_ro(lua_State *L) {
    return rl_geosearch_generic(L, 1, RL_RADIUS_MEMBER|RL_RADIUS_NOSTORE);
}

/* GEOSEARCH key [FROMMEMBER|FROMLONLAT] [BYRADIUS|BYBOX] ...  (geo.c:867) */
static int
lgeosearch(lua_State *L) {
    return rl_geosearch_generic(L, 1, RL_GEOSEARCH);
}

/* GEOSEARCHSTORE dst src ...                                  (geo.c:871)
 * dst is argument 1, so the method call form is dst:geosearchstore(src, ...). */
static int
lgeosearchstore(lua_State *L) {
    return rl_geosearch_generic(L, 2, RL_GEOSEARCH|RL_GEOSEARCHSTORE);
}

/* ---- registration ------------------------------------------------------- */

static const luaL_Reg geo_methods[] = {
    {"geoadd", lgeoadd},
    {"geodist", lgeodist},
    {"geohash", lgeohash},
    {"geopos", lgeopos},
    {"georadius", lgeoradius},
    {"georadius_ro", lgeoradius_ro},
    {"georadiusbymember", lgeoradiusbymember},
    {"georadiusbymember_ro", lgeoradiusbymember_ro},
    {"geosearch", lgeosearch},
    {"geosearchstore", lgeosearchstore},
    {"free", lgeo_free},
    {NULL, NULL},
};

static const luaL_Reg geo_module_methods[] = {
    {"new", lgeo_new},
    {"geosearchstore", lgeosearchstore},
    {NULL, NULL},
};

/* LuaJIT's searcher uses the full module name with dots converted to
 * underscores (same rule as NetBull's luaopen_skiplist_c for "skiplist.c"). */
int luaopen_redis_geo(lua_State *L) {
    rl_register_class(L, RL_GEO, geo_methods, NULL);
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, geo_module_methods, 0);
    return 1;
}
