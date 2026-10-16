/*
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

#include "redislib_port.h"
#include "intset.h"  /* Compact integer set structure */
#include "hyperloglog.h"

/*-----------------------------------------------------------------------------
 * Set Commands
 *----------------------------------------------------------------------------*/

/* When estimating cardinality with HLL and a LIMIT is set, this is the minimum
 * number of additional elements to process between two hllCount() checks. */
#define HLL_CHECK_INTERVAL_FLOOR 1024

/* Factory method to return a set that *can* hold "value". When the object has
 * an integer-encodable value, an intset will be returned. Otherwise a listpack
 * or a regular hash table.
 *
 * The size hint indicates approximately how many items will be added which is
 * used to determine the initial representation. */
robj *setTypeCreate(sds value, size_t size_hint) {
    if (isSdsRepresentableAsLongLong(value,NULL) == C_OK && size_hint <= server.set_max_intset_entries)
        return createIntsetObject();
    if (size_hint <= server.set_max_listpack_entries)
        return createSetListpackObject();

    /* We may oversize the set by using the hint if the hint is not accurate,
     * but we will assume this is acceptable to maximize performance. */
    robj *o = createSetObject();
    dictExpand(o->ptr, size_hint);
    return o;
}

/* Check if the existing set should be converted to another encoding based off the
 * the size hint. */
void setTypeMaybeConvert(robj *set, size_t size_hint) {
    if ((set->encoding == OBJ_ENCODING_LISTPACK && size_hint > server.set_max_listpack_entries)
        || (set->encoding == OBJ_ENCODING_INTSET && size_hint > server.set_max_intset_entries))
    {
        setTypeConvertAndExpand(set, OBJ_ENCODING_HT, size_hint, 1);
    }
}

/* Return the maximum number of entries to store in an intset. */
static size_t intsetMaxEntries(void) {
    size_t max_entries = server.set_max_intset_entries;
    /* limit to 1G entries due to intset internals. */
    if (max_entries >= 1<<30) max_entries = 1<<30;
    return max_entries;
}

/* Converts intset to HT if it contains too many entries. */
static void maybeConvertIntset(robj *subject) {
    serverAssert(subject->encoding == OBJ_ENCODING_INTSET);
    if (intsetLen(subject->ptr) > intsetMaxEntries())
        setTypeConvert(subject,OBJ_ENCODING_HT);
}

/* Add the specified sds value into a set.
 *
 * If the value was already member of the set, nothing is done and 0 is
 * returned, otherwise the new element is added and 1 is returned. */
int setTypeAdd(robj *subject, sds value) {
    return setTypeAddAux(subject, value, sdslen(value), 0, 1);
}

/* Add member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was added and 0 if it was already a member. */
int setTypeAddAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET) {
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr, llval, &success);
            if (success) maybeConvertIntset(set);
            return success;
        }
        /* Convert int to string. */
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    serverAssert(str);
    if (set->encoding == OBJ_ENCODING_HT) {
        /* Avoid duping the string if it is an sds string. */
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        dict *ht = set->ptr;
        dictEntryLink bucket, link = dictFindLink(ht, sdsval, &bucket);
        if (link == NULL) {
            /* Key doesn't already exist in the set. Add it but dup the key. */
            if (sdsval == str) sdsval = sdsdup(sdsval);
            dictSetKeyAtLink(ht, sdsval, &bucket, 1);
            *htGetMetadataSize(ht) += sdsAllocSize(sdsval);
            return 1;
        } else if (sdsval != str) {
            /* String is already a member. Free our temporary sds copy. */
            sdsfree(sdsval);
            return 0;
        }
    } else if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        if (p != NULL)
            p = lpFind(lp, p, (unsigned char*)str, len, 0);
        if (p == NULL) {
            /* Not found.  */
            if (lpLength(lp) < server.set_max_listpack_entries &&
                len <= server.set_max_listpack_value &&
                lpSafeToAdd(lp, len))
            {
                if (str == tmpbuf) {
                    /* This came in as integer so we can avoid parsing it again.
                     * TODO: Create and use lpFindInteger; don't go via string. */
                    lp = lpAppendInteger(lp, llval);
                } else {
                    lp = lpAppend(lp, (unsigned char*)str, len);
                }
                set->ptr = lp;
            } else {
                /* Size limit is reached. Convert to hashtable and add. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT, lpLength(lp) + 1, 1);
                sds newval = sdsnewlen(str,len);
                serverAssert(dictAdd(set->ptr,newval,NULL) == DICT_OK);
                *htGetMetadataSize(set->ptr) += sdsAllocSize(newval);
            }
            return 1;
        }
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long value;
        if (string2ll(str, len, &value)) {
            uint8_t success = 0;
            set->ptr = intsetAdd(set->ptr,value,&success);
            if (success) {
                maybeConvertIntset(set);
                return 1;
            }
        } else {
            /* Check if listpack encoding is safe not to cross any threshold. */
            size_t maxelelen = 0, totsize = 0;
            unsigned long n = intsetLen(set->ptr);
            if (n != 0) {
                size_t elelen1 = sdigits10(intsetMax(set->ptr));
                size_t elelen2 = sdigits10(intsetMin(set->ptr));
                maxelelen = max(elelen1, elelen2);
                size_t s1 = lpEstimateBytesRepeatedInteger(intsetMax(set->ptr), n);
                size_t s2 = lpEstimateBytesRepeatedInteger(intsetMin(set->ptr), n);
                totsize = max(s1, s2);
            }
            if (intsetLen((const intset*)set->ptr) < server.set_max_listpack_entries &&
                len <= server.set_max_listpack_value &&
                maxelelen <= server.set_max_listpack_value &&
                lpSafeToAdd(NULL, totsize + len))
            {
                /* In the "safe to add" check above we assumed all elements in
                 * the intset are of size maxelelen. This is an upper bound. */
                setTypeConvertAndExpand(set, OBJ_ENCODING_LISTPACK,
                                        intsetLen(set->ptr) + 1, 1);
                unsigned char *lp = set->ptr;
                lp = lpAppend(lp, (unsigned char *)str, len);
                lp = lpShrinkToFit(lp);
                set->ptr = lp;
                return 1;
            } else {
                setTypeConvertAndExpand(set, OBJ_ENCODING_HT,
                                        intsetLen(set->ptr) + 1, 1);
                /* The set *was* an intset and this value is not integer
                 * encodable, so dictAdd should always work. */
                sds newval = sdsnewlen(str,len);
                serverAssert(dictAdd(set->ptr,newval,NULL) == DICT_OK);
                *htGetMetadataSize(set->ptr) += sdsAllocSize(newval);
                return 1;
            }
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Deletes a value provided as an sds string from the set. Returns 1 if the
 * value was deleted and 0 if it was not a member of the set. */
int setTypeRemove(robj *setobj, sds value) {
    return setTypeRemoveAux(setobj, value, sdslen(value), 0, 1);
}

/* Remove a member. This function is optimized for the different encodings. The
 * value can be provided as an sds string (indicated by passing str_is_sds =
 * 1), as string and length (str_is_sds = 0) or as an integer in which case str
 * is set to NULL and llval is provided instead.
 *
 * Returns 1 if the value was deleted and 0 if it was not a member of the set. */
int setTypeRemoveAux(robj *setobj, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (setobj->encoding == OBJ_ENCODING_INTSET) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            return success;
        }
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (setobj->encoding == OBJ_ENCODING_HT) {
        sds sdsval = str_is_sds ? (sds)str : sdsnewlen(str, len);
        int deleted = (dictDelete(setobj->ptr, sdsval) == DICT_OK);
        if (sdsval != str) sdsfree(sdsval); /* free temp copy */
        return deleted;
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = setobj->ptr;
        unsigned char *p = lpFirst(lp);
        if (p == NULL) return 0;
        p = lpFind(lp, p, (unsigned char*)str, len, 0);
        if (p != NULL) {
            lp = lpDelete(lp, p, NULL);
            setobj->ptr = lp;
            return 1;
        }
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        if (string2ll(str, len, &llval)) {
            int success;
            setobj->ptr = intsetRemove(setobj->ptr,llval,&success);
            if (success) return 1;
        }
    } else {
        serverPanic("Unknown set encoding");
    }
    return 0;
}

/* Check if an sds string is a member of the set. Returns 1 if the value is a
 * member of the set and 0 if it isn't. */
int setTypeIsMember(robj *subject, sds value) {
    return setTypeIsMemberAux(subject, value, sdslen(value), 0, 1);
}

/* Membership checking optimized for the different encodings. The value can be
 * provided as an sds string (indicated by passing str_is_sds = 1), as string
 * and length (str_is_sds = 0) or as an integer in which case str is set to NULL
 * and llval is provided instead.
 *
 * Returns 1 if the value is a member of the set and 0 if it isn't. */
int setTypeIsMemberAux(robj *set, char *str, size_t len, int64_t llval, int str_is_sds) {
    char tmpbuf[LONG_STR_SIZE];
    if (!str) {
        if (set->encoding == OBJ_ENCODING_INTSET)
            return intsetFind(set->ptr, llval);
        len = ll2string(tmpbuf, sizeof tmpbuf, llval);
        str = tmpbuf;
        str_is_sds = 0;
    }

    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        return p && lpFind(lp, p, (unsigned char*)str, len, 0);
    } else if (set->encoding == OBJ_ENCODING_INTSET) {
        long long llval;
        return string2ll(str, len, &llval) && intsetFind(set->ptr, llval);
    } else if (set->encoding == OBJ_ENCODING_HT && str_is_sds) {
        return dictFind(set->ptr, (sds)str) != NULL;
    } else if (set->encoding == OBJ_ENCODING_HT) {
        sds sdsval = sdsnewlen(str, len);
        int result = dictFind(set->ptr, sdsval) != NULL;
        sdsfree(sdsval);
        return result;
    } else {
        serverPanic("Unknown set encoding");
    }
}

void setTypeInitIterator(setTypeIterator *si, robj *subject) {
    si->subject = subject;
    si->encoding = subject->encoding;
    if (si->encoding == OBJ_ENCODING_HT) {
        dictInitIterator(&si->di, subject->ptr);
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        si->ii = 0;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        si->lpi = NULL;
    } else {
        serverPanic("Unknown set encoding");
    }
}

void setTypeResetIterator(setTypeIterator *si) {
    if (si->encoding == OBJ_ENCODING_HT)
        dictResetIterator(&si->di);
}

/* Move to the next entry in the set. Returns the object at the current
 * position, as a string or as an integer.
 *
 * Since set elements can be internally be stored as SDS strings, char buffers or
 * simple arrays of integers, setTypeNext returns the encoding of the
 * set object you are iterating, and will populate the appropriate pointers
 * (str and len) or (llele) depending on whether the value is stored as a string
 * or as an integer internally.
 *
 * If OBJ_ENCODING_HT is returned, then str points to an sds string and can be
 * used as such. If OBJ_ENCODING_INTSET, then llele is populated and str is
 * pointed to NULL. If OBJ_ENCODING_LISTPACK is returned, the value can be
 * either a string or an integer. If *str is not NULL, then str and len are
 * populated with the string content and length. Otherwise, llele populated with
 * an integer value.
 *
 * Note that str, len and llele pointers should all be passed and cannot
 * be NULL since the function will try to defensively populate the non
 * used field with values which are easy to trap if misused.
 *
 * When there are no more elements -1 is returned. */
int setTypeNext(setTypeIterator *si, char **str, size_t *len, int64_t *llele) {
    if (si->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictNext(&si->di);
        if (de == NULL) return -1;
        *str = dictGetKey(de);
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (si->encoding == OBJ_ENCODING_INTSET) {
        if (!intsetGet(si->subject->ptr,si->ii++,llele))
            return -1;
        *str = NULL;
    } else if (si->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = si->subject->ptr;
        unsigned char *lpi = si->lpi;
        if (lpi == NULL) {
            lpi = lpFirst(lp);
        } else {
            lpi = lpNext(lp, lpi);
        }
        if (lpi == NULL) return -1;
        si->lpi = lpi;
        unsigned int l = 0;
        *str = (char *)lpGetValue(lpi, &l, (long long *)llele);
        *len = (size_t)l;
    } else {
        serverPanic("Wrong set encoding in setTypeNext");
    }
    return si->encoding;
}

/* The not copy on write friendly version but easy to use version
 * of setTypeNext() is setTypeNextObject(), returning new SDS
 * strings. So if you don't retain a pointer to this object you should call
 * sdsfree() against it.
 *
 * This function is the way to go for write operations where COW is not
 * an issue. */
sds setTypeNextObject(setTypeIterator *si) {
    int64_t intele = 0;
    char *str;
    size_t len = 0;

    if (setTypeNext(si, &str, &len, &intele) == -1) return NULL;
    if (str != NULL) return sdsnewlen(str, len);
    return sdsfromlonglong(intele);
}

/* Return random element from a non empty set.
 * The returned element can be an int64_t value if the set is encoded
 * as an "intset" blob of integers, or an string.
 *
 * The caller provides three pointers to be populated with the right
 * object. The return value of the function is the object->encoding
 * field of the object and can be used by the caller to check if the
 * int64_t pointer or the str and len pointers were populated, as for
 * setTypeNext. If OBJ_ENCODING_HT is returned, str is pointed to a
 * string which is actually an sds string and it can be used as such.
 *
 * Note that both the str, len and llele pointers should be passed and cannot
 * be NULL. If str is set to NULL, the value is an integer stored in llele. */
int setTypeRandomElement(robj *setobj, char **str, size_t *len, int64_t *llele) {
    if (setobj->encoding == OBJ_ENCODING_HT) {
        dictEntry *de = dictGetFairRandomKey(setobj->ptr);
        *str = dictGetKey(de);
        *len = sdslen(*str);
        *llele = -123456789; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_INTSET) {
        *llele = intsetRandom(setobj->ptr);
        *str = NULL; /* Not needed. Defensive. */
    } else if (setobj->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = setobj->ptr;
        int r = rand() % lpLength(lp);
        unsigned char *p = lpSeek(lp, r);
        unsigned int l;
        *str = (char *)lpGetValue(p, &l, (long long *)llele);
        *len = (size_t)l;
    } else {
        serverPanic("Unknown set encoding");
    }
    return setobj->encoding;
}

unsigned long setTypeSize(const robj *subject) {
    if (subject->encoding == OBJ_ENCODING_HT) {
        return dictSize((const dict*)subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_INTSET) {
        return intsetLen((const intset*)subject->ptr);
    } else if (subject->encoding == OBJ_ENCODING_LISTPACK) {
        return lpLength((unsigned char *)subject->ptr);
    } else {
        serverPanic("Unknown set encoding");
    }
}

size_t setTypeAllocSize(const robj *o) {
    serverAssertWithInfo(NULL,o,o->type == OBJ_SET);
    size_t size = 0;
    if (o->encoding == OBJ_ENCODING_HT) {
        dict *d = o->ptr;
        size += sizeof(dict) + dictMemUsage(d) + *htGetMetadataSize(d);
    } else if (o->encoding == OBJ_ENCODING_INTSET) {
        size = intsetAllocSize(o->ptr);
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        size = lpBytes(o->ptr);
    } else {
        serverPanic("Unknown set encoding");
    }
    return size;
}

/* Convert the set to specified encoding. The resulting dict (when converting
 * to a hash table) is presized to hold the number of elements in the original
 * set. */
void setTypeConvert(robj *setobj, int enc) {
    setTypeConvertAndExpand(setobj, enc, setTypeSize(setobj), 1);
}

/* Converts a set to the specified encoding, pre-sizing it for 'cap' elements.
 * The 'panic' argument controls whether to panic on OOM (panic=1) or return
 * C_ERR on OOM (panic=0). If panic=1 is given, this function always returns
 * C_OK. */
int setTypeConvertAndExpand(robj *setobj, int enc, unsigned long cap, int panic) {
    setTypeIterator si;
    serverAssertWithInfo(NULL,setobj,setobj->type == OBJ_SET &&
                             setobj->encoding != enc);

    if (enc == OBJ_ENCODING_HT) {
        dict *d = dictCreate(&setDictType);
        sds element;

        /* Presize the dict to avoid rehashing */
        if (panic) {
            dictExpand(d, cap);
        } else if (dictTryExpand(d, cap) != DICT_OK) {
            dictRelease(d);
            return C_ERR;
        }

        /* To add the elements we extract integers and create redis objects */
        size_t *alloc_size = htGetMetadataSize(d);
        setTypeInitIterator(&si, setobj);
        while ((element = setTypeNextObject(&si)) != NULL) {
            serverAssert(dictAdd(d,element,NULL) == DICT_OK);
            *alloc_size += sdsAllocSize(element);
        }
        setTypeResetIterator(&si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_HT;
        setobj->ptr = d;
    } else if (enc == OBJ_ENCODING_LISTPACK) {
        /* Preallocate the minimum two bytes per element (enc/value + backlen) */
        size_t estcap = cap * 2;
        if (setobj->encoding == OBJ_ENCODING_INTSET && setTypeSize(setobj) > 0) {
            /* If we're converting from intset, we have a better estimate. */
            size_t s1 = lpEstimateBytesRepeatedInteger(intsetMin(setobj->ptr), cap);
            size_t s2 = lpEstimateBytesRepeatedInteger(intsetMax(setobj->ptr), cap);
            estcap = max(s1, s2);
        }
        unsigned char *lp = lpNew(estcap);
        char *str;
        size_t len = 0;
        int64_t llele = 0;
        setTypeInitIterator(&si, setobj);
        while (setTypeNext(&si, &str, &len, &llele) != -1) {
            if (str != NULL)
                lp = lpAppend(lp, (unsigned char *)str, len);
            else
                lp = lpAppendInteger(lp, llele);
        }
        setTypeResetIterator(&si);

        freeSetObject(setobj); /* frees the internals but not setobj itself */
        setobj->encoding = OBJ_ENCODING_LISTPACK;
        setobj->ptr = lp;
    } else {
        serverPanic("Unsupported set conversion");
    }
    return C_OK;
}

/* This is a helper function for the COPY command.
 * Duplicate a set object, with the guarantee that the returned object
 * has the same encoding as the original one.
 *
 * The resulting object always has refcount set to 1 */
robj *setTypeDup(robj *o) {
    robj *set;

    serverAssert(o->type == OBJ_SET);

    /* Create a new set object that have the same encoding as the original object's encoding */
    if (o->encoding == OBJ_ENCODING_INTSET) {
        intset *is = o->ptr;
        size_t size = intsetBlobLen(is);
        intset *newis = zmalloc(size);
        memcpy(newis,is,size);
        set = createObject(OBJ_SET, newis);
        set->encoding = OBJ_ENCODING_INTSET;
    } else if (o->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = o->ptr;
        size_t sz = lpBytes(lp);
        unsigned char *new_lp = zmalloc(sz);
        memcpy(new_lp, lp, sz);
        set = createObject(OBJ_SET, new_lp);
        set->encoding = OBJ_ENCODING_LISTPACK;
    } else if (o->encoding == OBJ_ENCODING_HT) {
        set = createSetObject();
        dict *d = o->ptr;
        dictExpand(set->ptr, dictSize(d));
        setTypeIterator si;
        setTypeInitIterator(&si, o);
        char *str;
        size_t len = 0;
        int64_t intobj = 0;
        while (setTypeNext(&si, &str, &len, &intobj) != -1) {
            setTypeAdd(set, (sds)str);
        }
        setTypeResetIterator(&si);
    } else {
        serverPanic("Unknown set encoding");
    }
    return set;
}
