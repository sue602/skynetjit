/* Bit operations.
 *
 * Copyright (c) 2009-Present, Redis Ltd.
 * All rights reserved.
 *
 * Licensed under your choice of (a) the Redis Source Available License 2.0
 * (RSALv2); or (b) the Server Side Public License v1 (SSPLv1); or (c) the
 * GNU Affero General Public License v3 (AGPLv3).
 */
/* skynetjit port: command layer removed, see redislib-src/NOTICE */

#include "redislib_port.h"
#include "ctype.h"

#ifdef HAVE_AVX2
/* Define __MM_MALLOC_H to prevent importing the memory aligned
 * allocation functions, which we don't use. */
#define __MM_MALLOC_H
#include <immintrin.h>
#endif

#ifdef HAVE_AVX512
/* Define __MM_MALLOC_H to prevent importing the memory aligned
 * allocation functions, which we don't use. */
#define __MM_MALLOC_H
#include <immintrin.h>
#endif

#ifdef HAVE_AARCH64_NEON
#include <arm_neon.h>
#endif

#ifdef HAVE_AVX2
#define BITOP_USE_AVX2 (__builtin_cpu_supports("avx2"))
#else
#define BITOP_USE_AVX2 0
#endif

/* AArch64 NEON support is determined at compile time via HAVE_AARCH64_NEON */
#ifdef HAVE_AVX512
#define BITOP_USE_AVX512 (__builtin_cpu_supports("avx512f"))
#define BITOPS_USE_AVX512_POPCOUNT  (__builtin_cpu_supports("avx512f") && __builtin_cpu_supports("avx512vpopcntdq"))
#else
#define BITOP_USE_AVX512 0
#define BITOPS_USE_AVX512_POPCOUNT  0
#endif


/* -----------------------------------------------------------------------------
 * Helpers and low level bit functions.
 * -------------------------------------------------------------------------- */

 /* Shared lookup table for bit counting - maps each byte value to its popcount */
static const uint8_t bitsinbyte[256] = {
    #define B2(n) n, n+1, n+1, n+2
    #define B4(n) B2(n), B2(n+1), B2(n+1), B2(n+2)
    #define B6(n) B4(n), B4(n+1), B4(n+1), B4(n+2)
    B6(0), B6(1), B6(1), B6(2)
    #undef B6
    #undef B4
    #undef B2
};

/* Count number of bits set in the binary array pointed by 's' and long
 * 'count' bytes. The implementation of this function is required to
 * work with an input string length up to 512 MB or more (server.proto_max_bulk_len) */
ATTRIBUTE_TARGET_POPCNT
long long redisPopcount(void *s, long count) {
    long long bits = 0;
    unsigned char *p = s;
    uint32_t *p4;
#if defined(HAVE_POPCNT)
    int use_popcnt = __builtin_cpu_supports("popcnt"); /* Check if CPU supports POPCNT instruction. */
#else
    int use_popcnt = 0; /* Assume CPU does not support POPCNT if
                         * __builtin_cpu_supports() is not available. */
#endif
    /* Count initial bytes not aligned to 64-bit when using the POPCNT instruction,
     * otherwise align to 32-bit. */
    int align = use_popcnt ? 7 : 3;
    while ((unsigned long)p & align && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    if (likely(use_popcnt)) {
        /* Use separate counters to make the CPU think there are no
         * dependencies between these popcnt operations. */
        uint64_t cnt[4];
        memset(cnt, 0, sizeof(cnt));

        /* Count bits 32 bytes at a time by using popcnt.
         * Unroll the loop to avoid the overhead of a single popcnt per iteration,
         * allowing the CPU to extract more instruction-level parallelism.
         * Reference: https://danluu.com/assembly-intrinsics/ */
        while (count >= 32) {
            cnt[0] += __builtin_popcountll(*(uint64_t*)(p));
            cnt[1] += __builtin_popcountll(*(uint64_t*)(p + 8));
            cnt[2] += __builtin_popcountll(*(uint64_t*)(p + 16));
            cnt[3] += __builtin_popcountll(*(uint64_t*)(p + 24));
            count -= 32;
            p += 32;
            /* Prefetch with 2K stride is just enough to overlap L3 miss latency effectively
             * without causing pressure on lower memory hierarchy or polluting L1/L2 */
            redis_prefetch_read(p + 2048);
        }
        bits += cnt[0] + cnt[1] + cnt[2] + cnt[3];
        goto remain;
    }

    /* Count bits 28 bytes at a time */
    p4 = (uint32_t*)p;
    while(count>=28) {
        uint32_t aux1, aux2, aux3, aux4, aux5, aux6, aux7;

        aux1 = *p4++;
        aux2 = *p4++;
        aux3 = *p4++;
        aux4 = *p4++;
        aux5 = *p4++;
        aux6 = *p4++;
        aux7 = *p4++;
        count -= 28;

        aux1 = aux1 - ((aux1 >> 1) & 0x55555555);
        aux1 = (aux1 & 0x33333333) + ((aux1 >> 2) & 0x33333333);
        aux2 = aux2 - ((aux2 >> 1) & 0x55555555);
        aux2 = (aux2 & 0x33333333) + ((aux2 >> 2) & 0x33333333);
        aux3 = aux3 - ((aux3 >> 1) & 0x55555555);
        aux3 = (aux3 & 0x33333333) + ((aux3 >> 2) & 0x33333333);
        aux4 = aux4 - ((aux4 >> 1) & 0x55555555);
        aux4 = (aux4 & 0x33333333) + ((aux4 >> 2) & 0x33333333);
        aux5 = aux5 - ((aux5 >> 1) & 0x55555555);
        aux5 = (aux5 & 0x33333333) + ((aux5 >> 2) & 0x33333333);
        aux6 = aux6 - ((aux6 >> 1) & 0x55555555);
        aux6 = (aux6 & 0x33333333) + ((aux6 >> 2) & 0x33333333);
        aux7 = aux7 - ((aux7 >> 1) & 0x55555555);
        aux7 = (aux7 & 0x33333333) + ((aux7 >> 2) & 0x33333333);
        bits += ((((aux1 + (aux1 >> 4)) & 0x0F0F0F0F) +
                    ((aux2 + (aux2 >> 4)) & 0x0F0F0F0F) +
                    ((aux3 + (aux3 >> 4)) & 0x0F0F0F0F) +
                    ((aux4 + (aux4 >> 4)) & 0x0F0F0F0F) +
                    ((aux5 + (aux5 >> 4)) & 0x0F0F0F0F) +
                    ((aux6 + (aux6 >> 4)) & 0x0F0F0F0F) +
                    ((aux7 + (aux7 >> 4)) & 0x0F0F0F0F))* 0x01010101) >> 24;
    }
    p = (unsigned char*)p4;

remain:
    /* Count the remaining bytes. */
    while(count--) bits += bitsinbyte[*p++];
    return bits;
}

#ifdef HAVE_AARCH64_NEON
/* AArch64 optimized popcount implementation.
 * Processes the input bitmap using four NEON vector accumulators in parallel
 * to improve instruction-level parallelism and reduce the frequency of
 * scalar reductions. Each accumulator holds 16-bit partial sums that are
 * combined only once per large block (128 bytes), minimizing data movement.
 *
 * Benchmark results show this approach outperforms 2-lane implementations
 * and matches or exceeds 8-lane versions in throughput, while avoiding
 * register pressure and keeping the backend pipeline fully utilized.
 *
 * This function is now memory bound on large bitmaps, as confirmed by perf
 * profiling, with backend stalls dominated by L1/L2 data cache refills.
 */
long long redisPopCountAarch64(void *s, long count) {
    long long bits = 0;
    const uint8_t *p = (const uint8_t*)s;

    /* Align */
    while (((uintptr_t)p & 15) && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    /* Four vector accumulators of u16 (pairwise-accumulated byte counts). */
    uint16x8_t acc0 = vdupq_n_u16(0);
    uint16x8_t acc1 = vdupq_n_u16(0);
    uint16x8_t acc2 = vdupq_n_u16(0);
    uint16x8_t acc3 = vdupq_n_u16(0);

    /* Process 128B per loop to amortize reductions. */
    while (count >= 128) {
        uint8x16_t d0 = vld1q_u8(p +  0);
        uint8x16_t d1 = vld1q_u8(p + 16);
        uint8x16_t d2 = vld1q_u8(p + 32);
        uint8x16_t d3 = vld1q_u8(p + 48);
        uint8x16_t d4 = vld1q_u8(p + 64);
        uint8x16_t d5 = vld1q_u8(p + 80);
        uint8x16_t d6 = vld1q_u8(p + 96);
        uint8x16_t d7 = vld1q_u8(p +112);

        /* Per-byte popcount */
        uint8x16_t c0 = vcntq_u8(d0);
        uint8x16_t c1 = vcntq_u8(d1);
        uint8x16_t c2 = vcntq_u8(d2);
        uint8x16_t c3 = vcntq_u8(d3);
        uint8x16_t c4 = vcntq_u8(d4);
        uint8x16_t c5 = vcntq_u8(d5);
        uint8x16_t c6 = vcntq_u8(d6);
        uint8x16_t c7 = vcntq_u8(d7);

        /* Pairwise widen-add with accumulation: u8 -> u16, stay in vectors */
        acc0 = vpadalq_u8(acc0, c0);
        acc1 = vpadalq_u8(acc1, c1);
        acc2 = vpadalq_u8(acc2, c2);
        acc3 = vpadalq_u8(acc3, c3);

        acc0 = vpadalq_u8(acc0, c4);
        acc1 = vpadalq_u8(acc1, c5);
        acc2 = vpadalq_u8(acc2, c6);
        acc3 = vpadalq_u8(acc3, c7);

        p += 128;
        count -= 128;
    }

    /* Reduce vector accumulators to scalar once. */
    uint32x4_t s0 = vpaddlq_u16(acc0);
    uint32x4_t s1 = vpaddlq_u16(acc1);
    uint32x4_t s2 = vpaddlq_u16(acc2);
    uint32x4_t s3 = vpaddlq_u16(acc3);
    uint32x4_t s01 = vaddq_u32(s0, s1);
    uint32x4_t s23 = vaddq_u32(s2, s3);
    uint32x4_t st = vaddq_u32(s01, s23);
    uint64x2_t s64 = vpaddlq_u32(st);
    bits += (long long)(vgetq_lane_u64(s64, 0) + vgetq_lane_u64(s64, 1));

    /* Remaining 64B blocks (keep vector domain) */
    while (count >= 64) {
        uint8x16_t d0 = vld1q_u8(p +  0);
        uint8x16_t d1 = vld1q_u8(p + 16);
        uint8x16_t d2 = vld1q_u8(p + 32);
        uint8x16_t d3 = vld1q_u8(p + 48);

        uint8x16_t c0 = vcntq_u8(d0);
        uint8x16_t c1 = vcntq_u8(d1);
        uint8x16_t c2 = vcntq_u8(d2);
        uint8x16_t c3 = vcntq_u8(d3);

        uint64x2_t t0 = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c0)));
        uint64x2_t t1 = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c1)));
        uint64x2_t t2 = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c2)));
        uint64x2_t t3 = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(c3)));

        uint64x2_t s = vaddq_u64(vaddq_u64(t0, t1), vaddq_u64(t2, t3));
        bits += (long long)(vgetq_lane_u64(s, 0) + vgetq_lane_u64(s, 1));

        p += 64;
        count -= 64;
    }

    /* 16B chunks */
    while (count >= 16) {
        uint8x16_t d = vld1q_u8(p);
        uint64x2_t s = vpaddlq_u32(vpaddlq_u16(vpaddlq_u8(vcntq_u8(d))));
        bits += (long long)(vgetq_lane_u64(s, 0) + vgetq_lane_u64(s, 1));
        p += 16;
        count -= 16;
    }

    /* Tail */
    while (count--) bits += bitsinbyte[*p++];

    return bits;
}
#endif

#ifdef HAVE_AVX512
/* AVX512 optimized version of redisPopcount using VPOPCNTDQ instruction.
 * This function requires AVX512F and AVX512VPOPCNTDQ support. */
ATTRIBUTE_TARGET_AVX512_POPCOUNT
long long redisPopCountAvx512(void *s, long count) {
    long long bits = 0;
    unsigned char *p = s;

    /* Align to 64-byte boundary for optimal AVX512 performance */
    while ((unsigned long)p & 63 && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    /* Process 64 bytes at a time using AVX512 */
    while (count >= 64) {
        __m512i data = _mm512_loadu_si512((__m512i*)p);
        __m512i popcnt = _mm512_popcnt_epi64(data);

        /* Sum all 8 64-bit popcount results */
        bits += _mm512_reduce_add_epi64(popcnt);

        p += 64;
        count -= 64;

        /* Prefetch next cache line */
        redis_prefetch_read(p + 2048);
    }

    /* Handle remaining bytes with scalar popcount */
    while (count >= 8) {
        bits += __builtin_popcountll(*(uint64_t*)p);
        p += 8;
        count -= 8;
    }

    /* Handle final bytes */
    while (count--) {
        bits += bitsinbyte[*p++];
    }

    return bits;
}
#endif

#ifdef HAVE_AVX2
/* AVX2 optimized version of redisPopcount.
 * This function requires AVX2 and POPCNT support. */
ATTRIBUTE_TARGET_AVX2_POPCOUNT
long long redisPopCountAvx2(void *s, long count) {
    long long bits = 0;
    unsigned char *p = s;

    /* Align to 8-byte boundary for 64-bit operations */
    while ((unsigned long)p & 7 && count) {
        bits += bitsinbyte[*p++];
        count--;
    }

    /* Use separate counters to avoid dependencies, similar to regular redisPopcount */
    uint64_t cnt[4];
    memset(cnt, 0, sizeof(cnt));

    /* Process 32 bytes at a time using POPCNT on 64-bit chunks */
    while (count >= 32) {
        cnt[0] += __builtin_popcountll(*(uint64_t*)(p));
        cnt[1] += __builtin_popcountll(*(uint64_t*)(p + 8));
        cnt[2] += __builtin_popcountll(*(uint64_t*)(p + 16));
        cnt[3] += __builtin_popcountll(*(uint64_t*)(p + 24));

        p += 32;
        count -= 32;

        /* Prefetch next cache line */
        redis_prefetch_read(p + 2048);
    }

    bits += cnt[0] + cnt[1] + cnt[2] + cnt[3];

    /* Handle remaining bytes with scalar popcount */
    while (count >= 8) {
        bits += __builtin_popcountll(*(uint64_t*)p);
        p += 8;
        count -= 8;
    }

    /* Handle final bytes */
    while (count--) {
        bits += bitsinbyte[*p++];
    }

    return bits;
}
#endif

/* Automatically select the best available popcount implementation */
static inline long long redisPopcountAuto(const unsigned char *p, long count) {
#ifdef HAVE_AVX512
    if (BITOPS_USE_AVX512_POPCOUNT) {
        return redisPopCountAvx512((void*)p, count);
    }
#endif
#ifdef HAVE_AVX2
    if (BITOP_USE_AVX2) {
        return redisPopCountAvx2((void*)p, count);
    }
#endif
#ifdef HAVE_AARCH64_NEON
    return redisPopCountAarch64((void*)p, count);
#else
    return redisPopcount((void*)p, count);
#endif
}

/* Return the position of the first bit set to one (if 'bit' is 1) or
 * zero (if 'bit' is 0) in the bitmap starting at 's' and long 'count' bytes.
 *
 * The function is guaranteed to return a value >= 0 if 'bit' is 0 since if
 * no zero bit is found, it returns count*8 assuming the string is zero
 * padded on the right. However if 'bit' is 1 it is possible that there is
 * not a single set bit in the bitmap. In this special case -1 is returned. */
long long redisBitpos(void *s, unsigned long count, int bit) {
    unsigned long *l;
    unsigned char *c;
    unsigned long skipval, word = 0, one;
    long long pos = 0; /* Position of bit, to return to the caller. */
    unsigned long j;
    int found;

    /* Process whole words first, seeking for first word that is not
     * all ones or all zeros respectively if we are looking for zeros
     * or ones. This is much faster with large strings having contiguous
     * blocks of 1 or 0 bits compared to the vanilla bit per bit processing.
     *
     * Note that if we start from an address that is not aligned
     * to sizeof(unsigned long) we consume it byte by byte until it is
     * aligned. */

    /* Skip initial bits not aligned to sizeof(unsigned long) byte by byte. */
    skipval = bit ? 0 : UCHAR_MAX;
    c = (unsigned char*) s;
    found = 0;
    while((unsigned long)c & (sizeof(*l)-1) && count) {
        if (*c != skipval) {
            found = 1;
            break;
        }
        c++;
        count--;
        pos += 8;
    }

    /* Skip bits with full word step. */
    l = (unsigned long*) c;
    if (!found) {
        skipval = bit ? 0 : ULONG_MAX;
        while (count >= sizeof(*l)) {
            if (*l != skipval) break;
            l++;
            count -= sizeof(*l);
            pos += sizeof(*l)*8;
        }
    }

    /* Load bytes into "word" considering the first byte as the most significant
     * (we basically consider it as written in big endian, since we consider the
     * string as a set of bits from left to right, with the first bit at position
     * zero.
     *
     * Note that the loading is designed to work even when the bytes left
     * (count) are less than a full word. We pad it with zero on the right. */
    c = (unsigned char*)l;
    for (j = 0; j < sizeof(*l); j++) {
        word <<= 8;
        if (count) {
            word |= *c;
            c++;
            count--;
        }
    }

    /* Special case:
     * If bits in the string are all zero and we are looking for one,
     * return -1 to signal that there is not a single "1" in the whole
     * string. This can't happen when we are looking for "0" as we assume
     * that the right of the string is zero padded. */
    if (bit == 1 && word == 0) return -1;

    /* Last word left, scan bit by bit. The first thing we need is to
     * have a single "1" set in the most significant position in an
     * unsigned long. We don't know the size of the long so we use a
     * simple trick. */
    one = ULONG_MAX; /* All bits set to 1.*/
    one >>= 1;       /* All bits set to 1 but the MSB. */
    one = ~one;      /* All bits set to 0 but the MSB. */

    while(one) {
        if (((one & word) != 0) == bit) return pos;
        pos++;
        one >>= 1;
    }

    /* If we reached this point, there is a bug in the algorithm, since
     * the case of no match is handled as a special case before. */
    serverPanic("End of redisBitpos() reached.");
    return 0; /* Just to avoid warnings. */
}

/* The following set.*Bitfield and get.*Bitfield functions implement setting
 * and getting arbitrary size (up to 64 bits) signed and unsigned integers
 * at arbitrary positions into a bitmap.
 *
 * The representation considers the bitmap as having the bit number 0 to be
 * the most significant bit of the first byte, and so forth, so for example
 * setting a 5 bits unsigned integer to value 23 at offset 7 into a bitmap
 * previously set to all zeroes, will produce the following representation:
 *
 * +--------+--------+
 * |00000001|01110000|
 * +--------+--------+
 *
 * When offsets and integer sizes are aligned to bytes boundaries, this is the
 * same as big endian, however when such alignment does not exist, its important
 * to also understand how the bits inside a byte are ordered.
 *
 * Note that this format follows the same convention as SETBIT and related
 * commands.
 */

void setUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, uint64_t value) {
    uint64_t byte, bit, byteval, bitval, j;

    for (j = 0; j < bits; j++) {
        bitval = (value & ((uint64_t)1<<(bits-1-j))) != 0;
        byte = offset >> 3;
        bit = 7 - (offset & 0x7);
        byteval = p[byte];
        byteval &= ~(1 << bit);
        byteval |= bitval << bit;
        p[byte] = byteval & 0xff;
        offset++;
    }
}

void setSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits, int64_t value) {
    uint64_t uv = value; /* Casting will add UINT64_MAX + 1 if v is negative. */
    setUnsignedBitfield(p,offset,bits,uv);
}

uint64_t getUnsignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits) {
    uint64_t byte, bit, byteval, bitval, j, value = 0;

    for (j = 0; j < bits; j++) {
        byte = offset >> 3;
        bit = 7 - (offset & 0x7);
        byteval = p[byte];
        bitval = (byteval >> bit) & 1;
        value = (value<<1) | bitval;
        offset++;
    }
    return value;
}

int64_t getSignedBitfield(unsigned char *p, uint64_t offset, uint64_t bits) {
    int64_t value;
    union {uint64_t u; int64_t i;} conv;

    /* Converting from unsigned to signed is undefined when the value does
     * not fit, however here we assume two's complement and the original value
     * was obtained from signed -> unsigned conversion, so we'll find the
     * most significant bit set if the original value was negative.
     *
     * Note that two's complement is mandatory for exact-width types
     * according to the C99 standard. */
    conv.u = getUnsignedBitfield(p,offset,bits);
    value = conv.i;

    /* If the top significant bit is 1, propagate it to all the
     * higher bits for two's complement representation of signed
     * integers. */
    if (bits < 64 && (value & ((uint64_t)1 << (bits-1))))
        value |= ((uint64_t)-1) << bits;
    return value;
}

/* The following two functions detect overflow of a value in the context
 * of storing it as an unsigned or signed integer with the specified
 * number of bits. The functions both take the value and a possible increment.
 * If no overflow could happen and the value+increment fit inside the limits,
 * then zero is returned, otherwise in case of overflow, 1 is returned,
 * otherwise in case of underflow, -1 is returned.
 *
 * When non-zero is returned (overflow or underflow), if not NULL, *limit is
 * set to the value the operation should result when an overflow happens,
 * depending on the specified overflow semantics:
 *
 * For BFOVERFLOW_SAT if 1 is returned, *limit it is set maximum value that
 * you can store in that integer. when -1 is returned, *limit is set to the
 * minimum value that an integer of that size can represent.
 *
 * For BFOVERFLOW_WRAP *limit is set by performing the operation in order to
 * "wrap" around towards zero for unsigned integers, or towards the most
 * negative number that is possible to represent for signed integers. */

#define BFOVERFLOW_WRAP 0
#define BFOVERFLOW_SAT 1
#define BFOVERFLOW_FAIL 2 /* Used by the BITFIELD command implementation. */

int checkUnsignedBitfieldOverflow(uint64_t value, int64_t incr, uint64_t bits, int owtype, uint64_t *limit) {
    uint64_t max = (bits == 64) ? UINT64_MAX : (((uint64_t)1<<bits)-1);
    int64_t maxincr = max-value;
    int64_t minincr = -value;

    if (value > max || (incr > 0 && incr > maxincr)) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = max;
            }
        }
        return 1;
    } else if (incr < 0 && incr < minincr) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = 0;
            }
        }
        return -1;
    }
    return 0;

handle_wrap:
    {
        uint64_t mask = ((uint64_t)-1) << bits;
        uint64_t res = value+incr;

        res &= ~mask;
        *limit = res;
    }
    return 1;
}

int checkSignedBitfieldOverflow(int64_t value, int64_t incr, uint64_t bits, int owtype, int64_t *limit) {
    int64_t max = (bits == 64) ? INT64_MAX : (((int64_t)1<<(bits-1))-1);
    int64_t min = (-max)-1;

    /* Note that maxincr and minincr could overflow, but we use the values
     * only after checking 'value' range, so when we use it no overflow
     * happens. 'uint64_t' cast is there just to prevent undefined behavior on
     * overflow */
    int64_t maxincr = (uint64_t)max-value;
    int64_t minincr = min-value;

    if (value > max || (bits != 64 && incr > maxincr) || (value >= 0 && incr > 0 && incr > maxincr))
    {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = max;
            }
        }
        return 1;
    } else if (value < min || (bits != 64 && incr < minincr) || (value < 0 && incr < 0 && incr < minincr)) {
        if (limit) {
            if (owtype == BFOVERFLOW_WRAP) {
                goto handle_wrap;
            } else if (owtype == BFOVERFLOW_SAT) {
                *limit = min;
            }
        }
        return -1;
    }
    return 0;

handle_wrap:
    {
        uint64_t msb = (uint64_t)1 << (bits-1);
        uint64_t a = value, b = incr, c;
        c = a+b; /* Perform addition as unsigned so that's defined. */

        /* If the sign bit is set, propagate to all the higher order
         * bits, to cap the negative value. If it's clear, mask to
         * the positive integer limit. */
        if (bits < 64) {
            uint64_t mask = ((uint64_t)-1) << bits;
            if (c & msb) {
                c |= mask;
            } else {
                c &= ~mask;
            }
        }
        *limit = c;
    }
    return 1;
}

/* Debugging function. Just show bits in the specified bitmap. Not used
 * but here for not having to rewrite it when debugging is needed. */
void printBits(unsigned char *p, unsigned long count) {
    unsigned long j, i, byte;

    for (j = 0; j < count; j++) {
        byte = p[j];
        for (i = 0x80; i > 0; i /= 2)
            printf("%c", (byte & i) ? '1' : '0');
        printf("|");
    }
    printf("\n");
}

/* -----------------------------------------------------------------------------
 * Bits related string commands: GETBIT, SETBIT, BITCOUNT, BITOP.
 * -------------------------------------------------------------------------- */

#define BITOP_AND   0
#define BITOP_OR    1
#define BITOP_XOR   2
#define BITOP_NOT   3
#define BITOP_DIFF  4 /* DIFF(X, A1, A2, ..., An) = X & !(A1 | A2 | ... | An) */
#define BITOP_DIFF1 5 /* DIFF1(X, A1, A2, ..., An) = !X & (A1 | A2 | ... | An) */
#define BITOP_ANDOR 6 /* ANDOR(X, A1, A2, ..., An) = X & (A1 | A2 | ... | An) */

/* ONE(A1, A2, ..., An) = X.
 * If X[i] is the i-th bit of X then:
 * X[i] == 1 if and only if there is m such that:
 * Am[i] == 1 and Al[i] == 0 for all l != m. */
#define BITOP_ONE   7

#define BITFIELDOP_GET 0
#define BITFIELDOP_SET 1
#define BITFIELDOP_INCRBY 2

/* Return a pointer to the string object content, and stores its length
 * in 'len'. The user is required to pass (likely stack allocated) buffer
 * 'llbuf' of at least LONG_STR_SIZE bytes. Such a buffer is used in the case
 * the object is integer encoded in order to provide the representation
 * without using heap allocation.
 *
 * The function returns the pointer to the object array of bytes representing
 * the string it contains, that may be a pointer to 'llbuf' or to the
 * internal object representation. As a side effect 'len' is filled with
 * the length of such buffer.
 *
 * If the source object is NULL the function is guaranteed to return NULL
 * and set 'len' to 0. */
unsigned char *getObjectReadOnlyString(robj *o, long *len, char *llbuf) {
    serverAssert(!o || o->type == OBJ_STRING);
    unsigned char *p = NULL;

    /* Set the 'p' pointer to the string, that can be just a stack allocated
     * array if our string was integer encoded. */
    if (o && o->encoding == OBJ_ENCODING_INT) {
        p = (unsigned char*) llbuf;
        if (len) *len = ll2string(llbuf,LONG_STR_SIZE,(long)o->ptr);
    } else if (o) {
        p = (unsigned char*) o->ptr;
        if (len) *len = sdslen(o->ptr);
    } else {
        if (len) *len = 0;
    }
    return p;
}

#ifdef HAVE_AVX2
/* Compute the given bitop operation using AVX2 intrinsics.
 * Return how many bytes were successfully processed, as AVX2 operates on
 * 256-bit registers so if `minlen` is not a multiple of 32 some of the bytes
 * will be skipped. They will be taken care for in the unoptimized loop in the
 * main bitopCommand function. */
ATTRIBUTE_TARGET_AVX2
unsigned long bitopCommandAVX(unsigned char **keys, unsigned char *res, 
                              unsigned long op, unsigned long numkeys,
                              unsigned long minlen)
{
    const unsigned long step = sizeof(__m256i);

    unsigned long i;
    unsigned long processed = 0;
    unsigned char *res_start = res;
    unsigned char *fst_key = keys[0];

    if (minlen < step) {
        return 0;
    }

    const __m256i max256 = _mm256_set1_epi64x(-1);
    const __m256i zero256 = _mm256_set1_epi64x(0);

    switch (op) {
    case BITOP_AND:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)(keys[0]+processed));

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_and_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    /* Unlike other operations that do the same with all source keys
     * DIFF, DIFF1 and ANDOR all compute the disjunction of all the source keys
     * but the first one. We first store that disjunction in `lres` and later
     * compute the final operation using the first source key. */
    case BITOP_DIFF:
    case BITOP_DIFF1:
    case BITOP_ANDOR:
    case BITOP_OR:
        while (minlen >= step) {
            __m256i lres = (op == BITOP_OR) ?
                _mm256_lddqu_si256((__m256i*)(keys[0]+processed)) :
                zero256;

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_or_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_XOR:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)(keys[0]+processed));

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                lres = _mm256_xor_si256(lres, lkey);
            }
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_NOT:
        while (minlen >= step) {
             __m256i lres = _mm256_lddqu_si256((__m256i*)(keys[0]+processed));
            lres = _mm256_xor_si256(lres, max256);
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_ONE:
        while (minlen >= step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)(keys[0]+processed));
            __m256i common_bits = zero256;

            for (i = 1; i < numkeys; i++) {
                __m256i lkey = _mm256_lddqu_si256((__m256i*)(keys[i]+processed));
                __m256i common = _mm256_and_si256(lres, lkey);
                common_bits = _mm256_or_si256(common_bits, common);

                lres = _mm256_xor_si256(lres, lkey);
            }
            lres = _mm256_andnot_si256(common_bits, lres);
            _mm256_storeu_si256((__m256i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    default:
        break;
    }

    res = res_start;
    switch (op) {
    case BITOP_DIFF:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_andnot_si256(lres, fkey);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_DIFF1:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_andnot_si256(fkey, lres);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_ANDOR:
        for (i = 0; i < processed; i += step) {
            __m256i lres = _mm256_lddqu_si256((__m256i*)res);
            __m256i fkey = _mm256_lddqu_si256((__m256i*)fst_key);

            lres = _mm256_and_si256(fkey, lres);
            _mm256_storeu_si256((__m256i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    default:
        break;
    }

    return processed;
}
#endif /* HAVE_AVX2 */

#ifdef HAVE_AVX512
/* Compute the given bitop operation using AVX512 intrinsics.
 * Return how many bytes were successfully processed, as AVX512 operates on
 * 512-bit registers so if `minlen` is not a multiple of 64 some of the bytes
 * will be skipped. They will be taken care for in the unoptimized loop in the
 * main bitopCommand function. */
ATTRIBUTE_TARGET_AVX512
unsigned long bitopCommandAVX512(unsigned char **keys, unsigned char *res, 
                                 unsigned long op, unsigned long numkeys,
                                 unsigned long minlen)
{
    const unsigned long step = sizeof(__m512i);  /* 64 bytes */

    unsigned long i;
    unsigned long processed = 0;
    unsigned char *res_start = res;
    unsigned char *fst_key = keys[0];

    if (minlen < step) {
        return 0;
    }

    const __m512i max512 = _mm512_set1_epi64(-1);
    const __m512i zero512 = _mm512_set1_epi64(0);
    switch (op) {
    case BITOP_AND:
        while (minlen >= step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)(keys[0]+processed));

            for (i = 1; i < numkeys; i++) {
                __m512i lkey = _mm512_loadu_si512((__m512i*)(keys[i]+processed));
                lres = _mm512_and_si512(lres, lkey);
            }
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    /* Unlike other operations that do the same with all source keys
     * DIFF, DIFF1 and ANDOR all compute the disjunction of all the source keys
     * but the first one. We first store that disjunction in `lres` and later
     * compute the final operation using the first source key. */
    case BITOP_DIFF:
    case BITOP_DIFF1:
    case BITOP_ANDOR:
    case BITOP_OR:
        while (minlen >= step) {
            __m512i lres = (op == BITOP_OR) ?
                _mm512_loadu_si512((__m512i*)(keys[0]+processed)) :
                zero512;

            for (i = 1; i < numkeys; i++) {
                __m512i lkey = _mm512_loadu_si512((__m512i*)(keys[i]+processed));
                lres = _mm512_or_si512(lres, lkey);
            }
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_XOR:
        while (minlen >= step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)(keys[0]+processed));

            for (i = 1; i < numkeys; i++) {
                __m512i lkey = _mm512_loadu_si512((__m512i*)(keys[i]+processed));
                lres = _mm512_xor_si512(lres, lkey);
            }
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_NOT:
        while (minlen >= step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)(keys[0]+processed));
            lres = _mm512_xor_si512(lres, max512);
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    case BITOP_ONE:
        while (minlen >= step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)(keys[0]+processed));
            __m512i common_bits = zero512;

            for (i = 1; i < numkeys; i++) {
                __m512i lkey = _mm512_loadu_si512((__m512i*)(keys[i]+processed));
                /* common_bits |= (lres & lkey): ternary-logic with imm8 0xEA == c|(a&b)
                 * (a=lres, b=lkey, c=common_bits), replacing a separate AND+OR. */
                common_bits = _mm512_ternarylogic_epi32(lres, lkey, common_bits, 0xEA);

                lres = _mm512_xor_si512(lres, lkey);
            }
            lres = _mm512_andnot_si512(common_bits, lres);
            _mm512_storeu_si512((__m512i*)res, lres);
            res += step;
            processed += step;
            minlen -= step;
        }
        break;
    default:
        break;
    }

    res = res_start;
    switch (op) {
    case BITOP_DIFF:
        for (i = 0; i < processed; i += step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)res);
            __m512i fkey = _mm512_loadu_si512((__m512i*)fst_key);

            lres = _mm512_andnot_si512(lres, fkey);
            _mm512_storeu_si512((__m512i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_DIFF1:
        for (i = 0; i < processed; i += step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)res);
            __m512i fkey = _mm512_loadu_si512((__m512i*)fst_key);

            lres = _mm512_andnot_si512(fkey, lres);
            _mm512_storeu_si512((__m512i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    case BITOP_ANDOR:
        for (i = 0; i < processed; i += step) {
            __m512i lres = _mm512_loadu_si512((__m512i*)res);
            __m512i fkey = _mm512_loadu_si512((__m512i*)fst_key);

            lres = _mm512_and_si512(fkey, lres);
            _mm512_storeu_si512((__m512i*)res, lres);

            res += step;
            fst_key += step;
        }
        break;
    default:
        break;
    }

    return processed;
}
#endif /* HAVE_AVX512 */

/* BITFIELD key subcommand-1 arg ... subcommand-2 arg ... subcommand-N ...
 *
 * Supported subcommands:
 *
 * GET <type> <offset>
 * SET <type> <offset> <value>
 * INCRBY <type> <offset> <increment>
 * OVERFLOW [WRAP|SAT|FAIL]
 */

#define BITFIELD_FLAG_NONE      0
#define BITFIELD_FLAG_READONLY  (1<<0)

struct bitfieldOp {
    uint64_t offset;    /* Bitfield offset. */
    int64_t i64;        /* Increment amount (INCRBY) or SET value */
    int opcode;         /* Operation id. */
    int owtype;         /* Overflow type to use. */
    int bits;           /* Integer bitfield bits width. */
    int sign;           /* True if signed, otherwise unsigned op. */
};

#ifdef REDIS_TEST
/* Test function to verify popcount implementations */
int bitopsTest(int argc, char **argv, int flags) {
    UNUSED(argc);
    UNUSED(argv);
    UNUSED(flags);

    /* Test data with known popcount values */
    unsigned char test_data[] = {0xFF, 0x00, 0xAA, 0x55, 0xF0, 0x0F, 0x33, 0xCC};
    int expected_bits = 8 + 0 + 4 + 4 + 4 + 4 + 4 + 4; /* = 32 bits */

    long long result_regular = redisPopcount(test_data, sizeof(test_data));

    printf("Regular popcount: %lld (expected: %d)\n", result_regular, expected_bits);

    if (result_regular != expected_bits) {
        printf("FAIL: Regular popcount mismatch\n");
        return 1;
    }

#ifdef HAVE_AVX2
    if (BITOP_USE_AVX2) {
        long long result_avx2 = redisPopCountAvx2(test_data, sizeof(test_data));
        printf("AVX2 popcount: %lld (expected: %d)\n", result_avx2, expected_bits);

        if (result_avx2 != expected_bits) {
            printf("FAIL: AVX2 popcount mismatch\n");
            return 1;
        }
    } else {
        printf("AVX2 not supported on this CPU\n");
    }
#else
    printf("AVX2 not compiled in\n");
#endif

#ifdef HAVE_AVX512
    if (BITOP_USE_AVX512) {
        long long result_avx512 = redisPopCountAvx512(test_data, sizeof(test_data));
        printf("AVX512 popcount: %lld (expected: %d)\n", result_avx512, expected_bits);

        if (result_avx512 != expected_bits) {
            printf("FAIL: AVX512 popcount mismatch\n");
            return 1;
        }
    } else {
        printf("AVX512 not supported on this CPU\n");
    }
#else
    printf("AVX512 not compiled in\n");
#endif

#ifdef HAVE_AARCH64_NEON
    {
        long long result_aarch64 = redisPopCountAarch64(test_data, sizeof(test_data));
        printf("AArch64 NEON popcount: %lld (expected: %d)\n", result_aarch64, expected_bits);

        if (result_aarch64 != expected_bits) {
            printf("FAIL: AArch64 NEON popcount mismatch\n");
            return 1;
        }
    }
#else
    printf("AArch64 NEON not available\n");
#endif
    printf("All popcount tests passed!\n");
    return 0;
}
#endif
