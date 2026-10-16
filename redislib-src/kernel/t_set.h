/* t_set.h -- skynetjit redislib: set types and kernel API.
 *
 * Extracted verbatim from Redis 8.10.0 src/server.h (the block that t_set.c
 * needs no longer lives in server.h, which is not part of this port). Same
 * technique as kernel/t_zset.h; see redislib-src/NOTICE for provenance and the
 * list of mechanical edits.
 */

#ifndef REDISLIB_T_SET_H
#define REDISLIB_T_SET_H

#include <stdint.h>
#include "sds.h"
#include "dict.h"

/* Structure to hold set iteration abstraction.  src/server.h:3148-3156 */
typedef struct {
    robj *subject;
    int encoding;
    int ii; /* intset iterator */
    dictIterator di;
    unsigned char *lpi; /* listpack iterator */
} setTypeIterator;

/* --- set kernel API (src/server.h:3918-3934) -----------------------------
 * setTypePopRandom() is not among them: it returned a refcounted string object,
 * its only callers were the two SPOP command functions removed with the command
 * layer, and this port's robj has no refcount on purpose (NOTICE 2, 4, 6). */
robj *setTypeCreate(sds value, size_t size_hint);
int setTypeAdd(robj *subject, sds value);
int setTypeAddAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
int setTypeRemove(robj *subject, sds value);
int setTypeRemoveAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
int setTypeIsMember(robj *subject, sds value);
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds);
void setTypeInitIterator(setTypeIterator *si, robj *subject);
void setTypeResetIterator(setTypeIterator *si);
int setTypeNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele);
sds setTypeNextObject(setTypeIterator *si);
int setTypeRandomElement(robj *setobj, char **str, size_t *len, int64_t *llele);
unsigned long setTypeSize(const robj *subject);
size_t setTypeAllocSize(const robj *o);
void setTypeConvert(robj *subject, int enc);
int setTypeConvertAndExpand(robj *setobj, int enc, unsigned long cap, int panic);
robj *setTypeDup(robj *o);

#endif /* REDISLIB_T_SET_H */
