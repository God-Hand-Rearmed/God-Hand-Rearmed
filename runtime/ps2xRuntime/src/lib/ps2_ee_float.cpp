// The PS2 EE's multiplier tree and divider (ps2_ee_float.h). Adapted from GT3 Revisited's src/base/ee_float.cpp
// (github.com/matt-pulsipher/GT3-revisited, MIT licence, Copyright (c) 2026 Matthew Pulsipher and the GT3 Revisited
// contributors; docs/licenses/gt3-revisited-MIT.txt).

#include "ps2_ee_float.h"

#include <cmath>

namespace ps2ee
{
    // When a's mantissa's low 16 bits are 0 (a multiplicand with at most 8 significant bits: 1.0, 0.5, 3.0, 100.0 ...)
    // the lost carry depends on b's low 16 bits alone: 65536 bits, constant data.
    const uint64_t kRoundCarryBits[1024] = {
#include "ps2_ee_float_carries.inc"
    };

    namespace
    {
        // The EE's multiplier as measured: radix-4 Booth rows of a's mantissa by b's (a digit of -0 is a zero row; a
        // negative row is the one's complement, its +1 placed in the next row at the row's own column), four 4:2
        // compressors and a 3:2 in three levels (rows 0-3, 4-7, 8-11 and 12 alone), then a carry-propagate add of the
        // two vectors above column 16 only: the carry out of the low 16 columns is lost.
        inline void fullAdd(uint64_t a, uint64_t b, uint64_t c, uint64_t &s, uint64_t &k)
        {
            s = a ^ b ^ c;
            k = ((a & b) | (a & c) | (b & c)) << 1;
        }
        inline void compress42(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t &s, uint64_t &k)
        {
            uint64_t s1, c1;
            fullAdd(b, c, d, s1, c1);
            fullAdd(s1, a, c1, s, k);
        }
        // Row i of the radix-4 Booth recoding (digit -2*b1 + b0 + lo from y's bits 2i+1, 2i, 2i-1), low 32 bits: x
        // times the digit's size, complemented when the digit is negative (its +1 noted in plusOne at the row's
        // column).
        PS2EE_INLINE void boothRow(uint32_t x, uint32_t y, int i, uint32_t &row, uint32_t &plusOne)
        {
            const uint32_t lo = i ? (y >> (2 * i - 1)) & 1 : 0;
            const uint32_t b0 = (y >> (2 * i)) & 1, b1 = (y >> (2 * i + 1)) & 1;
            const uint32_t neg = b1 & ~(b0 & lo) & 1;
            const uint32_t mag = b1 ? 2 - b0 - lo : b0 + lo; // 0, 1 or 2 (-0 when b1, b0 and lo are all 1)
            const uint32_t m = (x << (mag >> 1)) & (0u - uint32_t(mag != 0));
            row = (m ^ (0u - neg)) << (2 * i);
            plusOne |= neg << (2 * i);
        }
        uint32_t lowCarry(uint32_t x32, uint32_t y32)
        {
            // The tree's two vectors s, c sum to the exact product; dropping their low 16 columns loses the product's
            // low 16 bits and k, the carry out of those columns. Carries only move up, so k needs the low 16 columns
            // of rows 0-7 (row 8 holds row 7's +1 at column 14; later rows start at column 16): 32-bit words, no
            // branches (the digits are random). Same result as the full 13-row, 48-bit tree, about three times faster.
            uint32_t r[9];
            uint32_t plusOne = 0;
            // Written out row by row (unrolled on every compiler: the shifts become constants).
            boothRow(x32, y32, 0, r[0], plusOne);
            boothRow(x32, y32, 1, r[1], plusOne);
            boothRow(x32, y32, 2, r[2], plusOne);
            boothRow(x32, y32, 3, r[3], plusOne);
            boothRow(x32, y32, 4, r[4], plusOne);
            boothRow(x32, y32, 5, r[5], plusOne);
            boothRow(x32, y32, 6, r[6], plusOne);
            boothRow(x32, y32, 7, r[7], plusOne);
            r[8] = 0;
            for (int i = 0; i < 8; ++i)
                r[i + 1] |= plusOne & (1u << (2 * i));
            uint64_t s0, c0, s1, c1, s2, c2, s3, c3, s4, c4, s, c;
            compress42(r[0], r[1], r[2], r[3], s0, c0);
            compress42(r[4], r[5], r[6], r[7], s1, c1);
            compress42(r[8], 0, 0, 0, s2, c2);
            compress42(s0, c0, s1, c1, s3, c3);
            fullAdd(s2, c2, 0, s4, c4);
            compress42(s3, c3, s4, c4, s, c);
            return uint32_t(((s & 0xFFFF) + (c & 0xFFFF)) >> 16);
        }

        uint64_t boothTreeProduct(uint64_t x, uint64_t y)
        {
            const uint32_t x32 = uint32_t(x), y32 = uint32_t(y);
            // y's low 16 bits 0 (a round multiplier): every low row is 0, nothing is lost.
            const uint32_t k = (y32 & 0xFFFF) == 0 ? 0u
                               : (x32 & 0xFFFF) == 0 ? roundCarry(y32)
                                                     : lowCarry(x32, y32);
            constexpr uint64_t kMask = (uint64_t(1) << 48) - 1;
            return (((x * y) & ~uint64_t(0xFFFF)) - (uint64_t(k) << 16)) & kMask;
        }

        // The EE's divider as measured: radix-2 SRT on a carry-save remainder of 25 fraction bits; 25 digits, never
        // corrected (the top 24 bits are the result). Each digit's three possible remainders are formed and the digit
        // picks one with masks (no branches: the digits are random).
        uint32_t srtQuotient(uint32_t x, uint32_t y)
        {
            const uint64_t d = uint64_t(y) << 1, nd = ~d;
            uint64_t s = x, c = 0, vs = s, vc = 0;
            int64_t q = 0;
            for (int j = 0; j < 25; ++j)
            {
                const uint64_t s2 = s << 1, c2 = c << 1, x2 = s2 ^ c2, a2 = s2 & c2;
                const uint32_t top = uint32_t((vs >> 23) + (vc >> 23)) & 7, low = uint32_t((vs | vc) >> 22) & 1;
                const uint64_t zero = 0 - uint64_t((0xC001u >> (top * 2 + low)) & 1), negative = 0 - uint64_t(top >> 2);
                const uint64_t plus = ~zero & ~negative, minus = ~zero & negative;
                s = (x2 & zero) | ((x2 ^ nd) & plus) | ((x2 ^ d) & minus);
                c = ((a2 << 1) & zero) | ((((a2 | (x2 & nd)) << 1) | 1) & plus) | (((a2 | (x2 & d)) << 1) & minus);
                vs = (s2 & zero) | (s & ~zero), vc = (c2 & zero) | (c & ~zero);
                q = q * 2 + int64_t(plus & 1) - int64_t(minus & 1);
            }
            return uint32_t(uint64_t(q) >> ((q >> 24) ? 1 : 0)) & 0xFFFFFFu;
        }

        // The same unit's square root: 25 digits of the same selection, uncorrected; the top 24 bits are the result.
        uint32_t srtRoot(uint32_t x, bool odd)
        {
            constexpr uint64_t kAdder = ~uint64_t(1);
            uint64_t s = uint64_t(x) << int(odd), c = 0, vs = s, vc = 0, root = 0;
            for (int j = 0; j < 25; ++j)
            {
                const uint64_t s2 = s << 1, c2 = c << 1, x2 = s2 ^ c2, a2 = s2 & c2;
                const uint64_t step = uint64_t(1) << (24 - j);
                const uint64_t mp = ~(root + step) & kAdder, mn = (root - step) & kAdder;
                const uint32_t top = uint32_t((vs >> 24) + (vc >> 24)) & 7, low = uint32_t((vs | vc) >> 23) & 1;
                const uint64_t zero = 0 - uint64_t((0xC001u >> (top * 2 + low)) & 1), negative = 0 - uint64_t(top >> 2);
                const uint64_t plus = ~zero & ~negative, minus = ~zero & negative;
                s = (x2 & zero) | ((x2 ^ mp) & plus) | ((x2 ^ mn) & minus);
                c = ((a2 << 1) & zero) | ((((a2 | (x2 & mp)) << 1) | 2) & plus) | (((a2 | (x2 & mn)) << 1) & minus);
                vs = (s2 & zero) | (s & ~zero), vc = (c2 & zero) | (c & ~zero);
                root += (2 * step) & plus, root -= (2 * step) & minus;
            }
            return uint32_t(root >> 2) & 0xFFFFFFu;
        }

        // The last quotients and roots by mantissa (direct-mapped; one word per entry, relaxed atomics). 2^kMemoBits
        // entries each: 1024 (GT3's) hit 56% of God Hand's quotients and 46% of its roots in K's replays of 5d766f0;
        // 16K raised that to 62% / 54% but the game thread came out busier (2ce7d39, L2 pressure), so 1024 again.
        // Since the default rules use the host's divider (ps2_runtime_macros.h), only special operands and
        // PS2_EXACT_EE_FLOAT builds get here. A quotient's slot holds x's low
        // kMemoBits bits (slot ^ hash(y) gives them back), its key the rest of x, y and a valid bit: 1 + (23 -
        // kMemoBits) + 23 bits, shifted past the 23-bit result, so kMemoBits >= 6. A root's key is the whole 24-bit
        // input + the valid bit at 40 (the slot is only a hash).
        constexpr uint32_t kMemoBits = 10, kMemoSlots = 1u << kMemoBits;
        static_assert(kMemoBits >= 6 && kMemoBits <= 22);
        class Memo
        {
        public:
            bool find(uint32_t slot, uint64_t key, uint32_t &r) const
            {
                const uint64_t e = m_entries[slot].load(std::memory_order_relaxed);
                r = uint32_t(e) & 0x7FFFFFu;
                return (e >> 23) == key;
            }
            void put(uint32_t slot, uint64_t key, uint32_t r)
            {
                m_entries[slot].store(key << 23 | (r & 0x7FFFFFu), std::memory_order_relaxed);
            }

        private:
            alignas(64) std::atomic<uint64_t> m_entries[kMemoSlots] = {};
        };
        constinit Memo g_quotients, g_roots;

        uint32_t quotient(uint32_t x, uint32_t y) // 24-bit mantissas
        {
            const uint32_t x23 = x & 0x7FFFFFu, y23 = y & 0x7FFFFFu;
            const uint32_t slot = (x23 ^ (y23 * 0x9E3779B1u >> (32 - kMemoBits))) & (kMemoSlots - 1);
            const uint64_t key = uint64_t(1) << (46 - kMemoBits) | uint64_t(x23 >> kMemoBits) << 23 | y23;
            uint32_t r;
            if (g_quotients.find(slot, key, r))
                return r | 0x800000u;
            // The recurrence is the chopped quotient where it cannot end above it: an exact one, and for x >= y one
            // whose dropped half (the 25th digit) is under 1/2.
            const bool big = x >= y;
            const uint64_t n = uint64_t(x) << (big ? 23 : 24), chop = n / y, rem = n - chop * y;
            r = rem == 0 || (big && 2 * rem < y) ? uint32_t(chop) : srtQuotient(x, y);
            g_quotients.put(slot, key, r);
            return r;
        }

        uint32_t root(uint32_t x, bool odd)
        {
            const uint32_t k = (x & 0x7FFFFFu) | uint32_t(odd) << 23, slot = (k * 0x9E3779B1u) >> (32 - kMemoBits);
            const uint64_t key = uint64_t(1) << 40 | k;
            uint32_t r;
            if (g_roots.find(slot, key, r))
                return r | 0x800000u;
            // The chopped root wherever the exact root's 25th bit is 0.
            const uint64_t n = uint64_t(x) << (27 + int(odd));
            uint64_t g = uint64_t(std::sqrt(double(n)));
            while (g * g > n)
                --g;
            while ((g + 1) * (g + 1) <= n)
                ++g;
            r = (g & 2) == 0 ? uint32_t(g >> 2) : srtRoot(x, odd);
            g_roots.put(slot, key, r);
            return r;
        }
    } // namespace

    namespace
    {
        // The multiplier's product of two normal operands' mantissas and the result's exponent (before clamping).
        uint64_t treeProduct(uint32_t a, uint32_t b, int32_t &e)
        {
            const uint32_t ea = (a >> 23) & 0xFFu, eb = (b >> 23) & 0xFFu;
            const uint64_t x = (a & 0x7FFFFFu) | 0x800000u, y = (b & 0x7FFFFFu) | 0x800000u;
            uint64_t p = x * y;
            const uint32_t hi = uint32_t(p >> 47) & 1;
            if ((p & ((uint64_t(1) << (23 + hi)) - 1)) < (uint64_t(1) << 16))
                p = boothTreeProduct(x, y);
            e = int32_t(ea + eb) - 127 + int32_t(uint32_t(p >> 47) & 1);
            return p;
        }
    } // namespace

    uint32_t mulTree(uint32_t a, uint32_t b)
    {
        const uint32_t sign = (a ^ b) & kSign;
        if (!((a >> 23) & 0xFFu) || !((b >> 23) & 0xFFu))
            return sign;
        int32_t e;
        const uint64_t p = treeProduct(a, b, e);
        if (e <= 0)
            return sign;
        if (e > 255)
            return sign | kMax;
        const uint32_t top = uint32_t(p >> 47) & 1;
        return sign | (uint32_t(e) << 23) | (uint32_t(p >> (23 + top)) & 0x7FFFFFu);
    }

    bool mulOverflows(uint32_t a, uint32_t b)
    {
        if (!((a >> 23) & 0xFFu) || !((b >> 23) & 0xFFu))
            return false;
        int32_t e;
        treeProduct(a, b, e);
        return e > 255;
    }

    uint32_t div(uint32_t a, uint32_t b)
    {
        const uint32_t sign = (a ^ b) & kSign;
        const uint32_t ea = (a >> 23) & 0xFFu, eb = (b >> 23) & 0xFFu;
        if (!eb)
            return sign | kMax;
        if (!ea)
            return sign;
        const uint32_t x = (a & 0x7FFFFFu) | 0x800000u, y = (b & 0x7FFFFFu) | 0x800000u;
        const int32_t e = int32_t(ea) - int32_t(eb) + 126 + int32_t(x >= y);
        if (e <= 0)
            return sign;
        if (e > 255)
            return sign | kMax;
        return sign | (uint32_t(e) << 23) | (quotient(x, y) & 0x7FFFFFu);
    }

    uint32_t sqrt(uint32_t a)
    {
        const uint32_t e = (a >> 23) & 0xFFu;
        if (!e)
            return 0u;
        return ((e + 127) >> 1) << 23 | (root((a & 0x7FFFFFu) | 0x800000u, !(e & 1)) & 0x7FFFFFu);
    }
} // namespace ps2ee
