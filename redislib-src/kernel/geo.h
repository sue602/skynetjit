/* skynetjit port: include shim applied (redislib_port.h), see redislib-src/NOTICE */
#ifndef __GEO_H__
#define __GEO_H__

#include "redislib_port.h"

/* Structures used inside geo.c in order to represent points and array of
 * points on the earth. */
typedef struct geoPoint {
    double longitude;
    double latitude;
    double dist;
    double score;
    char *member;
} geoPoint;

typedef struct geoArray {
    struct geoPoint *array;
    size_t buckets;
    size_t used;
} geoArray;

#endif
