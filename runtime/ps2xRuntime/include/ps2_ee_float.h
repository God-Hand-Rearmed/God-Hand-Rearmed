#pragma once

// The PS2 EE's single-precision arithmetic, bit for bit (the FPU's rules; VU0's macro ops follow the same ones).
//
// Adapted from GT3 Revisited's src/base/ee_float.h / ee_float.cpp (github.com/matt-pulsipher/GT3-revisited, MIT
// licence, Copyright (c) 2026 Matthew Pulsipher and the GT3 Revisited contributors;
// docs/licenses/gt3-revisited-MIT.txt). Their findings, in short:
//  - no denormals: an exponent-0 operand reads as (signed) zero, a result below the smallest normal is a signed zero;
//  - no Inf / NaN: exponent 255 is an ordinary exponent (values up to 0x7FFFFFFF, ~2 x FLT_MAX); a result above it
//    clamps to +-0x7FFFFFFF;
//  - add / sub chop; the adder aligns the smaller operand to one guard bit below the larger's last bit, no sticky bit
//    (an operand 25+ binary orders smaller is ignored);
//  - mul: radix-4 Booth rows summed by 4:2 compressors whose carry out of the low 16 columns is lost: the chopped
//    product or, now and then, one below it (only possible when the product's bits below its last place are < 2^16).
//    Not commutative in those cases: mul(a, b) is a * b with a = fs, b = ft;
//  - div / sqrt: a radix-2 SRT recurrence, 25 digits, never corrected: the chopped result or one above it (never above
//    an exact one); rsqrt = a / sqrt(|b|), the root first; x / 0 = +-max (sign = xor of signs), sqrt uses |x|;
//  - madd / msub: the product rounded first; a product that overflows is the result (+-max, negated for msub).
// They verified it with the disc's PS2-recorded demo runs: all 58 end on their saved times with this multiplier and
// divider, 7 with a plain chopped product and nearest divisions (the rules this runtime used before).
//
// Functions take and return raw bit patterns (uint32_t). Integer code only: the same bits on every host and compiler.

#include <atomic>
#include <bit>
#include <cstdint>
#include <cstring>

#if defined(_MSC_VER)
#define PS2EE_INLINE __forceinline
#else
#define PS2EE_INLINE inline __attribute__((always_inline))
#endif

namespace ps2ee
{
    constexpr uint32_t kSign = 0x80000000u;
    constexpr uint32_t kMax = 0x7FFFFFFFu; // the largest magnitude (exponent 255, full mantissa)

    // Out of line (ps2_ee_float.cpp): the multiplier's partial-product tree, the divider's quotient and root.
    uint32_t mulTree(uint32_t a, uint32_t b);
    bool mulOverflows(uint32_t a, uint32_t b); // whether mul(a, b) clamped to +-max
    uint32_t div(uint32_t a, uint32_t b);
    uint32_t sqrt(uint32_t a); // sqrt(|a|)

    // The multiplier's lost carry (0 / 1) for a round a (its mantissa's low 16 bits 0: 1.0, 0.5, 3.0 ...), by b's low
    // 16 bits: the product is then the chopped one minus k in its last place, where its bits 16-22 (23 with the top
    // bit) are 0 (ps2_ee_float.cpp).
    extern const uint64_t kRoundCarryBits[1024];
    PS2EE_INLINE uint32_t roundCarry(uint32_t b)
    {
        return uint32_t(kRoundCarryBits[(b & 0xFFFFu) >> 6] >> (b & 63u)) & 1u;
    }

    PS2EE_INLINE uint32_t neg(uint32_t a) { return a ^ kSign; }
    PS2EE_INLINE uint32_t abs(uint32_t a) { return a & ~kSign; }
    PS2EE_INLINE bool isZero(uint32_t a) { return (a & 0x7F800000u) == 0u; } // exponent 0 reads as zero

    // a + b: the smaller magnitude keeps one guard bit below the larger's last bit; the exact sum of the aligned values
    // is chopped.
    PS2EE_INLINE uint32_t add(uint32_t a, uint32_t b)
    {
        const uint32_t ea = (a >> 23) & 0xFFu, eb = (b >> 23) & 0xFFu;
        a = ea ? a : (a & kSign);
        b = eb ? b : (b & kSign);
        const bool swap = (b & ~kSign) > (a & ~kSign);
        const uint32_t x = swap ? b : a, y = swap ? a : b; // |x| >= |y|
        const uint32_t ex = swap ? eb : ea, ey = swap ? ea : eb;
        if (!ey)
            return ex ? x : (a & b & kSign);
        const uint32_t big = ((x & 0x7FFFFFu) | 0x800000u) << 1; // in half ulps of x
        const uint32_t d = ex - ey;
        const uint32_t small = d > 25 ? 0u : (((y & 0x7FFFFFu) | 0x800000u) << 1) >> d;
        const uint32_t sum = ((x ^ y) >> 31) ? big - small : big + small;
        if (!sum)
            return 0u;
        const int top = 31 - std::countl_zero(sum); // 0..25
        const int e = int(ex) + top - 24;
        const uint32_t m = top >= 23 ? (sum >> (top - 23)) : (sum << (23 - top));
        const uint32_t sign = x & kSign;
        if (e <= 0)
            return sign;
        if (e > 255)
            return sign | kMax;
        return sign | (uint32_t(e) << 23) | (m & 0x7FFFFFu);
    }

    PS2EE_INLINE uint32_t sub(uint32_t a, uint32_t b) { return add(a, neg(b)); }

    // a * b (a = fs, b = ft): one 64-bit integer multiply; the Booth tree only when its lost carry could show.
    PS2EE_INLINE uint32_t mul(uint32_t a, uint32_t b)
    {
        const uint32_t sign = (a ^ b) & kSign;
        const uint32_t ea = (a >> 23) & 0xFFu, eb = (b >> 23) & 0xFFu;
        if (!ea || !eb)
            return sign;
        const uint64_t p = uint64_t((a & 0x7FFFFFu) | 0x800000u) * uint64_t((b & 0x7FFFFFu) | 0x800000u); // exact
        const uint32_t hi = uint32_t(p >> 47);
        // (A round b, its low 16 bits 0, loses nothing: x * 1.0, x * 2.0 ... are the chopped product.)
        if ((p & ((uint64_t(1) << (23 + hi)) - 1)) < (uint64_t(1) << 16) && (b & 0xFFFFu) != 0u)
            return mulTree(a, b);
        const int32_t e = int32_t(ea + eb) - 127 + int32_t(hi);
        const uint32_t m = uint32_t(p >> (23 + hi)) & 0x7FFFFFu; // chopped
        if (e <= 0)
            return sign;
        if (e > 255)
            return sign | kMax;
        return sign | (uint32_t(e) << 23) | m;
    }

    // acc + b * c / acc - b * c: the product rounded first; an overflowed product (+-max) is the result. (A product of
    // exactly +-max that did not overflow is added as usual: mulOverflows tells them apart, out of line.)
    PS2EE_INLINE uint32_t madd(uint32_t acc, uint32_t b, uint32_t c)
    {
        const uint32_t p = mul(b, c);
        return (p & kMax) == kMax && mulOverflows(b, c) ? p : add(acc, p);
    }
    PS2EE_INLINE uint32_t msub(uint32_t acc, uint32_t b, uint32_t c)
    {
        const uint32_t p = mul(b, c);
        return (p & kMax) == kMax && mulOverflows(b, c) ? neg(p) : sub(acc, p);
    }

    // a / sqrt(|b|): the root, then the division (the same unit twice).
    PS2EE_INLINE uint32_t rsqrt(uint32_t a, uint32_t b) { return div(a, sqrt(b)); }

    // Float to int: truncates toward zero, saturates (cvt.w.s). Int to float: chopped to 24 bits (cvt.s.w).
    PS2EE_INLINE int32_t toInt(uint32_t a)
    {
        const uint32_t e = (a >> 23) & 0xFFu;
        if (e < 127)
            return 0;
        if (e >= 127 + 31)
            return (a & kSign) ? INT32_MIN : INT32_MAX;
        const uint32_t m = (a & 0x7FFFFFu) | 0x800000u;
        const uint32_t mag = e >= 150 ? m << (e - 150) : m >> (150 - e);
        return (a & kSign) ? int32_t(0u - mag) : int32_t(mag);
    }
    PS2EE_INLINE uint32_t fromInt(int32_t v)
    {
        if (v == 0)
            return 0u;
        const uint32_t sign = v < 0 ? kSign : 0u;
        const uint32_t mag = v < 0 ? 0u - uint32_t(v) : uint32_t(v);
        const int top = 31 - std::countl_zero(mag);
        const uint32_t m = top > 23 ? mag >> (top - 23) : mag << (23 - top);
        return sign | (uint32_t(127 + top) << 23) | (m & 0x7FFFFFu);
    }

    // The order of sign-magnitude values with exponent-0 values as zero (+0 == -0).
    PS2EE_INLINE int32_t key(uint32_t x)
    {
        if (!((x >> 23) & 0xFFu))
            return 0;
        const int32_t m = int32_t(x & ~kSign);
        return (x & kSign) ? -m : m;
    }
    PS2EE_INLINE uint32_t maxOf(uint32_t a, uint32_t b) { return key(a) >= key(b) ? a : b; } // (not max / min:
    PS2EE_INLINE uint32_t minOf(uint32_t a, uint32_t b) { return key(a) <= key(b) ? a : b; } //  windows.h macros)

    // Host float views of the same bits (the register file keeps floats; no conversion, just the bits).
    PS2EE_INLINE uint32_t bits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
    PS2EE_INLINE float fl(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
} // namespace ps2ee
