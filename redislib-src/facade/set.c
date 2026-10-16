/* set.c -- the redis.set facade: Redis 8.10.0's set commands over an in-memory
 * set object (intset, listpack or hashtable encoded, exactly like a real Redis
 * set).
 *
 * Faithful transcription of Redis 8.10.0 src/t_set.c's command layer with the
 * client / keyspace layer removed: the key lookup becomes the rl_object's own
 * robj, addReply* becomes the rl_push* mapping (see rl_common.h), and every
 * algorithmic step is a call into the ported kernel (setTypeCreate / setTypeAdd /
 * setTypeRemove / setTypeIsMember / the setType* iterator / setTypeRandomElement /
 * the payload factories object.c provides) rather than a rewrite. Error texts are
 * the upstream strings.
 *
 * The set operations are transcribed from sinterGenericCommand() and
 * sunionDiffGenericCommand(), including the cardinality sort, the DIFF 1-versus-2
 * cost heuristic and the same-operand shortcut: those live in t_set.c's command
 * layer (which edit B deletes), so this file is their new home -- the same
 * treatment facade/geo_sem.c gives geo.c's non-command half. They build their
 * result out of the exported kernel primitives, so nothing had to be copied here
 * the way the zset family had to copy its file-local iterator types.
 *
 * What the port could not inherit: setTypePopRandom() came back as a refcounted
 * string object, its only callers were the two SPOP commands, and this port's robj
 * has no refcount on purpose (redislib_port.h). SPOP therefore samples with
 * setTypeRandomElement() + setTypeRemoveAux(), which is what that function's own
 * generic branch did, and facade/sortedset.c's ZRANDMEMBER is the precedent for
 * answering a random sample from the exported primitives. Same elements, same
 * distribution; every such site is marked `rl:` here and listed in NOTICE 6.
 */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <limits.h>

#include "rl_common.h"

/* t_set.c:61 setTypeMaybeConvert(). Upstream gives it no prototype anywhere -- it
 * is not in server.h, and its only caller was saddCommand() -- so the declaration
 * the deleted caller relied on has to be made here. NOTICE 4 records the rule:
 * declare, next to the kernel file you mirror, what the kernel exports but
 * server.h never named. */
void setTypeMaybeConvert(robj *set, size_t size_hint);

/* t_zset.c:2897, upstream's own reply when a transient operand array cannot be
 * allocated, reused for this family's operand arrays. */
static const char *setop_oom =
    "Insufficient memory, failed allocating transient memory, too many args.";

/* ---- argument helpers --------------------------------------------------- */

/* The payload of the handle at idx, which may be NULL: "no value yet" is exactly
 * the key a server cannot look up, and every command of this file answers that
 * case the way lookupKey*OrReply() does -- 0, the empty array, or nil. */
static robj *
rl_set(lua_State *L, int idx) {
    return rl_check(L, idx, RL_SET)->sobj;
}

/* An argument a Redis command would have got from argv and the Lua caller did not
 * pass: the server stops at its command table's arity field, and this port has no
 * command table, so the same line is said here (NOTICE 7). Presence only -- the
 * kind is rl_checkmember()'s and rl_member()'s business, and checking it separately
 * is what lets a command validate all of its elements before it allocates one. */
static void
rl_setneed(lua_State *L, int idx, const char *cmdname) {
    if (lua_gettop(L) < idx)
        rl_error(L, "wrong number of arguments for '%s' command", cmdname);
}

/* A member argument, both failures in the order a server produces them. */
static sds
rl_setmember(lua_State *L, int idx, const char *cmdname) {
    rl_setneed(L, idx, cmdname);
    return rl_member(L, idx);
}

/* The fixed-arity commands of this family (scard, smembers, sismember, smove: the
 * command table gives those a hard argv count, so extra arguments are arity errors
 * too). */
static void
rl_setarity(lua_State *L, int want, const char *cmdname) {
    if (lua_gettop(L) != want)
        rl_error(L, "wrong number of arguments for '%s' command", cmdname);
}

/* getRangeLongFromObjectOrReply() (object.c:1270) with no caller message, which is
 * how SRANDMEMBER reads its count: the two getLongFromObjectOrReply() texts are
 * already inside rl_checklong(), and the range text is upstream's formatted line. */
static long
rl_rangelong(lua_State *L, int idx, long lo, long hi) {
    long v = rl_checklong(L, idx);

    if (v < lo || v > hi)
        rl_error(L, "value is out of range, value must between %ld and %ld", lo, hi);
    return v;
}

/* getPositiveLongFromObjectOrReply() (object.c:1283), which passes its message down
 * to the integer reader, so the same text answers a non-integer and a negative
 * value. SPOP calls it with its default message; SINTERCARD and friends call
 * getPositiveLongFromObjectOrReply() with their own, hence the `msg` argument. */
static long
rl_poslong_msg(lua_State *L, int idx, const char *msg) {
    long long v;

    if (rl_tolong(L, idx, &v) != C_OK || v < 0 || v > LONG_MAX)
        rl_error(L, "%s", msg);
    return (long)v;
}

/* Push one iterated element the way Redis replies it: a hashtable member is an sds
 * string, an intset element and a listpack integer entry are
 * addReplyBulkLongLong()'s decimal text. A member is a Lua string either way (spec
 * 3.2), which is what makes SMEMBERS of the set {1,2} answer {"1","2"}. */
static void
rl_push_setele(lua_State *L, char *str, size_t len, int64_t llele) {
    if (str != NULL) {
        lua_pushlstring(L, str, len);
    } else {
        char buf[LONG_STR_SIZE];
        lua_pushlstring(L, buf, ll2string(buf, sizeof(buf), (long long)llele));
    }
}

/* The same for one listpackEntry, which is what the batched listpack samplers hand
 * back (t_set.c:1180 turns sval into a bulk and lval into a bulk long long). */
static void
rl_push_lpentry(lua_State *L, listpackEntry *ele) {
    if (ele->sval) lua_pushlstring(L, (const char*)ele->sval, ele->slen);
    else {
        char buf[LONG_STR_SIZE];
        lua_pushlstring(L, buf, ll2string(buf, sizeof(buf), ele->lval));
    }
}

/* ---- object lifecycle --------------------------------------------------- */

/* Redis deletes a set the moment it empties, so the next SADD of that key goes
 * through setTypeCreate() with its own first member. Releasing the payload here is
 * that deletion: the handle itself stays addressable (NOTICE 6's object model), and
 * every read of it now answers what a server answers for a missing key. */
static void
rl_set_reset(rl_object *o) {
    rl_free_set(o->sobj);
    o->sobj = NULL;
}

/* set.new() => an empty set. There is no such object on a server -- an empty set
 * is a key that is not there -- so this creates no payload at all, and the first
 * write decides the encoding exactly like the first SADD decides it. */
static int
lset_new(lua_State *L) {
    rl_newobject(L, RL_SET);
    return 1;
}

static int
lset_free(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_SET);
    rl_free_ud(L, o);
    return 0;
}

/* ---- dump / load (spec 4, not Redis commands) --------------------------- */

/* s:dump() => string */
static int
lset_dump(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_SET);
    sds blob = rl_dump_set(o->sobj);

    lua_pushlstring(L, blob, sdslen(blob));
    sdsfree(blob);
    return 1;
}

/* set.load(string) => set */
static int
lset_load(lua_State *L) {
    rl_object *o;
    size_t len;
    const char *buf = rl_checkblob(L, 1, &len);

    /* The handle is pushed before the payload is parsed, so a raise in
     * rl_load_set() leaves an empty collectible object instead of an allocated set
     * nobody points at. Same shape as sortedset.load(). */
    rl_newobject(L, RL_SET);
    o = (rl_object*)lua_touserdata(L, -1);
    o->sobj = rl_load_set(L, buf, len);
    return 1;
}

/* ---- SADD / SREM -------------------------------------------------------- */

/* SADD key member [member ...] => :added                          (t_set.c:626)
 *
 * The key-not-there branch is setTypeCreate(first member, how many members this
 * call brings); the existing-key branch is setTypeMaybeConvert(), what t_set.c:643
 * does before the loop so a large SADD does not grow a listpack one element at a
 * time. */
static int
lsadd(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_SET);
    robj *set = o->sobj;
    int top = lua_gettop(L), j, added = 0;
    size_t n;

    rl_setneed(L, 2, "sadd");                    /* arity + nothing allocated yet */
    n = (size_t)(top - 1);
    for (j = 2; j <= top; j++) rl_checkmember(L, j);

    if (set != NULL) setTypeMaybeConvert(set, n);

    for (j = 2; j <= top; j++) {
        sds ele = rl_member(L, j);

        if (set == NULL) set = o->sobj = setTypeCreate(ele, n);
        added += setTypeAdd(set, ele);
        sdsfree(ele);                             /* the set owns its copy */
    }
    rl_pushint(L, added);
    return 1;
}

/* SREM key member [member ...] => :deleted                        (t_set.c:663)
 *
 * Upstream stops as soon as the set empties -- that is the key's deletion -- and
 * skips the auto-resize bookkeeping with it. Same break, same skip, and the released
 * payload below is the dbDelete() that follows there. */
static int
lsrem(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_SET);
    robj *set = o->sobj;
    int top = lua_gettop(L), j, deleted = 0, emptied = 0;

    rl_setneed(L, 2, "srem");
    for (j = 2; j <= top; j++) rl_checkmember(L, j);
    if (set == NULL) { rl_pushint(L, 0); return 1; }

    /* The two dictionary bookkeeping calls of sremCommand(): a set payload is a
     * dict* only in the hashtable encoding, so it is cast, as upstream casts it. */
    if (set->encoding == OBJ_ENCODING_HT) dictPauseAutoResize((dict*)set->ptr);

    for (j = 2; j <= top; j++) {
        sds ele = rl_member(L, j);

        if (setTypeRemove(set, ele)) deleted++;
        sdsfree(ele);
        if (setTypeSize(set) == 0) { emptied = 1; break; }
    }

    if (!emptied && set->encoding == OBJ_ENCODING_HT) {
        dictResumeAutoResize((dict*)set->ptr);
        dictShrinkIfNeeded((dict*)set->ptr);
    }
    if (emptied) rl_set_reset(o);
    rl_pushint(L, deleted);
    return 1;
}

/* ---- SCARD / SMEMBERS / SISMEMBER / SMISMEMBER -------------------------- */

/* SCARD key => :size                                              (t_set.c:821) */
static int
lscard(lua_State *L) {
    robj *set;

    rl_setarity(L, 1, "scard");
    set = rl_set(L, 1);
    rl_pushint(L, set ? (long long)setTypeSize(set) : 0);
    return 1;
}

/* One full walk of a payload, pushing every member: SMEMBERS' loop and the two
 * "reply the whole set" branches of SPOP and SRANDMEMBER. */
static unsigned long
rl_set_push_all(lua_State *L, robj *set, int tab) {
    setTypeIterator si;
    unsigned long n = 0;

    setTypeInitIterator(&si, set);
    for (;;) {
        char *str;
        size_t len = 0;
        int64_t llele = 0;

        if (setTypeNext(&si, &str, &len, &llele) == -1) break;
        rl_push_setele(L, str, len, llele);
        lua_rawseti(L, tab, (int)++n);
    }
    setTypeResetIterator(&si);
    return n;
}

/* SMEMBERS key => [member...]                                   (t_set.c:1579)
 *
 * The order is the payload's own, which is Redis' order for the same encoding and
 * which Redis promises nothing about -- SMEMBERS is a set, and tools/redis-diff
 * compares it as one. */
static int
lsmembers(lua_State *L) {
    robj *set;
    unsigned long n;
    int tab;

    rl_setarity(L, 1, "smembers");
    set = rl_set(L, 1);
    if (set == NULL) { lua_createtable(L, 0, 0); return 1; }

    lua_createtable(L, (int)setTypeSize(set), 0);
    tab = lua_gettop(L);
    n = rl_set_push_all(L, set, tab);
    serverAssert(n == setTypeSize(set));     /* smembersCommand()'s own length check */
    lua_pushvalue(L, tab);
    return 1;
}

/* SISMEMBER key member => :1|:0                                 (t_set.c:783) */
static int
lsismember(lua_State *L) {
    robj *set;
    sds ele;
    int found;

    rl_setarity(L, 2, "sismember");
    set = rl_set(L, 1);
    ele = rl_member(L, 2);
    /* rl: a missing key answers 0 there (lookupKeyReadOrReply), an untouched handle
     * answers 0 here. */
    found = (set != NULL) ? setTypeIsMember(set, ele) : 0;
    sdsfree(ele);
    rl_pushint(L, found);
    return 1;
}

/* SMISMEMBER key member [member ...] => [:1|:0 ...]              (t_set.c:800)
 *
 * "Don't abort when the key cannot be found. Non-existing keys are empty sets, where
 * SMISMEMBER should respond with a series of zeros." */
static int
lsmismember(lua_State *L) {
    robj *set;
    int top = lua_gettop(L), j;

    rl_setneed(L, 2, "smismember");
    set = rl_set(L, 1);
    for (j = 2; j <= top; j++) rl_checkmember(L, j);

    lua_createtable(L, top - 2, 0);
    for (j = 2; j <= top; j++) {
        sds ele = rl_member(L, j);
        int found = (set != NULL) ? setTypeIsMember(set, ele) : 0;

        sdsfree(ele);
        rl_pushint(L, found);
        lua_rawseti(L, -2, j - 1);
    }
    return 1;
}

/* ---- SMOVE -------------------------------------------------------------- */

/* SMOVE src dst member => :1|:0                                 (t_set.c:711)
 *
 * The receiver is the source, like Redis' argv[1], and the destination is argument 2
 * in both call shapes, so this one function serves `src:smove(dst, m)` and
 * `set.smove(src, dst, m)`. */
static int
lsmove(lua_State *L) {
    rl_object *src, *dst;
    sds ele;
    int moved;

    rl_setarity(L, 3, "smove");
    src = rl_check(L, 1, RL_SET);
    /* "If the source key does not exist return 0" comes before either type check
     * upstream, so an empty source answers 0 without judging the destination; the
     * destination's WRONGTYPE is only said when there is something to move, exactly
     * as on a server. */
    if (src->sobj == NULL || setTypeSize(src->sobj) == 0) { rl_pushint(L, 0); return 1; }
    dst = rl_check(L, 2, RL_SET);
    ele = rl_member(L, 3);

    if (src->sobj == dst->sobj) {
        /* srcset == dstset: SMOVE is a no-op whose reply is membership. */
        moved = setTypeIsMember(src->sobj, ele);
        sdsfree(ele);
        rl_pushint(L, moved);
        return 1;
    }

    moved = setTypeRemove(src->sobj, ele);
    if (!moved) { sdsfree(ele); rl_pushint(L, 0); return 1; }

    if (setTypeSize(src->sobj) == 0) rl_set_reset(src);
    if (dst->sobj == NULL) dst->sobj = setTypeCreate(ele, 1);
    setTypeAdd(dst->sobj, ele);
    sdsfree(ele);
    rl_pushint(L, 1);
    return 1;
}

/* ---- SPOP --------------------------------------------------------------- */

/* t_set.c:836's threshold for the "take random elements out" strategy. */
#define RL_SPOP_MOVE_STRATEGY_MUL 5

/* SPOP key [count]                                      (t_set.c:1065, :838)
 *
 * Without COUNT: one random member, nil when there is nothing to pop. With COUNT:
 * the array spopWithCountCommand() builds, case for case, and getPositiveLong-
 * FromObjectOrReply()'s single text for a bad count.
 *
 * rl: upstream's listpack fast paths (lpNextRandom + lpBatchDelete, one traversal)
 * are not reproduced -- every draw goes through setTypeRandomElement() and every
 * removal through setTypeRemoveAux(), which is that function's own generic branch.
 * Same elements, same distribution, O(count * size) instead of O(count) on a
 * listpack. SPOP's reply is a sample of a set Redis promises no order for, and the
 * state left behind is what tools/redis-diff compares. */
static int
lspop(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_SET);
    robj *set = o->sobj;
    int top = lua_gettop(L);

    if (top > 2) return rl_error(L, "-ERR syntax error");

    if (top == 1) {
        char *str;
        size_t len = 0;
        int64_t llele = 0;
        int encoding;

        if (set == NULL) { rl_pushnilreply(L); return 1; }
        encoding = setTypeRandomElement(set, &str, &len, &llele);
        rl_push_setele(L, str, len, llele);
        setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
        if (setTypeSize(set) == 0) rl_set_reset(o);
        return 1;
    }

    {
        long lcount = rl_poslong_msg(L, 2, "value is out of range, must be positive");
        unsigned long count = (unsigned long)lcount, size;

        /* A missing key answers the empty set reply (shared.emptyset), and so does
         * count == 0. */
        if (set == NULL || count == 0) { lua_createtable(L, 0, 0); return 1; }

        size = setTypeSize(set);

        /* CASE 1: the count reaches the size, so the whole set goes out and the key
         * is deleted -- which is what leaving no payload means here. */
        if (count >= size) {
            lua_createtable(L, (int)size, 0);
            rl_set_push_all(L, set, lua_gettop(L));
            rl_set_reset(o);
            return 1;
        }

        lua_createtable(L, (int)count, 0);

        /* CASE 2: few enough to take out one at a time. */
        if ((size - count) * RL_SPOP_MOVE_STRATEGY_MUL > count) {
            unsigned long i;

            for (i = 0; i < count; i++) {
                char *str;
                size_t len = 0;
                int64_t llele = 0;
                int encoding = setTypeRandomElement(set, &str, &len, &llele);

                rl_push_setele(L, str, len, llele);
                lua_rawseti(L, -2, (int)i + 1);
                setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
            }
            return 1;
        }

        /* CASE 3: the count is close to the size, so draw the elements that STAY
         * into a new payload and hand over whatever the old one still holds. */
        {
            robj *newset = NULL;
            unsigned long left = size - count, tab = lua_gettop(L), n = 0;

            while (left--) {
                char *str;
                size_t len = 0;
                int64_t llele = 0;
                int encoding = setTypeRandomElement(set, &str, &len, &llele);

                if (newset == NULL)
                    newset = str ? createSetListpackObject() : createIntsetObject();
                setTypeAddAux(newset, str, len, llele, encoding == OBJ_ENCODING_HT);
                setTypeRemoveAux(set, str, len, llele, encoding == OBJ_ENCODING_HT);
            }

            n = rl_set_push_all(L, set, (int)tab);
            serverAssert(n == count);       /* the reply is exactly `count` members */
            rl_free_set(o->sobj);
            o->sobj = newset;               /* dbReplaceValue()'s equivalent */
            return 1;
        }
    }
}

/* ---- SRANDMEMBER -------------------------------------------------------- */

/* t_set.c:1129 and :1134: the "take everything and drop the rest" threshold, and
 * the batch size of the repeated-sample loop so a huge count cannot queue unlimited
 * samples. */
#define RL_SRAND_SUB_STRATEGY_MUL 3
#define RL_SRAND_RANDOM_SAMPLE_LIMIT 1000

/* SRANDMEMBER key [count]                               (t_set.c:1335, :1130)
 *
 * RESP2 maps to: no count -> one member (nil when there is nothing to read), count
 * -> an array, and a negative count samples the same element more than once. Every
 * case of srandmemberWithCountCommand() is transcribed, including its listpack
 * branches (lpRandomEntries for the repeated sample, lpNextRandom for the unique
 * non-random one), so which members come back is the same distribution a server
 * draws from. */
static int
lsrandmember(lua_State *L) {
    robj *set = rl_set(L, 1);
    int top = lua_gettop(L);
    unsigned long size, count, i;
    long l;
    int uniq = 1;

    if (top > 2) return rl_error(L, "-ERR syntax error");

    if (top == 1) {
        char *str;
        size_t len = 0;
        int64_t llele = 0;

        if (set == NULL) { rl_pushnilreply(L); return 1; }
        setTypeRandomElement(set, &str, &len, &llele);
        rl_push_setele(L, str, len, llele);
        return 1;
    }

    l = rl_rangelong(L, 2, -LONG_MAX, LONG_MAX);
    if (l >= 0) count = (unsigned long)l;
    else { count = (unsigned long)(-l); uniq = 0; }

    /* A missing key answers the empty array, count 0 too. */
    if (set == NULL || count == 0) { lua_createtable(L, 0, 0); return 1; }
    size = setTypeSize(set);
    if (size == 0) { lua_createtable(L, 0, 0); return 1; }

    /* CASE 1: sampling with replacement, or one element. */
    if (!uniq || count == 1) {
        lua_createtable(L, (int)count, 0);

        if (set->encoding == OBJ_ENCODING_LISTPACK && count > 1) {
            unsigned long limit = count > RL_SRAND_RANDOM_SAMPLE_LIMIT ?
                                  RL_SRAND_RANDOM_SAMPLE_LIMIT : count;
            listpackEntry *entries = zmalloc(sizeof(listpackEntry) * limit);
            unsigned long remaining = count, idx = 0;

            while (remaining) {
                unsigned long sample = remaining > limit ? limit : remaining;

                remaining -= sample;
                lpRandomEntries(set->ptr, (unsigned int)sample, entries);
                for (i = 0; i < sample; i++) {
                    rl_push_lpentry(L, &entries[i]);
                    lua_rawseti(L, -2, (int)++idx);
                }
            }
            zfree(entries);
            return 1;
        }

        for (i = 0; i < count; i++) {
            char *str;
            size_t len = 0;
            int64_t llele = 0;

            setTypeRandomElement(set, &str, &len, &llele);
            rl_push_setele(L, str, len, llele);
            lua_rawseti(L, -2, (int)i + 1);
        }
        return 1;
    }

    /* CASE 2: at least as many asked for as the set holds -> the whole set. */
    if (count >= size) {
        lua_createtable(L, (int)size, 0);
        rl_set_push_all(L, set, lua_gettop(L));
        return 1;
    }

    /* CASE 2.5: listpack, unique elements, in non-random order. */
    if (set->encoding == OBJ_ENCODING_LISTPACK) {
        unsigned char *lp = set->ptr;
        unsigned char *p = lpFirst(lp);
        unsigned int index = 0, want = (unsigned int)count;

        lua_createtable(L, (int)count, 0);
        while (want) {
            unsigned int ulen = 0;
            long long llval = 0;
            char *str;

            p = lpNextRandom(lp, p, &index, want--, 1);
            str = (char *)lpGetValue(p, &ulen, &llval);
            rl_push_setele(L, str, (size_t)ulen, (int64_t)llval);
            lua_rawseti(L, -2, (int)(count - want));
            p = lpNext(lp, p);
            index++;
        }
        return 1;
    }

    /* CASE 3 and CASE 4 need the staging dictionary: sdsReplyDictType, which frees
     * nothing because upstream hands its keys straight to the reply, reached through
     * the shim's accessor because no data symbol crosses the rlcore boundary. */
    {
        dict *d = dictCreate(redislib_sds_reply_dict_type());

        if (count * RL_SRAND_SUB_STRATEGY_MUL > size) {
            /* CASE 3: copy everything in, drop random ones until `count` are left. */
            setTypeIterator si;

            dictExpand(d, size);
            setTypeInitIterator(&si, set);
            for (;;) {
                char *str;
                size_t len = 0;
                int64_t llele = 0;
                sds sdsele;

                if (setTypeNext(&si, &str, &len, &llele) == -1) break;
                sdsele = str ? sdsnewlen(str, len) : sdsfromlonglong(llele);
                serverAssert(dictAdd(d, sdsele, NULL) == DICT_OK);
            }
            setTypeResetIterator(&si);
            serverAssert(dictSize(d) == size);

            while (size > count) {
                dictEntry *de = dictGetFairRandomKey(d);

                dictUnlink(d, dictGetKey(de));
                sdsfree(dictGetKey(de));
                dictFreeUnlinkedEntry(d, de);
                size--;
            }
        } else {
            /* CASE 4: the set is big compared to the count, so sample and keep the
             * ones not seen yet. */
            unsigned long added = 0;

            dictExpand(d, count);
            while (added < count) {
                char *str;
                size_t len = 0;
                int64_t llele = 0;
                sds sdsele;

                setTypeRandomElement(set, &str, &len, &llele);
                sdsele = str ? sdsnewlen(str, len) : sdsfromlonglong(llele);
                if (dictAdd(d, sdsele, NULL) == DICT_OK) added++;
                else sdsfree(sdsele);
            }
        }

        {
            dictIterator di;
            dictEntry *de;
            unsigned long idx = 0;

            lua_createtable(L, (int)count, 0);
            dictInitIterator(&di, d);
            while ((de = dictNext(&di)) != NULL) {
                /* addReplyBulkSds() takes ownership upstream; the bytes are copied
                 * into Lua here and the key released, because this dict frees none. */
                sds key = dictGetKey(de);

                lua_pushlstring(L, key, sdslen(key));
                sdsfree(key);
                lua_rawseti(L, -2, (int)++idx);
            }
            dictResetIterator(&di);
        }
        dictRelease(d);
    }
    return 1;
}

/* ---- SINTER / SUNION / SDIFF and the CARDINALITY forms ------------------ */

#define RL_SET_OP_UNION 0
#define RL_SET_OP_DIFF  1

/* t_set.c's file-local `setopsrc`, minus the memory-tracking half: this port's
 * `server.memory_tracking_enabled` is 0, so its `oldsize` field is read by nothing
 * and updateSlotAllocSize() has no counterpart without a keyspace (NOTICE 6). */
typedef struct {
    robj *set;
} rl_setsrc;

/* t_set.c:1371 and :1380, verbatim apart from the field names. */
static int
rl_cmp_by_card(const void *s1, const void *s2) {
    robj *o1 = ((const rl_setsrc*)s1)->set, *o2 = ((const rl_setsrc*)s2)->set;

    if (setTypeSize(o1) > setTypeSize(o2)) return 1;
    if (setTypeSize(o1) < setTypeSize(o2)) return -1;
    return 0;
}

/* "This is used by SDIFF and in this case we can receive NULL that should be
 * handled as empty sets." */
static int
rl_cmp_by_rev_card(const void *s1, const void *s2) {
    robj *o1 = ((const rl_setsrc*)s1)->set, *o2 = ((const rl_setsrc*)s2)->set;
    unsigned long first = o1 ? setTypeSize(o1) : 0;
    unsigned long second = o2 ? setTypeSize(o2) : 0;

    if (first < second) return 1;
    if (first > second) return -1;
    return 0;
}

/* Redis' `numkeys key [key ...]` collapses to one table of handles (spec 3.1), so
 * the call shapes are
 *
 *   set.sunion({a,b})          set.sintercard({a,b}, "limit", 3)
 *
 * The two failures a server answers before the command body are produced here in
 * upstream's own order: the operand list that is not there at all is this port's
 * arity line (NOTICE 7), and an empty one is "too few arguments" for
 * SUNION/SINTER/SDIFF (arity -2) and `numkeys == 0` for the CARDINALITY family,
 * whose text t_set.c attaches to that very check. */
static long
rl_setop_count(lua_State *L, int keyidx, const char *cmdname, int cardinality_only) {
    long setnum;

    rl_checklist(L, keyidx, cmdname);
    setnum = (long)lua_objlen(L, keyidx);
    if (setnum < 1) {
        if (cardinality_only)
            rl_error(L, "numkeys should be greater than 0");
        rl_error(L, "wrong number of arguments for '%s' command", cmdname);
    }
    return setnum;
}

/* What may follow the operand list: nothing at all for SUNION / SINTER / SDIFF, and
 * `[LIMIT limit]` for the three CARDINALITY forms.
 *
 * A server's arguments after `numkeys` are *keys*, so there is no word for it to
 * refuse there; here the list already is the whole key argument, so a word that the
 * command does not take is refused with Redis' own syntax-error text -- the same
 * rule facade/sortedset.c applies to its WEIGHTS / AGGREGATE / WITHSCORES tail.
 * Note this happens before the operands are looked at, which is the order
 * sinterCardCommand() uses too (it parses its options, then calls
 * sinterGenericCommand()). */
static unsigned long
rl_setop_options(lua_State *L, int keyidx, int cardinality_only) {
    long top = lua_gettop(L), j;
    unsigned long limit = 0;

    for (j = keyidx + 1; j <= top;) {
        /* Arguments still to read, this option word included -- the same counting
         * upstream does with `moreargs = (c->argc - 1) - j`. */
        long remaining = top - j + 1;

        if (lua_type(L, (int)j) != LUA_TSTRING) rl_error(L, "-ERR syntax error");
        if (cardinality_only && remaining >= 2 &&
            !strcasecmp(lua_tostring(L, (int)j), "limit")) {
            j++;
            /* getPositiveLongFromObjectOrReply() with the caller's own message, which
             * is also the answer to a LIMIT that is not a number at all. */
            limit = (unsigned long)rl_poslong_msg(L, (int)j, "LIMIT can't be negative");
            j++;
        } else {
            rl_error(L, "-ERR syntax error");
        }
    }
    return limit;
}

/* Read the operand table into the array the algorithms want. Every operand kind is
 * checked in a first pass, so a WRONGTYPE raise can never leave the array behind
 * (NOTICE 6's leak rule). */
static rl_setsrc *
rl_setop_srcs(lua_State *L, int keyidx, long setnum) {
    rl_setsrc *sets;
    long i;

    for (i = 1; i <= setnum; i++) {
        lua_rawgeti(L, keyidx, (lua_Integer)i);
        rl_check(L, -1, RL_SET);
        lua_pop(L, 1);
    }

    sets = ztrycalloc(sizeof(rl_setsrc) * (size_t)setnum);
    if (sets == NULL) rl_error(L, "%s", setop_oom);
    for (i = 0; i < setnum; i++) {
        rl_object *o;

        lua_rawgeti(L, keyidx, (lua_Integer)(i + 1));
        o = rl_check(L, -1, RL_SET);
        lua_pop(L, 1);
        sets[i].set = o->sobj;
    }
    return sets;
}

/* sinterGenericCommand() -- t_set.c:1398, without the dstkey (SINTERSTORE) and
 * memory-tracking branches. 'cardinality_only' is SINTERCARD, and `limit` stops the
 * search as soon as it has that many common members. */
static int
lsinter_generic(lua_State *L, int cardinality_only, const char *cmdname) {
    rl_setsrc *sets;
    setTypeIterator si;
    long setnum, j;
    unsigned long limit = 0, cardinality = 0, empty = 0;
    char *str;
    size_t len = 0;
    int64_t intobj = 0;
    int encoding, tab = 0;

    setnum = rl_setop_count(L, 1, cmdname, cardinality_only);
    limit = rl_setop_options(L, 1, cardinality_only);
    sets = rl_setop_srcs(L, 1, setnum);

    for (j = 0; j < setnum; j++)
        if (sets[j].set == NULL) empty++;

    /* "Set intersection with an empty set always results in an empty set." */
    if (empty > 0) {
        zfree(sets);
        if (cardinality_only) rl_pushint(L, 0);
        else lua_createtable(L, 0, 0);
        return 1;
    }

    /* Sort sets from the smallest to largest, this improves the algorithm. */
    qsort(sets, (size_t)setnum, sizeof(rl_setsrc), rl_cmp_by_card);

    if (!cardinality_only) {
        lua_createtable(L, (int)setTypeSize(sets[0].set), 0);
        tab = lua_gettop(L);
    }

    /* Iterate the elements of the first (smallest) set and test each against all
     * the others; a member of the intersection is in every one of them. */
    setTypeInitIterator(&si, sets[0].set);
    while ((encoding = setTypeNext(&si, &str, &len, &intobj)) != -1) {
        for (j = 1; j < setnum; j++) {
            if (sets[j].set == sets[0].set) continue;
            if (!setTypeIsMemberAux(sets[j].set, str, len, intobj,
                                    encoding == OBJ_ENCODING_HT))
                break;
        }
        if (j == setnum) {
            if (cardinality_only) {
                cardinality++;
                if (limit && cardinality >= limit) break;
            } else {
                rl_push_setele(L, str, len, intobj);
                lua_rawseti(L, tab, (int)++cardinality);
            }
        }
    }
    setTypeResetIterator(&si);
    zfree(sets);

    if (cardinality_only) rl_pushint(L, (long long)cardinality);
    else lua_pushvalue(L, tab);
    return 1;
}

/* sunionDiffGenericCommand() -- t_set.c:1650, without the dstkey (SUNIONSTORE /
 * SDIFFSTORE) and `approx` (SUNIONCARD's HLL estimate) branches.
 *
 * rl: `approx` is not exposed. The word is accepted by no command of this facade,
 * so `set.sunioncard({a,b}, "approx")` answers "-ERR syntax error" while a server
 * answers an HLL-based estimate; NOTICE 6 records it, and the exact path below is
 * the one Redis takes without the word. */
static int
lsuniondiff_generic(lua_State *L, int op, int cardinality_only,
                    const char *cmdname) {
    rl_setsrc *sets;
    setTypeIterator si;
    robj *dstset;
    long setnum, j;
    unsigned long limit = 0;
    int dstset_encoding = OBJ_ENCODING_INTSET;
    int sameset = 0, diff_algo = 1, early_exit = 0;
    long cardinality = 0;
    char *str;
    size_t len = 0;
    int64_t llval = 0;
    int encoding, tab = 0;

    setnum = rl_setop_count(L, 1, cmdname, cardinality_only);
    limit = rl_setop_options(L, 1, cardinality_only);
    sets = rl_setop_srcs(L, 1, setnum);

    /* Which encoding the result object should start out as: an all-intset input
     * stays intset, anything else goes straight to a hashtable, because building a
     * listpack or intset and converting it afterwards is the slow way round. */
    for (j = 0; j < setnum; j++) {
        robj *o = sets[j].set;

        if (o == NULL) continue;        /* a missing key is an empty set */
        if (dstset_encoding == OBJ_ENCODING_INTSET &&
            (o->encoding == OBJ_ENCODING_LISTPACK || o->encoding == OBJ_ENCODING_HT))
            dstset_encoding = OBJ_ENCODING_HT;
        if (j > 0 && sets[0].set == sets[j].set) sameset = 1;
    }

    /* Select what DIFF algorithm to use: 1 is O(N*M) over the first set and the
     * number of sets, 2 is O(total elements). Give algorithm 1 the constant-factor
     * advantage upstream gives it. */
    if (op == RL_SET_OP_DIFF && sets[0].set && !sameset) {
        long long algo_one_work = 0, algo_two_work = 0;

        for (j = 0; j < setnum; j++) {
            if (sets[j].set == NULL) continue;
            algo_one_work += (long long)setTypeSize(sets[0].set);
            algo_two_work += (long long)setTypeSize(sets[j].set);
        }
        algo_one_work /= 2;
        diff_algo = (algo_one_work <= algo_two_work) ? 1 : 2;

        if (diff_algo == 1 && setnum > 1) {
            /* With algorithm 1 it is better to order the sets to subtract by
             * decreasing size, so that duplicates are found ASAP. */
            qsort(sets + 1, (size_t)(setnum - 1), sizeof(rl_setsrc),
                  rl_cmp_by_rev_card);
        }
    }

    dstset = dstset_encoding == OBJ_ENCODING_INTSET ? createIntsetObject()
                                                    : createSetObject();

    if (op == RL_SET_OP_UNION) {
        /* Union is trivial: add every element of every set. */
        for (j = 0; j < setnum && !early_exit; j++) {
            if (sets[j].set == NULL) continue;

            setTypeInitIterator(&si, sets[j].set);
            while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1) {
                cardinality += setTypeAddAux(dstset, str, len, llval,
                                             encoding == OBJ_ENCODING_HT);
                if (cardinality_only && limit > 0 && (unsigned long)cardinality >= limit) {
                    early_exit = 1;
                    break;
                }
            }
            setTypeResetIterator(&si);
        }
    } else if (op == RL_SET_OP_DIFF && sameset) {
        /* One of the sets to subtract is the first one itself: the result is empty. */
    } else if (op == RL_SET_OP_DIFF && sets[0].set && diff_algo == 1) {
        /* Algorithm 1: iterate the first set and add an element only when no other
         * set holds it. */
        setTypeInitIterator(&si, sets[0].set);
        while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1) {
            for (j = 1; j < setnum; j++) {
                if (sets[j].set == NULL) continue;    /* no key is an empty set */
                if (sets[j].set == sets[0].set) break; /* same set! */
                if (setTypeIsMemberAux(sets[j].set, str, len, llval,
                                       encoding == OBJ_ENCODING_HT))
                    break;
            }
            if (j == setnum) {
                if (cardinality_only) {
                    cardinality++;
                    if (limit > 0 && (unsigned long)cardinality >= limit) break;
                } else {
                    cardinality += setTypeAddAux(dstset, str, len, llval,
                                                 encoding == OBJ_ENCODING_HT);
                }
            }
        }
        setTypeResetIterator(&si);
    } else if (op == RL_SET_OP_DIFF && sets[0].set && diff_algo == 2 &&
               cardinality_only && limit > 0) {
        /* Algorithm 2b: collect what is to be subtracted into one auxiliary set and
         * test the first set's elements against it, so the LIMIT can stop early. */
        for (j = 1; j < setnum; j++) {
            if (sets[j].set == NULL) continue;

            setTypeInitIterator(&si, sets[j].set);
            while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1)
                setTypeAddAux(dstset, str, len, llval, encoding == OBJ_ENCODING_HT);
            setTypeResetIterator(&si);
        }

        setTypeInitIterator(&si, sets[0].set);
        while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1) {
            if (!setTypeIsMemberAux(dstset, str, len, llval,
                                    encoding == OBJ_ENCODING_HT)) {
                cardinality++;
                if ((unsigned long)cardinality >= limit) break;
            }
        }
        setTypeResetIterator(&si);
    } else if (op == RL_SET_OP_DIFF && sets[0].set && diff_algo == 2) {
        /* Algorithm 2: add the first set, then remove every later set's members. */
        for (j = 0; j < setnum; j++) {
            if (sets[j].set == NULL) continue;

            setTypeInitIterator(&si, sets[j].set);
            while ((encoding = setTypeNext(&si, &str, &len, &llval)) != -1) {
                if (j == 0)
                    cardinality += setTypeAddAux(dstset, str, len, llval,
                                                 encoding == OBJ_ENCODING_HT);
                else
                    cardinality -= setTypeRemoveAux(dstset, str, len, llval,
                                                    encoding == OBJ_ENCODING_HT);
            }
            setTypeResetIterator(&si);

            /* Any further removal can only keep it empty. */
            if (cardinality == 0) break;
        }
    }

    if (cardinality_only) {
        rl_pushint(L, cardinality);
    } else {
        lua_createtable(L, (int)setTypeSize(dstset), 0);
        tab = lua_gettop(L);
        rl_set_push_all(L, dstset, tab);
        lua_pushvalue(L, tab);
    }
    rl_free_set(dstset);
    zfree(sets);
    return 1;
}

static int
lsunion(lua_State *L) {
    return lsuniondiff_generic(L, RL_SET_OP_UNION, 0, "sunion");
}

static int
lsdiff(lua_State *L) {
    return lsuniondiff_generic(L, RL_SET_OP_DIFF, 0, "sdiff");
}

static int
lsinter(lua_State *L) {
    return lsinter_generic(L, 0, "sinter");
}

static int
lsintercard(lua_State *L) {
    return lsinter_generic(L, 1, "sintercard");
}

static int
lsunioncard(lua_State *L) {
    return lsuniondiff_generic(L, RL_SET_OP_UNION, 1, "sunioncard");
}

static int
lsdiffcard(lua_State *L) {
    return lsuniondiff_generic(L, RL_SET_OP_DIFF, 1, "sdiffcard");
}

/* ---- registration ------------------------------------------------------- */

static const luaL_Reg set_methods[] = {
    {"sadd", lsadd},
    {"srem", lsrem},
    {"scard", lscard},
    {"smembers", lsmembers},
    {"sismember", lsismember},
    {"smismember", lsmismember},
    {"smove", lsmove},
    {"spop", lspop},
    {"srandmember", lsrandmember},
    /* no sunion / sinter / sdiff / *card here: their operand list is argument 1, so
     * a receiver would have nothing to be -- module functions only, exactly like
     * sortedset.zmpop (NOTICE 6). */
    {"dump", lset_dump},
    {"free", lset_free},
    {NULL, NULL},
};

static const luaL_Reg set_module_methods[] = {
    {"new", lset_new},
    {"load", lset_load},
    {"smove", lsmove},
    {"sunion", lsunion},
    {"sinter", lsinter},
    {"sdiff", lsdiff},
    {"sunioncard", lsunioncard},
    {"sintercard", lsintercard},
    {"sdiffcard", lsdiffcard},
    {NULL, NULL},
};

/* LuaJIT's searcher uses the full module name with dots converted to
 * underscores (same rule as NetBull's luaopen_skiplist_c for "skiplist.c"). */
int luaopen_redis_set(lua_State *L) {
    rl_register_class(L, RL_SET, set_methods, NULL);
    lua_pop(L, 1);

    lua_newtable(L);
    luaL_setfuncs(L, set_module_methods, 0);
    return 1;
}
