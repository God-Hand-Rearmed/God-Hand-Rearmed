#ifndef PS2_RUNTIME_MACROS_H
#define PS2_RUNTIME_MACROS_H
#include <cstdint>
#include <cmath>
#include <cstring>
#include <bit>
#if defined(_MSC_VER)
#include <intrin.h>
#elif defined(USE_SSE2NEON)
#include "sse2neon.h"
#else
#include <immintrin.h> // For SSE/AVX intrinsics
#endif

#include "ps2_runtime.h"
#include "ps2_ee_float.h"

static PS2_FORCEINLINE int32_t Ps2ExtractEpi32(__m128i v, int index)
{
    switch (index & 3)
    {
    case 0:
        return _mm_extract_epi32(v, 0);
    case 1:
        return _mm_extract_epi32(v, 1);
    case 2:
        return _mm_extract_epi32(v, 2);
    default:
        return _mm_extract_epi32(v, 3);
    }
}

static PS2_FORCEINLINE int64_t Ps2ExtractEpi64(__m128i v, int index)
{
    if ((index & 1) == 0)
    {
        return _mm_cvtsi128_si64(v);
    }
    else
    {
        return _mm_extract_epi64(v, 1);
    }
}

static inline uint32_t ps2_clz32(uint32_t x)
{
    return static_cast<uint32_t>(std::countl_zero(x));
}

static PS2_FORCEINLINE uint64_t Ps2HiLoToU64(uint64_t hi, uint64_t lo)
{
    return ((hi & 0xFFFFFFFFull) << 32) | (lo & 0xFFFFFFFFull);
}

static PS2_FORCEINLINE uint64_t Ps2SignExt32ToU64(uint32_t v)
{
    return (uint64_t)(int64_t)(int32_t)v;
}

// PLZCW: Count leading bits that match the sign bit, minus 1.
// For positive values: count leading zeros minus 1 (excludes sign bit).
// For negative values: count leading ones minus 1 (excludes sign bit).
// Special cases: 0x00000000 -> 31, 0xFFFFFFFF -> 31.
static inline uint32_t ps2_plzcw32(uint32_t x)
{
    if (x == 0 || x == 0xFFFFFFFF)
        return 31;
    if (x & 0x80000000u)
        x = ~x; // If sign bit set, invert to count leading ones as zeros
    return static_cast<uint32_t>(std::countl_zero(x)) - 1;
}

#define PS2_BLENDV_PS(a, b, mask) _mm_blendv_ps((a), (b), (mask))
#define PS2_MIN_EPI32(a, b) _mm_min_epi32((a), (b))
#define PS2_MAX_EPI32(a, b) _mm_max_epi32((a), (b))
#define PS2_SHUFFLE_EPI8(v, mask) _mm_shuffle_epi8((v), (mask))

#define PS2_EXTRACT_EPI32(v, i) Ps2ExtractEpi32((v), (i))
#define PS2_EXTRACT_EPI64(v, i) Ps2ExtractEpi64((v), (i))

#define PS2_EXTRACT_EPI32_0(v) Ps2ExtractEpi32((v), 0)
#define PS2_EXTRACT_EPI32_1(v) Ps2ExtractEpi32((v), 1)
#define PS2_EXTRACT_EPI32_2(v) Ps2ExtractEpi32((v), 2)
#define PS2_EXTRACT_EPI32_3(v) Ps2ExtractEpi32((v), 3)

#define PS2_EXTRACT_EPI64_0(v) Ps2ExtractEpi64((v), 0)
#define PS2_EXTRACT_EPI64_1(v) Ps2ExtractEpi64((v), 1)

// Basic MIPS arithmetic operations
#define ADD32(a, b) ((uint32_t)((a) + (b)))
#define ADD32_OV(rs, rt, result32, overflow)              \
    do                                                    \
    {                                                     \
        int32_t _a = (int32_t)(rs);                       \
        int32_t _b = (int32_t)(rt);                       \
        int32_t _r = _a + _b;                             \
        overflow = (((_a ^ _b) >= 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                          \
    } while (0);
#define SUB32(a, b) ((uint32_t)((a) - (b)))
#define SUB32_OV(rs, rt, result32, overflow)             \
    do                                                   \
    {                                                    \
        int32_t _a = (int32_t)(rs);                      \
        int32_t _b = (int32_t)(rt);                      \
        int32_t _r = _a - _b;                            \
        overflow = (((_a ^ _b) < 0) && ((_a ^ _r) < 0)); \
        result32 = (uint32_t)_r;                         \
    } while (0);
#define MUL32(a, b) ((uint32_t)((a) * (b)))
#define DIV32(a, b) ((uint32_t)((a) / (b)))
#define AND32(a, b) ((uint32_t)((a) & (b)))
#define OR32(a, b) ((uint32_t)((a) | (b)))
#define XOR32(a, b) ((uint32_t)((a) ^ (b)))
#define NOR32(a, b) ((uint32_t)(~((a) | (b))))
#define SLL32(a, b) ((uint32_t)((a) << (b)))
#define SRL32(a, b) ((uint32_t)((a) >> (b)))
#define SRA32(a, b) ((uint32_t)((int32_t)(a) >> (b)))
#define SLT32(a, b) ((uint32_t)((int32_t)(a) < (int32_t)(b) ? 1 : 0))
#define SLTU32(a, b) ((uint32_t)((a) < (b) ? 1 : 0))

// PS2-specific 128-bit MMI operations
#define PS2_PEXTLW(a, b) _mm_unpacklo_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUW(a, b) _mm_unpackhi_epi32((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLH(a, b) _mm_unpacklo_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUH(a, b) _mm_unpackhi_epi16((__m128i)(b), (__m128i)(a))
#define PS2_PEXTLB(a, b) _mm_unpacklo_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PEXTUB(a, b) _mm_unpackhi_epi8((__m128i)(b), (__m128i)(a))
#define PS2_PADDW(a, b) _mm_add_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSUBW(a, b) _mm_sub_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PMAXW(a, b) PS2_MAX_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PMINW(a, b) PS2_MIN_EPI32((__m128i)(a), (__m128i)(b))
#define PS2_PADDH(a, b) _mm_add_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PSUBH(a, b) _mm_sub_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMAXH(a, b) _mm_max_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PMINH(a, b) _mm_min_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PADDB(a, b) _mm_add_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PSUBB(a, b) _mm_sub_epi8((__m128i)(a), (__m128i)(b))
#define PS2_PAND(a, b) _mm_and_si128((__m128i)(a), (__m128i)(b))
#define PS2_POR(a, b) _mm_or_si128((__m128i)(a), (__m128i)(b))
#define PS2_PXOR(a, b) _mm_xor_si128((__m128i)(a), (__m128i)(b))
#define PS2_PNOR(a, b) _mm_xor_si128(_mm_or_si128((__m128i)(a), (__m128i)(b)), _mm_set1_epi32(0xFFFFFFFF))

// EE FPU and VU0 macro arithmetic. Default (since Nov 20): the host's ops under the PS2's rounding and flush, ps2ee
// wherever a value is past FLT_MAX or a divisor is 0 (see ps2FpuPastMax below). With PS2_EXACT_EE_FLOAT defined at build
// time: the PS2's own rules, bit for bit (ps2_ee_float.h, from GT3 Revisited): add / sub
// chop after the adder's one-guard-bit alignment, the multiplier's and divider's last-place quirks, no denormals,
// exponent 255 an ordinary exponent (+-0x7FFFFFFF the largest values). The registers keep floats. VU1 keeps its own
// maths (rendering only). add / sub / mul (+ madd / msub, VU0's too) run on the host's floats under the game thread's
// chop + DAZ / FTZ (main.cpp applyEeFloatingPointMode: guest code runs on that thread only), bit for bit ps2ee's, and
// fall back to ps2ee (integer code, the same on every host) where the host can't be exact: add aligns the smaller
// operand to the larger's half ulp first ((y + S) - S, S = +-2^(ex-1)); mul is the chopped host product unless its
// bits 16-22 are 0 (the multiplier's lost carry may show; a round a's is taken from a table).
PS2_FORCEINLINE float ps2FpuAddHost(float a, float b, bool &inexact)
{
    const uint32_t ia = ps2ee::bits(a), ib = ps2ee::bits(b);
    const uint32_t ma = ia & 0x7FFFFFFFu, mb = ib & 0x7FFFFFFFu;
    const bool swap = mb > ma;
    const uint32_t x = swap ? ib : ia, y = swap ? ia : ib; // |x| >= |y|
    const uint32_t ex = (swap ? mb : ma) >> 23, ey = (swap ? ma : mb) >> 23;
    float yf = ps2ee::fl(y);
    if (ex - ey >= 2u)
    {
        const float half = ps2ee::fl((y & 0x80000000u) | ((ex - 1u) << 23));
        yf = (yf + half) - half;
    }
    const float r = ps2ee::fl(x) + yf;
    inexact = (ps2ee::bits(r) & 0x7FFFFFFFu) > 0x7F7FFFFEu; // past FLT_MAX: an overflow or an exponent-255 operand
    return r;
}
PS2_FORCEINLINE float ps2FpuMulHost(float a, float b, bool &inexact)
{
    const float r = a * b;
    const uint32_t ia = ps2ee::bits(a), ib = ps2ee::bits(b), rb = ps2ee::bits(r);
    const uint32_t low = ((ia & 0x7FFFFFu) | 0x800000u) * ((ib & 0x7FFFFFu) | 0x800000u); // product's low 32 bits
    const bool carry = (low & 0x7F0000u) == 0u && (ia & 0x7F800000u) != 0u && (ib & 0x7F800000u) != 0u &&
                       (ib & 0xFFFFu) != 0u; // (not with a 0 operand, nor a round b: x * 1.0 is the chopped product)
    inexact = (rb & 0x7FFFFFFFu) > 0x7F7FFFFEu;
    if (carry && !inexact)
    {
        // A round a (1.0 * x, 0.5 * x, 3.0 * x ...): the lost carry by table, the product one lower in its last place
        // where its bits 16-22 (23 with the top bit) are 0. Anything else, or a result near the smallest normal, goes
        // to ps2ee.
        if ((ia & 0xFFFFu) == 0u && (rb & 0x7F000000u) != 0u)
        {
            const uint64_t p = uint64_t((ia & 0x7FFFFFu) | 0x800000u) * uint64_t((ib & 0x7FFFFFu) | 0x800000u);
            const uint32_t hi = uint32_t(p >> 47);
            if ((p & ((uint64_t(1) << (23 + hi)) - 1)) < (uint64_t(1) << 16))
                return ps2ee::fl(rb - ps2ee::roundCarry(ib));
        }
        else
            inexact = true;
    }
    return r;
}
#if defined(PS2_EXACT_EE_FLOAT)
PS2_FORCEINLINE float ps2FpuAdd(float a, float b)
{
    bool inexact;
    const float r = ps2FpuAddHost(a, b, inexact);
    return inexact ? ps2ee::fl(ps2ee::add(ps2ee::bits(a), ps2ee::bits(b))) : r;
}
PS2_FORCEINLINE float ps2FpuSub(float a, float b) { return ps2FpuAdd(a, -b); }
// a = fs, b = ft: the multiplier's quirk is not commutative.
PS2_FORCEINLINE float ps2FpuMul(float a, float b)
{
    bool inexact;
    const float r = ps2FpuMulHost(a, b, inexact);
    return inexact ? ps2ee::fl(ps2ee::mul(ps2ee::bits(a), ps2ee::bits(b))) : r;
}
PS2_FORCEINLINE float ps2FpuMadd(float acc, float a, float b)
{
    bool mulInexact, addInexact;
    const float r = ps2FpuAddHost(acc, ps2FpuMulHost(a, b, mulInexact), addInexact);
    return (mulInexact || addInexact) ? ps2ee::fl(ps2ee::madd(ps2ee::bits(acc), ps2ee::bits(a), ps2ee::bits(b))) : r;
}
PS2_FORCEINLINE float ps2FpuMsub(float acc, float a, float b)
{
    bool mulInexact, addInexact;
    const float r = ps2FpuAddHost(acc, -ps2FpuMulHost(a, b, mulInexact), addInexact);
    return (mulInexact || addInexact) ? ps2ee::fl(ps2ee::msub(ps2ee::bits(acc), ps2ee::bits(a), ps2ee::bits(b))) : r;
}
#else
// The default (K, Nov 20: accurate to God Hand's gameplay, not to the PS2's hardware): the host's own add / mul under
// the game thread's chop + DAZ / FTZ (the PS2's rounding direction and its flush of tiny values), without the PS2
// adder's alignment and the multiplier's lost carry, which only ever move the last bit. A result at or past FLT_MAX
// (an overflow, or any exponent-255 operand: the PS2 has no Inf / NaN, those are ordinary large numbers to it) is
// redone by ps2ee, so overflow, the game's +-max sentinels and clamping stay the PS2's exactly and no Inf / NaN ever
// reaches the game. PS2_EXACT_EE_FLOAT (build-time) restores the exact rules above.
// At or past FLT_MAX - 1 ulp: FLT_MAX +- a small value chops to FLT_MAX - 1 ulp on the host but stays FLT_MAX on the
// PS2 (the adder drops the small operand), and the game may use +-FLT_MAX as a sentinel, so those go to ps2ee too.
PS2_FORCEINLINE bool ps2FpuPastMax(float r) { return (ps2ee::bits(r) & 0x7FFFFFFFu) > 0x7F7FFFFDu; }
PS2_FORCEINLINE float ps2FpuAdd(float a, float b)
{
    const float r = a + b;
    return ps2FpuPastMax(r) ? ps2ee::fl(ps2ee::add(ps2ee::bits(a), ps2ee::bits(b))) : r;
}
PS2_FORCEINLINE float ps2FpuSub(float a, float b) { return ps2FpuAdd(a, -b); }
PS2_FORCEINLINE float ps2FpuMul(float a, float b)
{
    const float r = a * b;
    return ps2FpuPastMax(r) ? ps2ee::fl(ps2ee::mul(ps2ee::bits(a), ps2ee::bits(b))) : r;
}
// The product rounded first, as on the PS2 (no fused multiply-add).
PS2_FORCEINLINE float ps2FpuMadd(float acc, float a, float b)
{
    const float p = a * b, r = acc + p;
    return ps2FpuPastMax(p) || ps2FpuPastMax(r) ? ps2ee::fl(ps2ee::madd(ps2ee::bits(acc), ps2ee::bits(a), ps2ee::bits(b))) : r;
}
PS2_FORCEINLINE float ps2FpuMsub(float acc, float a, float b)
{
    const float p = a * b, r = acc - p;
    return ps2FpuPastMax(p) || ps2FpuPastMax(r) ? ps2ee::fl(ps2ee::msub(ps2ee::bits(acc), ps2ee::bits(a), ps2ee::bits(b))) : r;
}
#endif
PS2_FORCEINLINE float ps2FpuMax(float a, float b) { return ps2ee::fl(ps2ee::maxOf(ps2ee::bits(a), ps2ee::bits(b))); }
PS2_FORCEINLINE float ps2FpuMin(float a, float b) { return ps2ee::fl(ps2ee::minOf(ps2ee::bits(a), ps2ee::bits(b))); }
PS2_FORCEINLINE float ps2FpuFromWord(int32_t v) { return ps2ee::fl(ps2ee::fromInt(v)); }

// VU0 macro ops: the same rules on each lane. Fast path on the host's SSE floats under the game thread's chop +
// DAZ / FTZ, bit for bit the same as ps2ee: add / sub align the smaller operand to the larger's half ulp first
// ((y + S) - S with S = +-2^(ex-1), exact under chop), mul is the chopped host product. A vector with a lane the host
// can't do exactly (an exponent-255 operand, a result at or past FLT_MAX, a product whose bits below its last place
// are < 2^16: the multiplier's lost carry may show; most products of round numbers such as 1.0 * x) is redone lane by
// lane with the scalar ops above (each exact, ps2ee only for the lanes that need it), out of line.
#if defined(_MSC_VER)
#define PS2_NOINLINE __declspec(noinline)
#else
#define PS2_NOINLINE __attribute__((noinline))
#endif
enum class Ps2VuLaneOp
{
    Add,
    Madd,
    Msub
};
PS2_NOINLINE inline __m128 ps2VuExactLanes(Ps2VuLaneOp op, __m128 acc, __m128 a, __m128 b)
{
    alignas(16) float z[4], x[4], y[4], r[4];
    _mm_store_ps(z, acc);
    _mm_store_ps(x, a);
    _mm_store_ps(y, b);
    for (int i = 0; i < 4; ++i)
        r[i] = op == Ps2VuLaneOp::Add    ? ps2FpuAdd(x[i], y[i])
               : op == Ps2VuLaneOp::Madd ? ps2FpuMadd(z[i], x[i], y[i])
                                         : ps2FpuMsub(z[i], x[i], y[i]);
    return _mm_load_ps(r);
}

// The host result r is exact unless a lane's magnitude is at or past FLT_MAX (an overflow chopped to FLT_MAX, or an
// exponent-255 operand's Inf / NaN).
PS2_FORCEINLINE bool ps2VuPastMax(__m128 r)
{
    const __m128i magnitude = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F7FFFFE)))) != 0;
}

// The default mode's check: the lanes of a vector at or past FLT_MAX - 1 ulp (an overflow, an exponent-255 operand,
// FLT_MAX +- a small value; see ps2FpuPastMax).
PS2_FORCEINLINE int ps2VuPastMaxLanes(__m128 r)
{
    const __m128i magnitude = _mm_and_si128(_mm_castps_si128(r), _mm_set1_epi32(0x7FFFFFFF));
    return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmpgt_epi32(magnitude, _mm_set1_epi32(0x7F7FFFFD))));
}

// The host sum, exact unless a lane comes out at or past FLT_MAX (*inexact set): an overflow chopped to FLT_MAX, or
// an exponent-255 operand (the host's Inf / NaN carries through to the result).
PS2_FORCEINLINE __m128 ps2VuAddHost(__m128 a, __m128 b, bool &inexact)
{
    const __m128i ia = _mm_castps_si128(a), ib = _mm_castps_si128(b), magMask = _mm_set1_epi32(0x7FFFFFFF);
    const __m128i ma = _mm_and_si128(ia, magMask), mb = _mm_and_si128(ib, magMask);
    const __m128i swap = _mm_cmpgt_epi32(mb, ma);
    const __m128i x = _mm_blendv_epi8(ia, ib, swap), y = _mm_blendv_epi8(ib, ia, swap); // |x| >= |y|
    const __m128i ex = _mm_srli_epi32(_mm_max_epi32(ma, mb), 23), ey = _mm_srli_epi32(_mm_min_epi32(ma, mb), 23);
    const __m128i align = _mm_cmpgt_epi32(_mm_sub_epi32(ex, ey), _mm_set1_epi32(1));
    // +-2^(ex-1) with y's sign (unused where align is off, so ex <= 1 needs no care)
    const __m128i half = _mm_or_si128(_mm_and_si128(y, _mm_set1_epi32(int(0x80000000u))),
                                      _mm_slli_epi32(_mm_sub_epi32(ex, _mm_set1_epi32(1)), 23));
    const __m128 yf = _mm_castsi128_ps(y), hf = _mm_castsi128_ps(half);
    const __m128 yAligned = _mm_blendv_ps(yf, _mm_sub_ps(_mm_add_ps(yf, hf), hf), _mm_castsi128_ps(align));
    const __m128 r = _mm_add_ps(_mm_castsi128_ps(x), yAligned);
    inexact = ps2VuPastMax(r);
    return r;
}

#if defined(PS2_EXACT_EE_FLOAT)
PS2_FORCEINLINE __m128 ps2VuAdd(__m128 a, __m128 b)
{
    bool inexact;
    const __m128 r = ps2VuAddHost(a, b, inexact);
    return inexact ? ps2VuExactLanes(Ps2VuLaneOp::Add, a, a, b) : r;
}
#else
// The default: the host's sum; a lane at or past FLT_MAX sends the vector lane by lane through ps2FpuAdd (host
// sums, ps2ee for the lanes past max).
PS2_FORCEINLINE __m128 ps2VuAdd(__m128 a, __m128 b)
{
    const __m128 r = _mm_add_ps(a, b);
    return ps2VuPastMaxLanes(r) ? ps2VuExactLanes(Ps2VuLaneOp::Add, a, a, b) : r;
}
#endif

PS2_FORCEINLINE __m128 ps2VuSub(__m128 a, __m128 b)
{
    return ps2VuAdd(a, _mm_xor_ps(b, _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u)))));
}

// The chopped host product, exact except in the lanes set in check: a lane past FLT_MAX (an overflow, or an
// exponent-255 operand's Inf / NaN), or a lane whose product's bits 16-22 are 0, so the multiplier's lost carry may
// show (not when the product is 0, an operand 0 or tiny, nor when b is round, its low 16 bits 0: x * 1.0, x * 2.0 ...
// are the chopped product).
PS2_FORCEINLINE __m128 ps2VuMulHost(__m128 a, __m128 b, int &check)
{
    const __m128 r = _mm_mul_ps(a, b);
    const __m128i ia = _mm_castps_si128(a), ib = _mm_castps_si128(b), ir = _mm_castps_si128(r);
    const __m128i zero = _mm_setzero_si128();
    const __m128i mantissa = _mm_set1_epi32(0x7FFFFF), hidden = _mm_set1_epi32(0x800000);
    const __m128i low = _mm_mullo_epi32(_mm_or_si128(_mm_and_si128(ia, mantissa), hidden),
                                        _mm_or_si128(_mm_and_si128(ib, mantissa), hidden)); // product's low 32 bits
    const __m128i carry = _mm_cmpeq_epi32(_mm_and_si128(low, _mm_set1_epi32(0x7F0000)), zero);
    const __m128i skip = _mm_or_si128(_mm_cmpeq_epi32(_mm_and_si128(ir, _mm_set1_epi32(0x7F800000)), zero),
                                      _mm_cmpeq_epi32(_mm_and_si128(ib, _mm_set1_epi32(0xFFFF)), zero));
    const __m128i past = _mm_cmpgt_epi32(_mm_and_si128(ir, _mm_set1_epi32(0x7FFFFFFF)), _mm_set1_epi32(0x7F7FFFFE));
    check = _mm_movemask_ps(_mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(skip, carry), past)));
    return r;
}

// The exact products of the lanes the host flagged (ps2FpuMul: the host's product, a round a's carry by table, ps2ee
// for the rest); a lane that overflowed is +-0x7FFFFFFF, which the add after it sees as past FLT_MAX.
PS2_NOINLINE inline __m128 ps2VuMulCheck(__m128 r, __m128 a, __m128 b, int lanes)
{
    alignas(16) float x[4], y[4], p[4];
    _mm_store_ps(x, a);
    _mm_store_ps(y, b);
    _mm_store_ps(p, r);
    for (int i = 0; i < 4; ++i)
        if (lanes & (1 << i))
            p[i] = ps2FpuMul(x[i], y[i]);
    return _mm_load_ps(p);
}

#if defined(PS2_EXACT_EE_FLOAT)
// a = fs, b = ft (the multiplier's quirk is not commutative).
PS2_FORCEINLINE __m128 ps2VuMul(__m128 a, __m128 b)
{
    int check;
    const __m128 r = ps2VuMulHost(a, b, check);
    return check ? ps2VuMulCheck(r, a, b, check) : r;
}
#else
// The default: the host's product; lanes past FLT_MAX redone by ps2FpuMul (ps2ee for them).
PS2_FORCEINLINE __m128 ps2VuMul(__m128 a, __m128 b)
{
    const __m128 r = _mm_mul_ps(a, b);
    const int lanes = ps2VuPastMaxLanes(r);
    return lanes ? ps2VuMulCheck(r, a, b, lanes) : r;
}
#endif

#if defined(PS2_EXACT_EE_FLOAT)
// acc + a * b / acc - a * b: the product rounded first; a product that overflowed (+-max) is the result. Any lane
// the host can't do exactly sends the vector lane by lane (an overflowed product reaches the add past FLT_MAX, so the
// add's own check catches it).
PS2_FORCEINLINE __m128 ps2VuMadd(__m128 acc, __m128 a, __m128 b)
{
    int check;
    bool addInexact;
    __m128 p = ps2VuMulHost(a, b, check);
    if (check)
        p = ps2VuMulCheck(p, a, b, check);
    const __m128 r = ps2VuAddHost(acc, p, addInexact);
    return addInexact ? ps2VuExactLanes(Ps2VuLaneOp::Madd, acc, a, b) : r;
}
PS2_FORCEINLINE __m128 ps2VuMsub(__m128 acc, __m128 a, __m128 b)
{
    int check;
    bool addInexact;
    __m128 p = ps2VuMulHost(a, b, check);
    if (check)
        p = ps2VuMulCheck(p, a, b, check);
    const __m128 r = ps2VuAddHost(acc, _mm_xor_ps(p, _mm_castsi128_ps(_mm_set1_epi32(int(0x80000000u)))), addInexact);
    return addInexact ? ps2VuExactLanes(Ps2VuLaneOp::Msub, acc, a, b) : r;
}
#else
// The default: the host's product rounded, then the host's sum (no fused multiply-add, as on the PS2); a lane past
// FLT_MAX in either sends the vector lane by lane through ps2FpuMadd / ps2FpuMsub.
PS2_FORCEINLINE __m128 ps2VuMadd(__m128 acc, __m128 a, __m128 b)
{
    const __m128 p = _mm_mul_ps(a, b), r = _mm_add_ps(acc, p);
    return (ps2VuPastMaxLanes(p) | ps2VuPastMaxLanes(r))
               ? ps2VuExactLanes(Ps2VuLaneOp::Madd, acc, a, b)
               : r;
}
PS2_FORCEINLINE __m128 ps2VuMsub(__m128 acc, __m128 a, __m128 b)
{
    const __m128 p = _mm_mul_ps(a, b), r = _mm_sub_ps(acc, p);
    return (ps2VuPastMaxLanes(p) | ps2VuPastMaxLanes(r))
               ? ps2VuExactLanes(Ps2VuLaneOp::Msub, acc, a, b)
               : r;
}
#endif

// max / min: sign-magnitude order with exponent-0 values as zero (+0 == -0: the first operand wins a tie).
PS2_FORCEINLINE __m128i ps2VuKey(__m128 v)
{
    const __m128i i = _mm_castps_si128(v);
    const __m128i sign = _mm_srai_epi32(i, 31);
    const __m128i key = _mm_sub_epi32(_mm_xor_si128(_mm_and_si128(i, _mm_set1_epi32(0x7FFFFFFF)), sign), sign);
    return _mm_andnot_si128(_mm_cmpeq_epi32(_mm_and_si128(i, _mm_set1_epi32(0x7F800000)), _mm_setzero_si128()), key);
}
PS2_FORCEINLINE __m128 ps2VuMax(__m128 a, __m128 b)
{
    return _mm_blendv_ps(a, b, _mm_castsi128_ps(_mm_cmpgt_epi32(ps2VuKey(b), ps2VuKey(a))));
}
PS2_FORCEINLINE __m128 ps2VuMin(__m128 a, __m128 b)
{
    return _mm_blendv_ps(a, b, _mm_castsi128_ps(_mm_cmpgt_epi32(ps2VuKey(a), ps2VuKey(b))));
}

PS2_FORCEINLINE uint32_t ps2FloatBits(float value)
{
    uint32_t bits;
    std::memcpy(&bits, &value, sizeof(bits));
    return bits;
}

PS2_FORCEINLINE uint32_t ps2FpuCompareKey(float value)
{
    uint32_t bits = ps2FloatBits(value);
    if ((bits & 0x7F800000u) == 0u)
        bits = 0u;
    return (bits & 0x80000000u) != 0u ? ~bits : bits ^ 0x80000000u;
}

PS2_FORCEINLINE bool ps2FloatIsZero(float value)
{
    return (ps2FloatBits(value) & 0x7F800000u) == 0u;
}

constexpr uint32_t PS2_FCR31_I = 0x00020000u;
constexpr uint32_t PS2_FCR31_D = 0x00010000u;
constexpr uint32_t PS2_FCR31_SI = 0x00000040u;
constexpr uint32_t PS2_FCR31_SD = 0x00000020u;

// Divide / square root (div.s, sqrt.s, rsqrt.s, and VU0's vdiv / vsqrt / vrsqrt into Q). By default the host's
// divider / root under the game thread's chop for ordinary operands (exponents 1-254, a result below FLT_MAX), without
// the PS2 divider's occasional one-above last bit; zero divisors, exponent-0 / 255 operands and results past FLT_MAX
// take ps2ee, so +-max for a division by zero, the PS2's huge-number handling and no Inf / NaN stay exact.
// PS2_EXACT_EE_FLOAT: ps2ee for every one.
PS2_FORCEINLINE uint32_t ps2EeDiv(uint32_t a, uint32_t b)
{
#if !defined(PS2_EXACT_EE_FLOAT)
    const uint32_t ea = (a >> 23) & 0xFFu, eb = (b >> 23) & 0xFFu;
    if (eb - 1u < 254u && ea != 255u)
    {
        const uint32_t r = ps2ee::bits(_mm_cvtss_f32(_mm_div_ss(_mm_set_ss(ps2ee::fl(a)), _mm_set_ss(ps2ee::fl(b)))));
        if ((r & 0x7FFFFFFFu) <= 0x7F7FFFFDu)
            return r;
    }
#endif
    return ps2ee::div(a, b);
}
PS2_FORCEINLINE uint32_t ps2EeSqrt(uint32_t a) // sqrt(|a|)
{
#if !defined(PS2_EXACT_EE_FLOAT)
    if (((a >> 23) & 0xFFu) - 1u < 254u)
        return ps2ee::bits(_mm_cvtss_f32(_mm_sqrt_ss(_mm_set_ss(ps2ee::fl(a & 0x7FFFFFFFu)))));
#endif
    return ps2ee::sqrt(a);
}
PS2_FORCEINLINE uint32_t ps2EeRsqrt(uint32_t a, uint32_t b) // a / sqrt(|b|)
{
#if !defined(PS2_EXACT_EE_FLOAT)
    const uint32_t ea = (a >> 23) & 0xFFu, eb = (b >> 23) & 0xFFu;
    if (eb - 1u < 254u && ea != 255u)
    {
        const __m128 root = _mm_sqrt_ss(_mm_set_ss(ps2ee::fl(b & 0x7FFFFFFFu)));
        const uint32_t r = ps2ee::bits(_mm_cvtss_f32(_mm_div_ss(_mm_set_ss(ps2ee::fl(a)), root)));
        if ((r & 0x7FFFFFFFu) <= 0x7F7FFFFDu)
            return r;
    }
#endif
    return ps2ee::rsqrt(a, b);
}
inline float ps2FpuDivide(uint32_t &fcr31, float dividend, float divisor)
{
    const uint32_t a = ps2ee::bits(dividend), b = ps2ee::bits(divisor);
    if (ps2ee::isZero(b))
        fcr31 |= ps2ee::isZero(a) ? (PS2_FCR31_I | PS2_FCR31_SI) : (PS2_FCR31_D | PS2_FCR31_SD);
    return ps2ee::fl(ps2EeDiv(a, b));
}

inline float ps2FpuSqrt(uint32_t &fcr31, float value)
{
    const uint32_t a = ps2ee::bits(value);
    if ((a & ps2ee::kSign) != 0u && !ps2ee::isZero(a))
        fcr31 |= PS2_FCR31_I | PS2_FCR31_SI;
    return ps2ee::fl(ps2EeSqrt(a));
}

inline float ps2FpuRsqrt(uint32_t &fcr31, float numerator, float value)
{
    const uint32_t a = ps2ee::bits(numerator), b = ps2ee::bits(value);
    if (ps2ee::isZero(b))
        fcr31 |= PS2_FCR31_D | PS2_FCR31_SD;
    else if ((b & ps2ee::kSign) != 0u)
        fcr31 |= PS2_FCR31_I | PS2_FCR31_SI;
    return ps2ee::fl(ps2EeRsqrt(a, b));
}

inline int32_t ps2FpuConvertToWord(float value)
{
    const uint32_t bits = ps2FloatBits(value);
    if ((bits & 0x7F800000u) >= 0x4F000000u)
        return (bits & 0x80000000u) ? INT32_MIN : INT32_MAX;
    return static_cast<int32_t>(value);
}

constexpr uint16_t PS2_VU_STATUS_I = 0x0010u;
constexpr uint16_t PS2_VU_STATUS_D = 0x0020u;
constexpr uint16_t PS2_VU_STATUS_IS = 0x0400u;
constexpr uint16_t PS2_VU_STATUS_DS = 0x0800u;

// VU0's divide unit (vdiv, vsqrt, vrsqrt into Q): the same divider's results; the status flags as before.
inline float ps2VuDivide(uint16_t &status, float dividend, float divisor)
{
    status &= static_cast<uint16_t>(~(PS2_VU_STATUS_I | PS2_VU_STATUS_D));
    const uint32_t a = ps2ee::bits(dividend), b = ps2ee::bits(divisor);
    if (ps2ee::isZero(b))
        status |= ps2ee::isZero(a) ? (PS2_VU_STATUS_I | PS2_VU_STATUS_IS) : (PS2_VU_STATUS_D | PS2_VU_STATUS_DS);
    return ps2ee::fl(ps2EeDiv(a, b));
}

inline float ps2VuSqrt(uint16_t &status, float value)
{
    status &= static_cast<uint16_t>(~(PS2_VU_STATUS_I | PS2_VU_STATUS_D));
    const uint32_t a = ps2ee::bits(value);
    if ((a & ps2ee::kSign) != 0u && !ps2ee::isZero(a))
        status |= PS2_VU_STATUS_I | PS2_VU_STATUS_IS;
    return ps2ee::fl(ps2EeSqrt(a));
}

inline float ps2VuRsqrt(uint16_t &status, float numerator, float value)
{
    status &= static_cast<uint16_t>(~(PS2_VU_STATUS_I | PS2_VU_STATUS_D));
    const uint32_t a = ps2ee::bits(numerator), b = ps2ee::bits(value);
    if (ps2ee::isZero(b))
        status |= ps2ee::isZero(a) ? (PS2_VU_STATUS_I | PS2_VU_STATUS_IS) : (PS2_VU_STATUS_D | PS2_VU_STATUS_DS);
    else if ((b & ps2ee::kSign) != 0u)
        status |= PS2_VU_STATUS_I | PS2_VU_STATUS_IS;
    return ps2ee::fl(ps2EeRsqrt(a, b));
}

// PS2 VU (Vector Unit) operations
#define PS2_VADD(a, b) ps2VuAdd((__m128)(a), (__m128)(b))
#define PS2_VSUB(a, b) ps2VuSub((__m128)(a), (__m128)(b))
#define PS2_VMUL(a, b) ps2VuMul((__m128)(a), (__m128)(b))
#define PS2_VMADD(acc, a, b) ps2VuMadd((__m128)(acc), (__m128)(a), (__m128)(b))
#define PS2_VMSUB(acc, a, b) ps2VuMsub((__m128)(acc), (__m128)(a), (__m128)(b))
#define PS2_VMAX(a, b) ps2VuMax((__m128)(a), (__m128)(b))
#define PS2_VMIN(a, b) ps2VuMin((__m128)(a), (__m128)(b))
#define PS2_VBLEND(a, b, mask) PS2_BLENDV_PS((__m128)(a), (__m128)(b), (__m128)(mask))

// Memory access helpers - Hybrid Fast/Slow Path
// Fast path: Direct RDRAM access (masked).
// Slow path: Full runtime->Load/Store

static PS2_FORCEINLINE bool Ps2FastRangeIsContiguous(uint32_t offset, uint32_t bytes)
{
    return offset <= (PS2_RDRAM_SIZE - bytes);
}

static PS2_FORCEINLINE uint8_t Ps2FastRead8(const uint8_t *rdram, uint32_t addr)
{
    return rdram[addr & PS2_RAM_MASK];
}

static PS2_FORCEINLINE uint16_t Ps2FastRead16(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint16_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint16_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static PS2_FORCEINLINE uint32_t Ps2FastRead32(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint32_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint32_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static PS2_FORCEINLINE uint64_t Ps2FastRead64(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        uint64_t value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    uint64_t value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static PS2_FORCEINLINE __m128i Ps2FastRead128(const uint8_t *rdram, uint32_t addr)
{
    const uint32_t offset = addr & PS2_RAM_MASK & ~0xFu;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            wrapped[i] = rdram[(offset + i) & PS2_RAM_MASK];
        }
        __m128i value;
        std::memcpy(&value, wrapped, sizeof(value));
        return value;
    }

    __m128i value;
    std::memcpy(&value, rdram + offset, sizeof(value));
    return value;
}

static PS2_FORCEINLINE void Ps2FastWrite8(uint8_t *rdram, uint32_t addr, uint8_t value)
{
    rdram[addr & PS2_RAM_MASK] = value;
}

static PS2_FORCEINLINE void Ps2FastWrite16(uint8_t *rdram, uint32_t addr, uint16_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint16_t)))
    {
        uint8_t wrapped[sizeof(uint16_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint16_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static PS2_FORCEINLINE void Ps2FastWrite32(uint8_t *rdram, uint32_t addr, uint32_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint32_t)))
    {
        uint8_t wrapped[sizeof(uint32_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint32_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static PS2_FORCEINLINE void Ps2FastWrite64(uint8_t *rdram, uint32_t addr, uint64_t value)
{
    const uint32_t offset = addr & PS2_RAM_MASK;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(uint64_t)))
    {
        uint8_t wrapped[sizeof(uint64_t)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(uint64_t); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

static PS2_FORCEINLINE void Ps2FastWrite128(uint8_t *rdram, uint32_t addr, __m128i value)
{
    const uint32_t offset = addr & PS2_RAM_MASK & ~0xFu;
    if (!Ps2FastRangeIsContiguous(offset, sizeof(__m128i)))
    {
        alignas(16) uint8_t wrapped[sizeof(__m128i)];
        std::memcpy(wrapped, &value, sizeof(value));
        for (uint32_t i = 0; i < sizeof(__m128i); ++i)
        {
            rdram[(offset + i) & PS2_RAM_MASK] = wrapped[i];
        }
        return;
    }
    std::memcpy(rdram + offset, &value, sizeof(value));
}

#define FAST_READ8(addr) Ps2FastRead8(rdram, (uint32_t)(addr))
#define FAST_READ16(addr) Ps2FastRead16(rdram, (uint32_t)(addr))
#define FAST_READ32(addr) Ps2FastRead32(rdram, (uint32_t)(addr))
#define FAST_READ64(addr) Ps2FastRead64(rdram, (uint32_t)(addr))
#define FAST_READ128(addr) Ps2FastRead128(rdram, (uint32_t)(addr))

#define FAST_WRITE8(addr, val) Ps2FastWrite8(rdram, (uint32_t)(addr), (uint8_t)(val))
#define FAST_WRITE16(addr, val) Ps2FastWrite16(rdram, (uint32_t)(addr), (uint16_t)(val))
#define FAST_WRITE32(addr, val) Ps2FastWrite32(rdram, (uint32_t)(addr), (uint32_t)(val))
#define FAST_WRITE64(addr, val) Ps2FastWrite64(rdram, (uint32_t)(addr), (uint64_t)(val))
#define FAST_WRITE128(addr, val) Ps2FastWrite128(rdram, (uint32_t)(addr), (val))

#define READ8(addr) ([&]() -> uint8_t {                       \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load8(rdram, ctx, _addr)                   \
        : FAST_READ8(_addr); }())

#define READ16(addr) ([&]() -> uint16_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load16(rdram, ctx, _addr)                  \
        : FAST_READ16(_addr); }())

#define READ32(addr) ([&]() -> uint32_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load32(rdram, ctx, _addr)                  \
        : FAST_READ32(_addr); }())

#define READ64(addr) ([&]() -> uint64_t {                     \
    uint32_t _addr = (uint32_t)(addr);                        \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load64(rdram, ctx, _addr)                  \
        : FAST_READ64(_addr); }())

#define READ128(addr) ([&]() -> __m128i {                     \
    uint32_t _addr = (uint32_t)(addr) & ~0xFu;                \
    return PS2Runtime::isSpecialAddress(_addr)                \
        ? runtime->Load128(rdram, ctx, _addr)                 \
        : FAST_READ128(_addr); }())

#define WRITE8(addr, val)                                                            \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (addr);                                                     \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store8(rdram, ctx, _addr, (val));                               \
        else                                                                         \
        {                                                                            \
            ps2TraceGuestWrite(rdram, _addr, 1u, (uint8_t)(val), 0u, "WRITE8", ctx); \
            FAST_WRITE8(_addr, (val));                                               \
        }                                                                            \
    } while (0)

#define WRITE16(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store16(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 2u, (uint16_t)(val), 0u, "WRITE16", ctx); \
            FAST_WRITE16(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE32(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store32(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 4u, (uint32_t)(val), 0u, "WRITE32", ctx); \
            FAST_WRITE32(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE64(addr, val)                                                             \
    do                                                                                 \
    {                                                                                  \
        uint32_t _addr = (addr);                                                       \
        if (PS2Runtime::isSpecialAddress(_addr))                                       \
            runtime->Store64(rdram, ctx, _addr, (val));                                \
        else                                                                           \
        {                                                                              \
            ps2TraceGuestWrite(rdram, _addr, 8u, (uint64_t)(val), 0u, "WRITE64", ctx); \
            FAST_WRITE64(_addr, (val));                                                \
        }                                                                              \
    } while (0)

#define WRITE128(addr, val)                                                          \
    do                                                                               \
    {                                                                                \
        uint32_t _addr = (uint32_t)(addr) & ~0xFu;                                   \
        __m128i _value = (val);                                                      \
        if (PS2Runtime::isSpecialAddress(_addr))                                     \
            runtime->Store128(rdram, ctx, _addr, _value);                            \
        else                                                                         \
        {                                                                            \
            const uint64_t _lo = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_value)); \
            const uint64_t _hi = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_value)); \
            ps2TraceGuestWrite(rdram, _addr, 16u, _lo, _hi, "WRITE128", ctx);        \
            FAST_WRITE128(_addr, _value);                                            \
        }                                                                            \
    } while (0)

// Packed Compare Greater Than (PCGT)
#define PS2_PCGTW(a, b) _mm_cmpgt_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCGTH(a, b) _mm_cmpgt_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCGTB(a, b) _mm_cmpgt_epi8((__m128i)(a), (__m128i)(b))

// Packed Add with Signed Saturation Word (PADDSW)
inline __m128i ps2_paddsw(__m128i a, __m128i b)
{

    __m128i sum = _mm_add_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, sum),
                                     _mm_xor_si128(b, sum));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(sum, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, sum));
    #endif
}
#define PS2_PADDSW(a, b) ps2_paddsw((__m128i)(a), (__m128i)(b))

// Packed Subtract with Signed Saturation Word (PSUBSW)
inline __m128i ps2_psubsw(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Check for over/underflow. Clamp to either INT32_MIN/INT32_MAX.
    __m128i overflow = _mm_and_si128(_mm_xor_si128(a, b),
                                     _mm_xor_si128(a, diff));
    // Extract input sign.
    overflow = _mm_srai_epi32(overflow, 31);
    __m128i input_sign = _mm_srai_epi32(a, 31);
    // Select saturation value based on overflow sign.
    #if defined(__SSE4_1__)
    __m128i sat = _mm_blendv_epi8(
        _mm_set1_epi32(INT32_MAX),
        _mm_set1_epi32(INT32_MIN),
        input_sign);
    return _mm_blendv_epi8(diff, sat, overflow);
    #else
    __m128i sat = _mm_or_si128(_mm_and_si128(input_sign, _mm_set1_epi32(INT32_MIN)),
                               _mm_andnot_si128(input_sign, _mm_set1_epi32(INT32_MAX)));
    return _mm_or_si128(_mm_and_si128(overflow, sat),
                        _mm_andnot_si128(overflow, diff));
    #endif

}
#define PS2_PSUBSW(a, b) ps2_psubsw((__m128i)(a), (__m128i)(b))

// Packed Compare Equal (PCEQ)
#define PS2_PCEQW(a, b) _mm_cmpeq_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PCEQH(a, b) _mm_cmpeq_epi16((__m128i)(a), (__m128i)(b))
#define PS2_PCEQB(a, b) _mm_cmpeq_epi8((__m128i)(a), (__m128i)(b))

// Packed Absolute (PABS)
#define PS2_PABSW(a) _mm_abs_epi32((__m128i)(a))
#define PS2_PABSH(a) _mm_abs_epi16((__m128i)(a))
#define PS2_PABSB(a) _mm_abs_epi8((__m128i)(a))

// Packed Pack (PPAC) - Packs larger elements into smaller ones
inline __m128i ps2_paddu32(__m128i a, __m128i b)
{
    __m128i sum = _mm_add_epi32(a, b);
    __m128i overflow = _mm_cmpgt_epi32(_mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)),
                                       _mm_xor_si128(sum, _mm_set1_epi32(INT32_MIN)));
    return _mm_or_si128(sum, overflow); // overflow lanes become all-1s
}
inline __m128i ps2_psubu32(__m128i a, __m128i b)
{
    __m128i diff = _mm_sub_epi32(a, b);
    // Underflow if a < b (unsigned). Clamp to 0.
    __m128i underflow = _mm_cmpgt_epi32(_mm_xor_si128(b, _mm_set1_epi32(INT32_MIN)),
                                        _mm_xor_si128(a, _mm_set1_epi32(INT32_MIN)));
    return _mm_andnot_si128(underflow, diff); // underflow lanes become 0
}

inline __m128i ps2_ppacw(__m128i rs, __m128i rt)
{
    // rs = [rs3 rs2 rs1 rs0], rt = [rt3 rt2 rt1 rt0]
    return _mm_castps_si128(_mm_shuffle_ps(_mm_castsi128_ps(rt), _mm_castsi128_ps(rs), _MM_SHUFFLE(2, 0, 2, 0)));
}
#define PS2_PPACW(a, b) ps2_ppacw((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppach(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 1, 4, 5, 8, 9, 12, 13,  // from rt: halfwords 0,2,4,6
        0, 1, 4, 5, 8, 9, 12, 13); // from rs: halfwords 0,2,4,6
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACH(a, b) ps2_ppach((__m128i)(a), (__m128i)(b))

inline __m128i ps2_ppacb(__m128i rs, __m128i rt)
{
    const __m128i mask = _mm_setr_epi8(
        0, 2, 4, 6, 8, 10, 12, 14,  // from rt: bytes 0,2,4,6,8,10,12,14
        0, 2, 4, 6, 8, 10, 12, 14); // from rs
    __m128i lo = _mm_shuffle_epi8(rt, mask);
    __m128i hi = _mm_shuffle_epi8(rs, mask);
    return _mm_unpacklo_epi64(lo, hi);
}
#define PS2_PPACB(a, b) ps2_ppacb((__m128i)(a), (__m128i)(b))

// Packed Interleave (PINT)
#define PS2_PINTH(a, b) _mm_unpacklo_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))
#define PS2_PINTEH(a, b) _mm_unpackhi_epi16(_mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)))

// Packed Multiply-Add (PMADD)
#define PS2_PMADDW(a, b) _mm_add_epi32(_mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(1, 0, 3, 2)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(1, 0, 3, 2))), _mm_mullo_epi32(_mm_shuffle_epi32((__m128i)(a), _MM_SHUFFLE(3, 2, 1, 0)), _mm_shuffle_epi32((__m128i)(b), _MM_SHUFFLE(3, 2, 1, 0))))

// Packed Variable Shifts
#define PS2_PSLLVW(a, b) _mm_custom_sllv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRLVW(a, b) _mm_custom_srlv_epi32((__m128i)(a), (__m128i)(b))
#define PS2_PSRAVW(a, b) _mm_custom_srav_epi32((__m128i)(a), (__m128i)(b))

inline __m128i _mm_custom_sllv_epi32(__m128i a, __m128i count)
{
    alignas(16) int32_t a_arr[4];
    alignas(16) int32_t count_arr[4];
    alignas(16) int32_t result[4];

    std::memcpy(a_arr, &a, sizeof(a));
    std::memcpy(count_arr, &count, sizeof(count));

    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] << (count_arr[i] & 0x1F);
    }

    __m128i out;
    std::memcpy(&out, result, sizeof(out));
    return out;
}

inline __m128i _mm_custom_srlv_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = (uint32_t)a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

inline __m128i _mm_custom_srav_epi32(__m128i a, __m128i count)
{
    int32_t a_arr[4], count_arr[4], result[4];
    _mm_storeu_si128((__m128i *)a_arr, a);
    _mm_storeu_si128((__m128i *)count_arr, count);
    for (int i = 0; i < 4; i++)
    {
        result[i] = a_arr[i] >> (count_arr[i] & 0x1F);
    }
    return _mm_loadu_si128((__m128i *)result);
}

// PMFHL function implementations
inline __m128i ps2_u64_to_epi64_pair(uint64_t value)
{
    return _mm_set1_epi64x(static_cast<long long>(value));
}

#define PS2_PMFHL_LW(hi, lo) _mm_unpacklo_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_UW(hi, lo) _mm_unpackhi_epi64(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_SLW(hi, lo) _mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi))
#define PS2_PMFHL_LH(hi, lo) _mm_shuffle_epi32(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0))
#define PS2_PMFHL_SH(hi, lo) _mm_shufflehi_epi16(_mm_shufflelo_epi16(_mm_packs_epi32(ps2_u64_to_epi64_pair(lo), ps2_u64_to_epi64_pair(hi)), _MM_SHUFFLE(3, 1, 2, 0)), _MM_SHUFFLE(3, 1, 2, 0))

// FPU (COP1) operations
#define FPU_SET_ACC(ctx, res) (ctx->f_acc = res)
#define FPU_ADD_S(a, b) ps2FpuAdd((float)(a), (float)(b))
#define FPU_SUB_S(a, b) ps2FpuSub((float)(a), (float)(b))
#define FPU_MUL_S(a, b) ps2FpuMul((float)(a), (float)(b))
#define FPU_MADD_S(acc, a, b) ps2FpuMadd((float)(acc), (float)(a), (float)(b))
#define FPU_MSUB_S(acc, a, b) ps2FpuMsub((float)(acc), (float)(a), (float)(b))
#define FPU_MAX_S(a, b) ps2FpuMax((float)(a), (float)(b))
#define FPU_MIN_S(a, b) ps2FpuMin((float)(a), (float)(b))
#define FPU_DIV_S(a, b) ps2ee::fl(ps2EeDiv(ps2ee::bits((float)(a)), ps2ee::bits((float)(b))))
#define FPU_SQRT_S(a) ps2ee::fl(ps2EeSqrt(ps2ee::bits((float)(a))))
#define FPU_ABS_S(a) fabsf((float)(a))
#define FPU_MOV_S(a) ((float)(a))
#define FPU_NEG_S(a) (-(float)(a))
#define FPU_ROUND_L_S(a) ((int64_t)roundf((float)(a)))
#define FPU_TRUNC_L_S(a) ((int64_t)(float)(a))
#define FPU_CEIL_L_S(a) ((int64_t)ceilf((float)(a)))
#define FPU_FLOOR_L_S(a) ((int64_t)floorf((float)(a)))
#define FPU_ROUND_W_S(a) ((int32_t)nearbyintf((float)(a)))
#define FPU_TRUNC_W_S(a) ((int32_t)(float)(a))
#define FPU_CEIL_W_S(a) ((int32_t)ceilf((float)(a)))
#define FPU_FLOOR_W_S(a) ((int32_t)floorf((float)(a)))
#define FPU_CVT_S_W(a) ps2FpuFromWord((int32_t)(a))
#define FPU_CVT_S_L(a) ((float)(int64_t)(a))
#define FPU_CVT_W_S(a) ps2FpuConvertToWord((float)(a))
#define FPU_CVT_L_S(a) ((int64_t)(float)(a))
#define FPU_C_F_S(a, b) (0)
#define FPU_C_UN_S(a, b) (0)
#define FPU_C_EQ_S(a, b) (ps2FpuCompareKey((float)(a)) == ps2FpuCompareKey((float)(b)))
#define FPU_C_UEQ_S(a, b) FPU_C_EQ_S(a, b)
#define FPU_C_OLT_S(a, b) (ps2FpuCompareKey((float)(a)) < ps2FpuCompareKey((float)(b)))
#define FPU_C_ULT_S(a, b) FPU_C_OLT_S(a, b)
#define FPU_C_OLE_S(a, b) (ps2FpuCompareKey((float)(a)) <= ps2FpuCompareKey((float)(b)))
#define FPU_C_ULE_S(a, b) FPU_C_OLE_S(a, b)
#define FPU_C_SF_S(a, b) (0)
#define FPU_C_NGLE_S(a, b) (0)
#define FPU_C_SEQ_S(a, b) FPU_C_EQ_S(a, b)
#define FPU_C_NGL_S(a, b) FPU_C_EQ_S(a, b)
#define FPU_C_LT_S(a, b) FPU_C_OLT_S(a, b)
#define FPU_C_NGE_S(a, b) FPU_C_OLT_S(a, b)
#define FPU_C_LE_S(a, b) FPU_C_OLE_S(a, b)
#define FPU_C_NGT_S(a, b) FPU_C_OLE_S(a, b)

// QFSRV: Quadword Funnel Shift Right Variable
// Concatenates rs || rt (256 bits) and right-shifts by SA bits, taking lower 128 bits.
inline __m128i ps2_qfsrv(__m128i rs, __m128i rt, uint32_t sa)
{
    if (sa == 0)
        return rt;
    if (sa >= 128)
    {
        if (sa >= 256)
            return _mm_setzero_si128();
        uint32_t shift = sa - 128;
        if (shift == 0)
            return rs;
        // Shift rs right by (sa-128) bits
        uint32_t byteShift = shift / 8;
        uint32_t bitShift = shift % 8;
        // Byte shift rs right
        alignas(16) uint8_t buf[16] = {};
        alignas(16) uint8_t src[16];
        _mm_store_si128((__m128i *)src, rs);
        for (uint32_t i = 0; i + byteShift < 16; i++)
            buf[i] = src[i + byteShift];
        __m128i result = _mm_load_si128((__m128i *)buf);
        if (bitShift > 0)
            result = _mm_or_si128(_mm_srli_epi64(result, bitShift),
                                  _mm_slli_epi64(_mm_bsrli_si128(result, 8), 64 - bitShift));
        return result;
    }
    // sa is 1..127: result = (rs || rt) >> sa, lower 128 bits
    uint32_t byteShift = sa / 8;
    uint32_t bitShift = sa % 8;
    alignas(16) uint8_t combined[32];
    _mm_store_si128((__m128i *)(combined), rt);      // low 128 bits
    _mm_store_si128((__m128i *)(combined + 16), rs); // high 128 bits
    // Shift right by byteShift bytes
    alignas(16) uint8_t shifted[16];
    for (uint32_t i = 0; i < 16; i++)
        shifted[i] = (i + byteShift < 32) ? combined[i + byteShift] : 0;
    __m128i result = _mm_load_si128((__m128i *)shifted);
    if (bitShift > 0)
    {
        uint8_t extra = (byteShift + 16 < 32) ? combined[byteShift + 16] : 0;
        __m128i hi_byte = _mm_insert_epi8(_mm_setzero_si128(), extra, 15);
        alignas(16) uint8_t src32[32];
        for (uint32_t i = 0; i < 32; i++)
            src32[i] = combined[i];
        uint64_t lo0, lo1, hi0, hi1;
        std::memcpy(&lo0, src32, 8);
        std::memcpy(&lo1, src32 + 8, 8);
        std::memcpy(&hi0, src32 + 16, 8);
        std::memcpy(&hi1, src32 + 24, 8);
        // 256-bit right shift by sa bits
        uint64_t r0, r1;
        if (sa < 64)
        {
            r0 = (lo0 >> sa) | (lo1 << (64 - sa));
            r1 = (lo1 >> sa) | (hi0 << (64 - sa));
        }
        else if (sa < 128)
        {
            uint32_t s = sa - 64;
            if (s == 0)
            {
                r0 = lo1;
                r1 = hi0;
            }
            else
            {
                r0 = (lo1 >> s) | (hi0 << (64 - s));
                r1 = (hi0 >> s) | (hi1 << (64 - s));
            }
        }
        else
        {
            r0 = 0;
            r1 = 0; // handled above
        }
        result = _mm_set_epi64x((long long)r1, (long long)r0);
    }
    return result;
}
#define PS2_QFSRV(rs, rt, sa) ps2_qfsrv((__m128i)(rs), (__m128i)(rt), (uint32_t)(sa))
#define PS2_PCPYLD(rs, rt) _mm_unpacklo_epi64(rt, rs)
// EE Core manual / PCSX2 MMI.cpp: PEXEH h0<->h2 in each doubleword; PEXEW w0<->w2; PROT3W rotates w0..w2 left (w3 kept).
#define PS2_PEXEH(rs) _mm_shufflelo_epi16(_mm_shufflehi_epi16(rs, _MM_SHUFFLE(3, 0, 1, 2)), _MM_SHUFFLE(3, 0, 1, 2))
#define PS2_PEXEW(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(3, 0, 1, 2))
#define PS2_PROT3W(rs) _mm_shuffle_epi32(rs, _MM_SHUFFLE(3, 0, 2, 1))

// Additional VU0 operations
#define PS2_VSQRT(x) sqrtf(x)
#define PS2_VRSQRT(x) (1.0f / sqrtf(x))

#define GPR_U32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0U : static_cast<uint32_t>(PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx])))
#define GPR_S32(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0 : PS2_EXTRACT_EPI32_0(ctx_ptr->r[reg_idx]))
#define GPR_U64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0ULL : static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx])))
#define GPR_S64(ctx_ptr, reg_idx) ((reg_idx == 0) ? 0LL : PS2_EXTRACT_EPI64_0(ctx_ptr->r[reg_idx]))
#define GPR_VEC(ctx_ptr, reg_idx) ((reg_idx == 0) ? _mm_setzero_si128() : ctx_ptr->r[reg_idx])

static PS2_FORCEINLINE void Ps2SetGprScalar64(R5900Context *ctx, int reg, int64_t value)
{
    if (reg != 0)
    {
        std::memcpy(&ctx->r[reg], &value, sizeof(value));
    }
}

static PS2_FORCEINLINE void Ps2SetGprLow64(R5900Context *ctx, int reg, __m128i new_low)
{
    if (reg != 0)
    {
        ctx->r[reg] = _mm_castpd_si128(_mm_move_sd(_mm_castsi128_pd(ctx->r[reg]), _mm_castsi128_pd(new_low)));
    }
}

#define SET_GPR_U32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            Ps2SetGprScalar64(ctx_ptr, reg_idx, (int64_t)(int32_t)(val)); \
        }                                                                 \
    } while (0)


#define SET_GPR_ZE32(ctx_ptr, reg_idx, val)                               \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            Ps2SetGprScalar64(ctx_ptr, reg_idx, (int64_t)(uint32_t)(val)); \
        }                                                                 \
    } while (0)

#define SET_GPR_S32(ctx_ptr, reg_idx, val)                                \
    do                                                                    \
    {                                                                     \
        if ((reg_idx) != 0)                                               \
        {                                                                 \
            Ps2SetGprScalar64(ctx_ptr, reg_idx, (int64_t)(int32_t)(val)); \
        }                                                                 \
    } while (0)

#define SET_GPR_U64(ctx_ptr, reg_idx, val)                       \
    do                                                           \
    {                                                            \
        if ((reg_idx) != 0)                                      \
        {                                                        \
            Ps2SetGprScalar64(ctx_ptr, reg_idx, (int64_t)(val)); \
        }                                                        \
    } while (0)

#define SET_GPR_S64(ctx_ptr, reg_idx, val) SET_GPR_U64(ctx_ptr, reg_idx, val)

#define SET_GPR_VEC(ctx_ptr, reg_idx, val) \
    do                                     \
    {                                      \
        if (reg_idx != 0)                  \
            ctx_ptr->r[reg_idx] = (val);   \
    } while (0)

#endif // PS2_RUNTIME_MACROS_H
