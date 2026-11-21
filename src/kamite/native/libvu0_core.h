#pragma once

// The libvu0 helpers' maths on plain Vec4s (portable: SSE, NEON, plain C++), shared by the natives (libvu0.cpp, which
// adds the guest context) and the offline checks (tools/native). Each op is the recompiled code's op in its order
// under the default float rules (claude/gt3-revisited.md "How ours works"): VMUL = the host product, VMADD = the host
// product rounded, then the host sum (no FMA); results are the recompiled code's bit for bit as long as nothing
// reaches FLT_MAX - 1 ulp, which the natives' entry checks rule out (finiteInputs).

#include "kamite/native/vu_vec.h"

namespace kamite::native::libvu0
{
    // |x| < 2^62 in every lane: products of two such values stay below 2^124 and sums of four below 2^126, far from
    // FLT_MAX, so the recompiled ops never take their exact-lane path (and no exponent-255 operand is let in).
    inline bool finiteInputs(Vec4 v) { return magnitudesBelow(v, 0x5E800000u); } // 2^62

    // vmulax / vmadday / vmaddaz (ACC) then vmaddw (the result): one row of a matrix product, b's lanes weighting
    // a0..a3. acc = ACC after the third step (what the original leaves in ACC).
    struct RowResult
    {
        Vec4 acc;
        Vec4 r;
    };
    inline RowResult mulRow(Vec4 a0, Vec4 a1, Vec4 a2, Vec4 a3, Vec4 b)
    {
        Vec4 acc = mul(a0, splat<0>(b));
        acc = add(acc, mul(a1, splat<1>(b)));
        acc = add(acc, mul(a2, splat<2>(b)));
        const Vec4 r = add(acc, mul(a3, splat<3>(b)));
        return RowResult{acc, r};
    }

    // VSQRT into Q (ps2VuSqrt under the default rules): sqrt(|a|), the host's root for exponents 1-254, 0 for exponent
    // 0; the status register's I / D cleared, I + IS set for a negative non-zero operand. (Exponent 255 never reaches
    // it here: the callers' entry checks bound the operand.)
    inline float vuSqrt(uint16_t &status, float a)
    {
        const uint32_t bits = floatBits(a);
        status = static_cast<uint16_t>(status & ~0x0030u);
        const bool zero = (bits & 0x7F800000u) == 0u;
        if ((bits & 0x80000000u) && !zero)
            status = static_cast<uint16_t>(status | 0x0010u | 0x0400u);
        return zero ? 0.0f : sqrtScalar(bitsFloat(bits & 0x7FFFFFFFu));
    }

    // _sceVu0ecossin 0x30A5D0: vf6.x = the angle (already pi/2 -+ the caller's), r7 = the sign choice; leaves cos /
    // sin in vf4.x / vf4.y (the polynomial on 0x3C3C90's coefficients, then the root). Every op of the original in its
    // order; vf4.yzw * 0, vf5 = 0, vf6.w / vf7.w / vf8 as the original leaves them.
    struct VuRegs
    {
        Vec4 vf4, vf5, vf6, vf7, vf8;
        float q;
        uint16_t status;
    };
    inline void ecossin(Vec4 vf0, Vec4 coefficients, bool negative, VuRegs &v)
    {
        v.vf5 = coefficients;
        v.vf6 = blend<8>(v.vf6, rotateLanes(v.vf6));
        v.vf4 = blend<1>(v.vf4, add(vf0, splat<0>(v.vf6)));
        v.vf6 = blend<1>(v.vf6, mul(v.vf6, v.vf6));
        v.vf4 = blend<14>(v.vf4, mul(v.vf4, splat<0>(vf0)));
        v.vf8 = mul(v.vf5, splat<3>(v.vf6));
        v.vf5 = sub(vf0, vf0);
        v.vf8 = mul(v.vf8, splat<0>(v.vf6));
        v.vf8 = blend<7>(v.vf8, mul(v.vf8, splat<0>(v.vf6)));
        v.vf4 = blend<1>(v.vf4, add(v.vf4, splat<3>(v.vf8)));
        v.vf8 = blend<3>(v.vf8, mul(v.vf8, splat<0>(v.vf6)));
        v.vf4 = blend<1>(v.vf4, add(v.vf4, splat<2>(v.vf8)));
        v.vf8 = blend<1>(v.vf8, mul(v.vf8, splat<0>(v.vf6)));
        v.vf4 = blend<1>(v.vf4, add(v.vf4, splat<1>(v.vf8)));
        v.vf4 = blend<1>(v.vf4, add(v.vf4, splat<0>(v.vf8)));
        v.vf4 = blend<3>(v.vf4, add(v.vf5, splat<0>(v.vf4)));
        v.vf7 = blend<1>(v.vf7, mul(v.vf4, v.vf4));
        v.vf7 = blend<8>(v.vf7, sub(vf0, splat<0>(v.vf7)));
        v.q = vuSqrt(v.status, lane<3>(v.vf7));
        v.vf7 = blend<1>(v.vf7, add(vf0, splat(v.q)));
        v.vf4 = blend<1>(v.vf4, negative ? sub(v.vf5, splat<0>(v.vf7)) : add(v.vf5, splat<0>(v.vf7)));
    }

    // sceVu0RotMatrixX / Y / Z after the ecossin call: the rotation rows vf6-vf9 from vf5 (= 0) and vf4.xy (cos, sin),
    // vf4.zw cleared, as each original builds them (Axis 0 = X, 1 = Y, 2 = Z).
    struct RotationRows
    {
        Vec4 r0, r1, r2, r3;
    };
    template <int Axis>
    inline RotationRows rotationRows(Vec4 vf0, Vec4 vf5, Vec4 &vf4)
    {
        RotationRows m{vf5, vf5, vf5, vf5};
        if constexpr (Axis == 2)
        {
            m.r3 = blend<7>(vf0, sub(vf0, vf0));
            m.r2 = rotateLanes(m.r3);
        }
        else if constexpr (Axis == 0)
        {
            m.r0 = blend<1>(m.r0, add(vf5, splat<3>(vf0)));
            m.r3 = blend<8>(m.r3, add(vf5, splat<3>(vf0)));
        }
        else
        {
            m.r1 = blend<2>(m.r1, add(vf5, splat<3>(vf0)));
            m.r3 = blend<8>(m.r3, add(vf5, splat<3>(vf0)));
        }
        vf4 = blend<12>(vf4, sub(vf4, vf4));
        if constexpr (Axis == 2)
        {
            m.r0 = blend<2>(m.r0, add(vf5, splat<0>(vf4)));
            m.r0 = blend<1>(m.r0, add(vf5, splat<1>(vf4)));
            m.r1 = blend<1>(m.r1, sub(vf5, splat<0>(vf4)));
            m.r1 = blend<2>(m.r1, add(vf5, splat<1>(vf4)));
        }
        else if constexpr (Axis == 0)
        {
            m.r1 = blend<4>(m.r1, add(vf5, splat<0>(vf4)));
            m.r1 = blend<2>(m.r1, add(vf5, splat<1>(vf4)));
            m.r2 = blend<2>(m.r2, sub(vf5, splat<0>(vf4)));
            m.r2 = blend<4>(m.r2, add(vf5, splat<1>(vf4)));
        }
        else
        {
            m.r0 = blend<4>(m.r0, sub(vf5, splat<0>(vf4)));
            m.r0 = blend<1>(m.r0, add(vf5, splat<1>(vf4)));
            m.r2 = blend<1>(m.r2, add(vf5, splat<0>(vf4)));
            m.r2 = blend<4>(m.r2, add(vf5, splat<1>(vf4)));
        }
        return m;
    }

    // ---- rung 3 (step a, batch 1): the rest of libvu0 that the character path calls --------------------------------

    // xyz below 2^62 (w ignored: the ops below compute it, but only xyz reaches a result).
    inline bool finiteXyz(Vec4 v) { return magnitudesBelow(blend<8>(v, splat(0.0f)), 0x5E800000u); }
    inline bool exponent255(uint32_t bits) { return (bits & 0x7F800000u) == 0x7F800000u; }

    // VDIV into Q under the default rules for an ordinary divisor (exponent 1-254) and a result below FLT_MAX (the
    // callers' checks): the host's quotient; I / D cleared.
    inline float vuDiv(uint16_t &status, float a, float b)
    {
        status = static_cast<uint16_t>(status & ~0x0030u);
        return divScalar(a, b);
    }

    // sceVu0OuterProduct 0x30A328: ACC.xyz = a.yzx * b.zxy; r.xyz = ACC - b.yzx * a.zxy (the product rounded first).
    struct OuterResult
    {
        Vec4 acc; // xyz only (the caller keeps ACC.w)
        Vec4 r;   // xyz only
    };
    inline OuterResult outerProduct(Vec4 a, Vec4 b)
    {
        const Vec4 acc = mul(shuffle<1, 2, 0, 3>(a), shuffle<2, 0, 1, 3>(b));
        const Vec4 r = sub(acc, mul(shuffle<1, 2, 0, 3>(b), shuffle<2, 0, 1, 3>(a)));
        return OuterResult{acc, r};
    }

    // x*x + y*y + z*z in lane x as the libvu0 code sums it (vmul.xyz, vaddy.x, vaddz.x); the other lanes of `into`
    // as the original leaves them (y, z squared, w kept).
    inline Vec4 squaresSummed(Vec4 v, Vec4 into)
    {
        Vec4 s = blend<7>(into, mul(v, v));
        s = blend<1>(s, add(s, splat<1>(s)));
        s = blend<1>(s, add(s, splat<2>(s)));
        return s;
    }
    // A sum of squares the divide unit takes on the host path (exponent 1-254).
    inline bool ordinaryLength(Vec4 summed) { return ((laneBits<0>(summed) >> 23) & 0xFFu) - 1u < 254u; }

    // sceVu0Normalize 0x30A370 after vf4 = v: vf5 = the sum and then Q's root, Q = 1 / root, vf6 = (v.xyz * Q, 0).
    struct NormalizeResult
    {
        Vec4 vf5, vf6;
        float q;
        uint16_t status;
    };
    inline NormalizeResult normalize(Vec4 vf0, Vec4 v, Vec4 vf5, uint16_t status)
    {
        NormalizeResult n;
        n.status = status;
        n.vf5 = squaresSummed(v, vf5);
        float q = vuSqrt(n.status, lane<0>(n.vf5));
        n.vf5 = blend<1>(n.vf5, add(vf0, splat(q)));
        q = vuDiv(n.status, lane<3>(vf0), lane<0>(n.vf5));
        n.vf6 = blend<7>(sub(vf0, vf0), mul(v, splat(q)));
        n.q = q;
        return n;
    }

    // capVu0Length 0x1F7DF8 (the game's own): vf4 = v, then the sum, Q = its root, vf4.x = Q.
    struct LengthResult
    {
        Vec4 vf4;
        float q;
        uint16_t status;
    };
    inline LengthResult length(Vec4 vf0, Vec4 v, uint16_t status)
    {
        LengthResult l;
        l.status = status;
        l.vf4 = squaresSummed(v, v);
        l.q = vuSqrt(l.status, lane<0>(l.vf4));
        l.vf4 = blend<1>(l.vf4, add(vf0, splat(l.q)));
        return l;
    }

    // The EE's 128-bit word shuffles on four 32-bit lanes (lane 0 = the low word): PEXTLW / PEXTUW rd, rs, rt and
    // PCPYLD / PCPYUD rd, rs, rt.
    struct Words
    {
        uint32_t w[4];
    };
    inline Words pextlw(Words rs, Words rt) { return Words{{rt.w[0], rs.w[0], rt.w[1], rs.w[1]}}; }
    inline Words pextuw(Words rs, Words rt) { return Words{{rt.w[2], rs.w[2], rt.w[3], rs.w[3]}}; }
    inline Words pcpyld(Words rs, Words rt) { return Words{{rt.w[0], rt.w[1], rs.w[0], rs.w[1]}}; }
    inline Words pcpyud(Words rs, Words rt) { return Words{{rs.w[2], rs.w[3], rt.w[2], rt.w[3]}}; }

    // sceVu0InversMatrix's float part: vf6-vf8 = the transposed rotation (its w lanes from row 3 with xyz cleared),
    // vf5 = row 3, vf9 = row 3 with xyz cleared; ACC.xyz = vf6 * vf5.x + vf7 * vf5.y; vf4.xyz = vf9 - (ACC + vf8 * vf5.z).
    struct InversResult
    {
        Vec4 acc, vf4;
    };
    inline InversResult inversRows(Vec4 v6, Vec4 v7, Vec4 v8, Vec4 v5, Vec4 v9, Vec4 acc)
    {
        acc = blend<7>(acc, mul(v6, splat<0>(v5)));
        acc = blend<7>(acc, add(acc, mul(v7, splat<1>(v5))));
        Vec4 v4 = blend<7>(v9, add(acc, mul(v8, splat<2>(v5))));
        v4 = blend<7>(v4, sub(v9, v4));
        return InversResult{acc, v4};
    }
}
