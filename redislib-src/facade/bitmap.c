/* bitmap.c -- the redis.bitmap facade: SETBIT GETBIT BITCOUNT BITPOS BITOP
 * BITFIELD BITFIELD_RO over an in-memory Redis string value (sds).
 *
 * Faithful transcription of Redis 8.10.0 src/bitops.c with the client /
 * keyspace layer removed: argument parsing becomes rl_* helpers carrying the
 * same Redis error texts, addReply* becomes the rl_push* mapping (see
 * rl_common.h), the string object is the rl_object's sds. Kernel primitives
 * (popcount / bitpos / bitfield get-set-overflow) are called, not rewritten.
 * "rl:" comments mark deliberate model deviations (no keyspace). */

#include <string.h>
#include <strings.h>
#include <stdlib.h>
#include <stdio.h>
#include <limits.h>

#include "rl_common.h"
#include "util.h"

/* rl: server.proto_max_bulk_len default (512MB): Redis 8's bit-offset cap. */
#define RL_PROTO_MAX_BULK_LEN (512LL*1024*1024)

/* Kernel entry points (redislib-src/kernel/bitops.c). */
long long redisPopcount(void *s, long count);
long long redisBitpos(void *s, unsigned long count, int bit);
void setUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, uint64_t value);
void setSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, int64_t value);
uint64_t getUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits);
int64_t getSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits);
int checkUnsignedBitfieldOverflow(uint64_t value, int64_t incr, uint64_t bits, int owtype, uint64_t *limit);
int checkSignedBitfieldOverflow(int64_t value, int64_t incr, uint64_t bits, int owtype, int64_t *limit);

#define BITOP_AND   0
#define BITOP_OR    1
#define BITOP_XOR   2
#define BITOP_NOT   3
#define BITOP_DIFF  4  /* X & ~(A1|..|An) */
#define BITOP_DIFF1 5  /* ~X & (A1|..|An) */
#define BITOP_ANDOR 6  /* X & (A1|..|An) */
#define BITOP_ONE   7  /* set exactly once */

#define BFOVERFLOW_WRAP 0
#define BFOVERFLOW_SAT  1
#define BFOVERFLOW_FAIL 2

#define BITFIELDOP_GET 0
#define BITFIELDOP_SET 1
#define BITFIELDOP_INCRBY 2

#define RL_POPCOUNT(p,n) redisPopcount((void*)(p),(long)(n))

/* LuaJIT's lauxlib.h does not mark rl_error() noreturn (upstream Lua 5.2+
 * does), so the compiler cannot see that the argument parsing below has no
 * fall-through path. The range/opcode variables get sentinel initialisers:
 * every reachable path assigns them before use. */
#define RL_UNSET (-1)

/* ---- argument helpers --------------------------------------------------- */

/* getLongLongFromObjectOrReply with err text when NULL err given. */
static long long
rl_checklonglong(lua_State *L, int idx, const char *err) {
    if (lua_isnumber(L, idx)) {
        double d = lua_tonumber(L, idx);
        long long v = (long long)d;
        if ((double)v == d) return v;
        rl_error(L, "%s", err);
    }
    if (lua_isstring(L, idx)) {
        size_t l;
        const char *s = lua_tolstring(L, idx, &l);
        long long v;
        if (string2ll(s, l, &v)) return v;
    }
    rl_error(L, "%s", err);
    return 0; /* unreachable */
}

/* getBitOffsetFromArgument(c,o,&off,hash,bits) without the client. */
static uint64_t
rl_bitoffset(lua_State *L, int idx, int hash, int bits) {
    const char *err = "bit offset is not an integer or out of range";
    long long loffset;

    if (lua_isstring(L, idx)) {
        size_t plen;
        const char *p = lua_tolstring(L, idx, &plen);
        int usehash = (p[0] == '#' && hash && bits > 0);
        if (string2ll(p+usehash, plen-usehash, &loffset) == 0 || loffset < 0)
            rl_error(L, "%s", err);
        if (usehash) {
            if (loffset > LLONG_MAX / bits) rl_error(L, "%s", err);
            loffset *= bits;
        }
    } else if (lua_isnumber(L, idx)) {
        double d = lua_tonumber(L, idx);
        loffset = (long long)d;
        if ((double)loffset != d || loffset < 0) rl_error(L, "%s", err);
    } else {
        rl_error(L, "%s", err);
        return 0; /* unreachable */
    }
    if ((loffset >> 3) >= RL_PROTO_MAX_BULK_LEN) rl_error(L, "%s", err);
    return (uint64_t)loffset;
}

/* unit token ("byte"/"bit") at idx; sets *isbit; Redis syntax error else. */
static void
rl_unit_arg(lua_State *L, int idx, int *isbit) {
    size_t l;
    const char *u = lua_tolstring(L, idx, &l);
    if (u && !strcasecmp(u, "bit")) *isbit = 1;
    else if (u && !strcasecmp(u, "byte")) *isbit = 0;
    else rl_error(L, "-ERR syntax error");
}

/* ---- SETBIT / GETBIT ---------------------------------------------------- */

static int
lbitmap_setbit(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_BITMAP);
    const char *err = "bit is not an integer or out of range";
    uint64_t bitoffset = rl_bitoffset(L, 2, 0, 0);
    long long on = rl_checklonglong(L, 3, err);
    size_t byte, bit;
    int byteval, bitval;

    if (on & ~1) rl_error(L, "%s", err);

    byte = bitoffset >> 3;
    if (sdslen(o->str) < byte + 1)          /* lookupStringForBitCommand */
        o->str = sdsgrowzero(o->str, byte + 1);

    byteval = ((uint8_t*)o->str)[byte];
    bit = 7 - (bitoffset & 0x7);
    bitval = byteval & (1 << bit);

    byteval &= ~(1 << bit);
    byteval |= ((on & 0x1) << bit);
    ((uint8_t*)o->str)[byte] = byteval;

    rl_pushint(L, bitval ? 1 : 0);          /* the ORIGINAL bit */
    return 1;
}

static int
lbitmap_getbit(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_BITMAP);
    uint64_t bitoffset = rl_bitoffset(L, 2, 0, 0);
    size_t byte = bitoffset >> 3;
    size_t bitval = 0;

    if (byte < sdslen(o->str))
        bitval = ((uint8_t*)o->str)[byte] & ((size_t)1 << (7 - (bitoffset & 0x7)));
    rl_pushint(L, bitval ? 1 : 0);
    return 1;
}

/* ---- BITCOUNT key [start end [BYTE|BIT]] -------------------------------- */

static int
lbitmap_bitcount(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_BITMAP);
    long long start = 0, end = RL_UNSET;
    long long strlen_ = (long long)sdslen(o->str);
    unsigned char *p = (unsigned char*)o->str;
    int isbit = 0;
    int argc = lua_gettop(L) - 1;
    unsigned char first_byte_neg_mask = 0, last_byte_neg_mask = 0;

    if (argc == 2 || argc == 3) {
        long long totlen = strlen_;
        start = rl_checklonglong(L, 2, "value is not an integer or out of range");
        end   = rl_checklonglong(L, 3, "value is not an integer or out of range");
        if (argc == 3) rl_unit_arg(L, 4, &isbit);

        if (start < 0 && end < 0 && start > end) { rl_pushint(L, 0); return 1; }
        if (isbit) totlen <<= 3;
        if (start < 0) start = totlen+start;
        if (end < 0) end = totlen+end;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if (end >= totlen) end = totlen-1;
        if (isbit && start <= end) {
            first_byte_neg_mask = ~((1<<(8-(start&7)))-1) & 0xFF;
            last_byte_neg_mask = (1<<(7-(end&7)))-1;
            start >>= 3;
            end >>= 3;
        }
    } else if (argc == 0) {
        start = 0;
        end = strlen_-1;
    } else {
        rl_error(L, "-ERR syntax error");
    }

    if (start > end) { rl_pushint(L, 0); return 1; }
    {
        long bytes = (long)(end-start+1);
        long long count = RL_POPCOUNT(p+start, bytes);
        if (first_byte_neg_mask != 0 || last_byte_neg_mask != 0) {
            unsigned char firstlast[2] = {0, 0};
            if (first_byte_neg_mask != 0) firstlast[0] = p[start] & first_byte_neg_mask;
            if (last_byte_neg_mask != 0) firstlast[1] = p[end] & last_byte_neg_mask;
            count -= RL_POPCOUNT(firstlast, 2);
        }
        rl_pushint(L, count);
    }
    return 1;
}

/* ---- BITPOS key bit [start [end [BYTE|BIT]]] ---------------------------- */

static int
lbitmap_bitpos(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_BITMAP);
    long long bit = rl_checklonglong(L, 2, "bit is not an integer or out of range");
    long long start = 0, end = RL_UNSET;
    long long strlen_ = (long long)sdslen(o->str);
    unsigned char *p = (unsigned char*)o->str;
    int isbit = 0, end_given = 0;
    int argc = lua_gettop(L) - 2;
    unsigned char first_byte_neg_mask = 0, last_byte_neg_mask = 0;

    if (bit != 0 && bit != 1)
        rl_error(L, "bit is not an integer or out of range");

    if (argc >= 1) {
        long long totlen = strlen_;
        start = rl_checklonglong(L, 3, "value is not an integer or out of range");
        if (argc >= 2) {
            end = rl_checklonglong(L, 4, "value is not an integer or out of range");
            end_given = 1;
            if (argc == 3) rl_unit_arg(L, 5, &isbit);
        } else {
            end = -1;
        }
        if (isbit) totlen <<= 3;
        if (start < 0) start = totlen+start;
        if (end < 0) end = totlen+end;
        if (start < 0) start = 0;
        if (end < 0) end = 0;
        if (end >= totlen) end = totlen-1;
        if (isbit && start <= end) {
            first_byte_neg_mask = ~((1<<(8-(start&7)))-1) & 0xFF;
            last_byte_neg_mask = (1<<(7-(end&7)))-1;
            start >>= 3;
            end >>= 3;
        }
    } else {
        end = strlen_-1;
    }

    /* rl: an empty object plays the role of the missing key, and Redis' answer for
     * a missing key is a number, never a nil: the key is "an infinite array of 0
     * bits", so the first clear bit is 0 and there is no set bit to find (bitops.c
     * looks it up as `bit ? -1 : 0`). A live server does distinguish the two -- a
     * key set to the empty string takes the empty-range answer below -- but with no
     * keyspace here there is nothing to distinguish, and the missing-key reading is
     * the one that matches an object no write has touched. */
    if (strlen_ == 0) {
        rl_pushint(L, bit ? -1 : 0);
        return 1;
    }
    /* Also for an empty range: it contains neither a 0 nor a 1 bit (bitops.c:890). */
    if (start > end) { rl_pushint(L, -1); return 1; }
    {
        long bytes = (long)(end-start+1);
        long long pos = -1;
        unsigned char tmpchar;
        if (first_byte_neg_mask) {
            if (bit) tmpchar = p[start] & ~first_byte_neg_mask;
            else tmpchar = p[start] | first_byte_neg_mask;
            if (last_byte_neg_mask && bytes == 1) {
                if (bit) tmpchar = tmpchar & ~last_byte_neg_mask;
                else tmpchar = tmpchar | last_byte_neg_mask;
            }
            pos = redisBitpos(&tmpchar, 1, (int)bit);
            if (bytes == 1 || (pos != -1 && pos != 8)) goto result;
            start++;
            bytes--;
        }
        {
            long curbytes = bytes - (last_byte_neg_mask ? 1 : 0);
            if (curbytes > 0) {
                pos = redisBitpos(p+start, (unsigned long)curbytes, (int)bit);
                if (bytes == curbytes || (pos != -1 && pos != (long long)curbytes<<3))
                    goto result;
                start += curbytes;
                bytes -= curbytes;
            }
            if (bit) tmpchar = p[end] & ~last_byte_neg_mask;
            else tmpchar = p[end] | last_byte_neg_mask;
            pos = redisBitpos(&tmpchar, 1, (int)bit);
        }
    result:
        /* bitops.c:872 -- BITPOS has no nil reply at all. A range that was given
         * explicitly and holds no clear bit answers -1, and a scan that found
         * nothing answers -1 as it is; only a found position gets the skipped
         * bytes added back. */
        if (end_given && bit == 0 && pos == ((long long)bytes<<3)) {
            rl_pushint(L, -1);
            return 1;
        }
        if (pos != -1) pos += (long long)start<<3; /* Adjust for bytes skipped. */
        rl_pushint(L, pos);
    }
    return 1;
}

/* ---- BITOP op {srcs} ---------------------------------------------------- */

static int
lbitmap_bitop(lua_State *L) {
    const char *opname = luaL_checkstring(L, 1);
    unsigned long op = RL_UNSET, j, numkeys, i;
    unsigned long maxlen = 0, minlen = 0;
    unsigned long *len;
    unsigned char **src;
    unsigned char *res = NULL;

    if      ((opname[0]=='a'||opname[0]=='A') && !strcasecmp(opname,"and"))   op = BITOP_AND;
    else if ((opname[0]=='o'||opname[0]=='O') && !strcasecmp(opname,"or"))    op = BITOP_OR;
    else if ((opname[0]=='x'||opname[0]=='X') && !strcasecmp(opname,"xor"))   op = BITOP_XOR;
    else if ((opname[0]=='n'||opname[0]=='N') && !strcasecmp(opname,"not"))   op = BITOP_NOT;
    else if ((opname[0]=='d'||opname[0]=='D') && !strcasecmp(opname,"diff"))  op = BITOP_DIFF;
    else if ((opname[0]=='d'||opname[0]=='D') && !strcasecmp(opname,"diff1")) op = BITOP_DIFF1;
    else if ((opname[0]=='a'||opname[0]=='A') && !strcasecmp(opname,"andor")) op = BITOP_ANDOR;
    else if ((opname[0]=='o'||opname[0]=='O') && !strcasecmp(opname,"one"))   op = BITOP_ONE;
    else rl_error(L, "-ERR syntax error");

    luaL_checktype(L, 2, LUA_TTABLE);
    numkeys = (unsigned long)lua_objlen(L, 2);
    if (op == BITOP_NOT && numkeys != 1)
        rl_error(L, "BITOP NOT must be called with a single source key.");
    if ((op == BITOP_DIFF || op == BITOP_DIFF1 || op == BITOP_ANDOR) && numkeys < 2) {
        char buf[128];
        sds up = sdsnew(opname);
        sdstoupper(up);
        snprintf(buf, sizeof(buf),
            "BITOP %s must be called with at least two source keys.", (char*)up);
        sdsfree(up);
        rl_error(L, "%s", buf);
    }
    if (numkeys == 0) rl_error(L, "wrong number of arguments");

    /* Every operand is type-checked before anything is allocated: rl_check()
     * raises, and a raise longjmps out of this function with no chance to free,
     * so checking after zmalloc() would leak the two arrays on every wrong-kind
     * argument. Redis cannot fail here -- its operands are keys, already in the
     * keyspace as strings. */
    for (j = 0; j < numkeys; j++) {
        lua_rawgeti(L, 2, (lua_Integer)(j+1));
        rl_check(L, -1, RL_BITMAP);
        lua_pop(L, 1);
    }

    src = zmalloc(sizeof(unsigned char*) * numkeys);
    len = zmalloc(sizeof(unsigned long) * numkeys);
    for (j = 0; j < numkeys; j++) {
        rl_object *so;
        lua_rawgeti(L, 2, (lua_Integer)(j+1));
        so = rl_check(L, -1, RL_BITMAP);
        lua_pop(L, 1);
        src[j] = (unsigned char*)so->str;
        len[j] = sdslen(so->str);
        if (len[j] > maxlen) maxlen = len[j];
        if (j == 0 || len[j] < minlen) minlen = len[j];
    }

    if (maxlen) {
        unsigned char output, byteb, disjunction, common_bits;
        res = (unsigned char*)sdsnewlen(NULL, maxlen);
        j = 0;

        if (minlen >= sizeof(unsigned long)*4) {
            unsigned long **lp = (unsigned long**)src;
            unsigned long *lres = (unsigned long*) res;
            unsigned long k = 0;

            if (op != BITOP_DIFF && op != BITOP_DIFF1 && op != BITOP_ANDOR)
                memcpy(lres, src[0], minlen);

            if (op == BITOP_AND) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] &= lp[i][k+0]; lres[1] &= lp[i][k+1];
                        lres[2] &= lp[i][k+2]; lres[3] &= lp[i][k+3];
                    }
                    k+=4; lres+=4; j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_OR) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] |= lp[i][k+0]; lres[1] |= lp[i][k+1];
                        lres[2] |= lp[i][k+2]; lres[3] |= lp[i][k+3];
                    }
                    k+=4; lres+=4; j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_XOR) {
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] ^= lp[i][k+0]; lres[1] ^= lp[i][k+1];
                        lres[2] ^= lp[i][k+2]; lres[3] ^= lp[i][k+3];
                    }
                    k+=4; lres+=4; j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_NOT) {
                while(minlen >= sizeof(unsigned long)*4) {
                    lres[0] = ~lres[0]; lres[1] = ~lres[1];
                    lres[2] = ~lres[2]; lres[3] = ~lres[3];
                    lres+=4; j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            } else if (op == BITOP_DIFF || op == BITOP_DIFF1 || op == BITOP_ANDOR) {
                size_t processed = 0;
                while(minlen >= sizeof(unsigned long)*4) {
                    for (i = 1; i < numkeys; i++) {
                        lres[0] |= lp[i][k+0]; lres[1] |= lp[i][k+1];
                        lres[2] |= lp[i][k+2]; lres[3] |= lp[i][k+3];
                    }
                    k+=4; lres+=4; j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                    processed += sizeof(unsigned long)*4;
                }
                {
                    unsigned long *ap = (unsigned long*) res;
                    unsigned long *first_key = (unsigned long*)src[0];
                    size_t i2;
                    for (i2 = 0; i2 < processed; i2 += sizeof(unsigned long)*4) {
                        switch (op) {
                        case BITOP_DIFF:
                            ap[0] = (first_key[0] & ~ap[0]);
                            ap[1] = (first_key[1] & ~ap[1]);
                            ap[2] = (first_key[2] & ~ap[2]);
                            ap[3] = (first_key[3] & ~ap[3]);
                            break;
                        case BITOP_DIFF1:
                            ap[0] = (~first_key[0] & ap[0]);
                            ap[1] = (~first_key[1] & ap[1]);
                            ap[2] = (~first_key[2] & ap[2]);
                            ap[3] = (~first_key[3] & ap[3]);
                            break;
                        default: /* ANDOR */
                            ap[0] = (first_key[0] & ap[0]);
                            ap[1] = (first_key[1] & ap[1]);
                            ap[2] = (first_key[2] & ap[2]);
                            ap[3] = (first_key[3] & ap[3]);
                            break;
                        }
                        ap += 4; first_key += 4;
                    }
                }
            } else if (op == BITOP_ONE) {
                unsigned long lcommon_bits[4];
                while(minlen >= sizeof(unsigned long)*4) {
                    memset(lcommon_bits, 0, sizeof(lcommon_bits));
                    for (i = 1; i < numkeys; i++) {
                        lcommon_bits[0] |= (lres[0] & lp[i][k+0]);
                        lcommon_bits[1] |= (lres[1] & lp[i][k+1]);
                        lcommon_bits[2] |= (lres[2] & lp[i][k+2]);
                        lcommon_bits[3] |= (lres[3] & lp[i][k+3]);
                        lres[0] ^= lp[i][k+0]; lres[1] ^= lp[i][k+1];
                        lres[2] ^= lp[i][k+2]; lres[3] ^= lp[i][k+3];
                    }
                    lres[0] &= ~lcommon_bits[0]; lres[1] &= ~lcommon_bits[1];
                    lres[2] &= ~lcommon_bits[2]; lres[3] &= ~lcommon_bits[3];
                    k+=4; lres+=4; j += sizeof(unsigned long)*4;
                    minlen -= sizeof(unsigned long)*4;
                }
            }
        }

        /* j is set to the next byte to process by the previous loop. */
        for (; j < maxlen; j++) {
            output = (len[0] <= j) ? 0 : src[0][j];
            if (op == BITOP_NOT) output = ~output;
            disjunction = 0;
            common_bits = 0;
            for (i = 1; i < numkeys; i++) {
                int skip = 0;
                byteb = (len[i] <= j) ? 0 : src[i][j];
                switch(op) {
                case BITOP_AND: output &= byteb; skip = (output == 0); break;
                case BITOP_OR:  output |= byteb; skip = (output == 0xff); break;
                case BITOP_XOR: output ^= byteb; break;
                case BITOP_DIFF:
                case BITOP_DIFF1:
                case BITOP_ANDOR:
                    disjunction |= byteb; skip = (disjunction == 0xff); break;
                case BITOP_ONE:
                    common_bits |= (output & byteb);
                    output ^= byteb;
                    output &= ~common_bits;
                    skip = (common_bits == 0xff);
                    break;
                default: break;
                }
                if (skip) break;
            }
            switch(op) {
            case BITOP_DIFF:  res[j] = (output & ~disjunction); break;
            case BITOP_DIFF1: res[j] = (~output & disjunction); break;
            case BITOP_ANDOR: res[j] = (output & disjunction); break;
            default: res[j] = output; break;
            }
        }
    }

    zfree(src);
    zfree(len);

    rl_newobject(L, RL_BITMAP);
    ((rl_object*)lua_touserdata(L, -1))->str = res ? (sds)res : sdsempty();
    return 1;
}

/* ---- BITFIELD / BITFIELD_RO --------------------------------------------- */

struct rl_bitfieldOp {
    uint64_t offset;
    int64_t i64;
    int opcode;
    int owtype;
    int bits;
    int sign;
};

static void
rl_bitfield_type(lua_State *L, int idx, int *sign, int *bits) {
    const char *err = "Invalid bitfield type. Use something like i16 u8. "
                      "Note that u64 is not supported but i64 is.";
    size_t plen;
    const char *p = lua_tolstring(L, idx, &plen);
    long long llbits;

    if (!p) rl_error(L, "%s", err);
    if (p[0] == 'i') *sign = 1;
    else if (p[0] == 'u') *sign = 0;
    else rl_error(L, "%s", err);

    if (string2ll(p+1, plen-1, &llbits) == 0 || llbits < 1 ||
        (*sign == 1 && llbits > 64) || (*sign == 0 && llbits > 63))
        rl_error(L, "%s", err);
    *bits = (int)llbits;
}

static void
bitmap_bitfield_generic(lua_State *L, int ro_cmd) {
    rl_object *obj = rl_check(L, 1, RL_BITMAP);
    int argc = lua_gettop(L);
    int j, numops = 0, readonly = 1;
    struct rl_bitfieldOp *ops = NULL;
    int owtype = BFOVERFLOW_WRAP;
    uint64_t highest_write_offset = 0;
    uint64_t bitoffset;

    for (j = 2; j <= argc; j++) {
        int remargs = argc - j;
        const char *subcmd = luaL_checkstring(L, j);
        int opcode = RL_UNSET;
        long long i64 = 0;
        int sign = 0, bits = 0;

        if (!strcasecmp(subcmd,"get") && remargs >= 2)
            opcode = BITFIELDOP_GET;
        else if (!strcasecmp(subcmd,"set") && remargs >= 3)
            opcode = BITFIELDOP_SET;
        else if (!strcasecmp(subcmd,"incrby") && remargs >= 3)
            opcode = BITFIELDOP_INCRBY;
        else if (!strcasecmp(subcmd,"overflow") && remargs >= 1) {
            const char *owtypename = luaL_checkstring(L, j+1);
            j++;
            if (!strcasecmp(owtypename,"wrap")) owtype = BFOVERFLOW_WRAP;
            else if (!strcasecmp(owtypename,"sat")) owtype = BFOVERFLOW_SAT;
            else if (!strcasecmp(owtypename,"fail")) owtype = BFOVERFLOW_FAIL;
            else { zfree(ops); rl_error(L, "Invalid OVERFLOW type specified"); }
            continue;
        } else {
            zfree(ops);
            rl_error(L, "-ERR syntax error");
        }

        rl_bitfield_type(L, j+1, &sign, &bits);
        bitoffset = rl_bitoffset(L, j+2, 1, bits);

        if (opcode != BITFIELDOP_GET) {
            readonly = 0;
            if (highest_write_offset < bitoffset + bits - 1)
                highest_write_offset = bitoffset + bits - 1;
            i64 = rl_checklonglong(L, j+3, "value is not an integer or out of range");
        }

        ops = zrealloc(ops, sizeof(*ops)*(numops+1));
        ops[numops].offset = bitoffset;
        ops[numops].i64 = i64;
        ops[numops].opcode = opcode;
        ops[numops].owtype = owtype;
        ops[numops].bits = bits;
        ops[numops].sign = sign;
        numops++;

        j += 3 - (opcode == BITFIELDOP_GET);
    }

    if (!readonly && ro_cmd) {
        zfree(ops);
        rl_error(L, "BITFIELD_RO only supports the GET subcommand");
    }

    if (!readonly) {
        /* lookupStringForBitCommand(highest_write_offset) */
        size_t byte = highest_write_offset >> 3;
        if (sdslen(obj->str) < byte + 1)
            obj->str = sdsgrowzero(obj->str, byte + 1);
    }

    lua_createtable(L, numops, 0);

    for (j = 0; j < numops; j++) {
        struct rl_bitfieldOp *thisop = ops+j;

        if (thisop->opcode == BITFIELDOP_SET || thisop->opcode == BITFIELDOP_INCRBY) {
            if (thisop->sign) {
                int64_t oldval, newval, wrapped = 0, retval;
                int overflow;
                oldval = getSignedBitfield((unsigned char*)obj->str, thisop->offset, thisop->bits);
                if (thisop->opcode == BITFIELDOP_INCRBY) {
                    overflow = checkSignedBitfieldOverflow(oldval, thisop->i64,
                                    thisop->bits, thisop->owtype, &wrapped);
                    newval = overflow ? wrapped : oldval + thisop->i64;
                    retval = newval;
                } else {
                    newval = thisop->i64;
                    overflow = checkSignedBitfieldOverflow(newval, 0,
                                    thisop->bits, thisop->owtype, &wrapped);
                    if (overflow) newval = wrapped;
                    retval = oldval;
                }
                if (!(overflow && thisop->owtype == BFOVERFLOW_FAIL)) {
                    lua_pushinteger(L, (lua_Integer)retval);
                    setSignedBitfield((unsigned char*)obj->str, thisop->offset, thisop->bits, newval);
                } else {
                    lua_pushboolean(L, 0);   /* addReplyNull -> false in array */
                }
            } else {
                uint64_t oldval, newval, retval, wrapped = 0;
                int overflow;
                oldval = getUnsignedBitfield((unsigned char*)obj->str, thisop->offset, thisop->bits);
                if (thisop->opcode == BITFIELDOP_INCRBY) {
                    newval = oldval + thisop->i64;
                    overflow = checkUnsignedBitfieldOverflow(oldval, thisop->i64,
                                    thisop->bits, thisop->owtype, &wrapped);
                    if (overflow) newval = wrapped;
                    retval = newval;
                } else {
                    newval = thisop->i64;
                    overflow = checkUnsignedBitfieldOverflow(newval, 0,
                                    thisop->bits, thisop->owtype, &wrapped);
                    if (overflow) newval = wrapped;
                    retval = oldval;
                }
                if (!(overflow && thisop->owtype == BFOVERFLOW_FAIL)) {
                    lua_pushinteger(L, (lua_Integer)retval);
                    setUnsignedBitfield((unsigned char*)obj->str, thisop->offset, thisop->bits, newval);
                } else {
                    lua_pushboolean(L, 0);
                }
            }
        } else {
            /* GET: copy up to 9 bytes into a zero-padded local buffer. */
            unsigned char buf[9];
            long strlen_ = (long)sdslen(obj->str);
            unsigned char *src = (unsigned char*)obj->str;
            int i;
            uint64_t byte = thisop->offset >> 3;
            memset(buf, 0, 9);
            for (i = 0; i < 9; i++) {
                if (src == NULL || (uint64_t)i + byte >= (uint64_t)strlen_) break;
                buf[i] = src[i + byte];
            }
            if (thisop->sign) {
                int64_t val = getSignedBitfield(buf, thisop->offset-(byte*8), thisop->bits);
                lua_pushinteger(L, (lua_Integer)val);
            } else {
                uint64_t val = getUnsignedBitfield(buf, thisop->offset-(byte*8), thisop->bits);
                lua_pushinteger(L, (lua_Integer)val);
            }
        }
        lua_rawseti(L, -2, j+1);
    }

    zfree(ops);
}

static int
lbitmap_bitfield(lua_State *L) {
    bitmap_bitfield_generic(L, 0);
    return 1;
}

static int
lbitmap_bitfield_ro(lua_State *L) {
    bitmap_bitfield_generic(L, 1);
    return 1;
}

/* ---- object lifecycle ---------------------------------------------------- */

static int
lbitmap_new(lua_State *L) {
    rl_object *o;
    rl_newobject(L, RL_BITMAP);
    o = lua_touserdata(L, -1);
    o->str = sdsempty();
    return 1;
}

static int
lbitmap_len(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_BITMAP);
    rl_pushint(L, (long long)sdslen(o->str));
    return 1;
}

static int
lbitmap_free(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_BITMAP);
    rl_free_ud(L, o);
    return 0;
}

/* ---- dump / load (spec §4, not Redis commands) --------------------------
 *
 * A bitmap IS a Redis string, so its dump is that string: byte for byte what
 * GET of such a key returns, and load is what SET does with it. Any bytes are a
 * valid string, so there is nothing to reject -- which is also why a blob from
 * a real Redis (SET k <the same bytes>) and one made here are the same thing. */
static int
lbitmap_dump(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_BITMAP);

    rl_pushvalue(L, o);
    return 1;
}

/* bitmap.load(string) => bitmap */
static int
lbitmap_load(lua_State *L) {
    rl_object *o;
    size_t len;
    const char *buf = rl_checkblob(L, 1, &len);

    rl_newobject(L, RL_BITMAP);
    o = (rl_object*)lua_touserdata(L, -1);
    o->str = sdsnewlen(buf, len);
    return 1;
}

/* __tostring: the raw Redis string value (may contain NUL bytes). */
static int
lbitmap_tostring(lua_State *L) {
    rl_object *o = rl_check(L, 1, RL_BITMAP);
    rl_pushvalue(L, o);
    return 1;
}

static const luaL_Reg bitmap_methods[] = {
    {"setbit", lbitmap_setbit},
    {"getbit", lbitmap_getbit},
    {"bitcount", lbitmap_bitcount},
    {"bitpos", lbitmap_bitpos},
    {"bitfield", lbitmap_bitfield},
    {"bitfield_ro", lbitmap_bitfield_ro},
    {"len", lbitmap_len},
    {"dump", lbitmap_dump},
    {"free", lbitmap_free},
    {NULL, NULL},
};

static const luaL_Reg bitmap_module_methods[] = {
    {"new", lbitmap_new},
    {"load", lbitmap_load},
    {"bitop", lbitmap_bitop},
    {NULL, NULL},
};

/* LuaJIT's searcher uses the full module name with dots converted to
 * underscores (same rule as NetBull's luaopen_skiplist_c for "skiplist.c"). */
int luaopen_redis_bitmap(lua_State *L) {
    rl_register_class(L, RL_BITMAP, bitmap_methods, NULL);
    lua_pushcfunction(L, lbitmap_tostring);
    lua_setfield(L, -2, "__tostring");
    lua_pop(L, 1);                       /* metatable is registered by name */

    lua_newtable(L);
    luaL_setfuncs(L, bitmap_module_methods, 0);
    return 1;
}
