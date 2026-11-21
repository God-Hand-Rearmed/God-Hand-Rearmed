#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>

// Four floats with the VU's arithmetic: the one maths layer of all native code (the native VU1 programs in
// src/kamite/render, claude/native-vu1.md; native EE functions in src/kamite/native, claude/native-next.md rung 0: no
// raw SSE / NEON outside this file). Three paths with one
// API: SSE (x86), NEON (AArch64) and plain C++ (the reference; KAMITE_VU_SCALAR forces it, to diff it against the other
// two). All of them leave rounding to the float environment (VuFloatEnv below): the VU's round toward zero, with
// denormal inputs and results flushed to zero. In that environment an overflow saturates to +-FLT_MAX (round toward
// zero never reaches infinity) and, with operands normalized (norm()), no operation makes an infinity or a NaN.
// Every operation is a separate rounding step, as on the VU: never contract a multiply and an add (no FMA; GCC/Clang
// build these files with -ffp-contract=off).
//
// Lane order is memory order: x, y, z, w (a VU quadword, x at the lowest address).

#if !defined(KAMITE_VU_SCALAR) && (defined(__SSE4_1__) || defined(_M_X64) || defined(__x86_64__))
#define KAMITE_VU_SSE 1
#include <smmintrin.h>
#elif !defined(KAMITE_VU_SCALAR) && (defined(__ARM_NEON) || defined(_M_ARM64))
#define KAMITE_VU_NEON 1
#include <arm_neon.h>
#else
#define KAMITE_VU_REF 1
#endif
#if defined(__x86_64__) || defined(_M_X64)
#include <xmmintrin.h>
#elif !defined(__aarch64__)
#include <cfenv>
#endif

namespace kamite::native
{
    // The status register's sticky zero / sign / underflow / overflow bits (ZS SS US OS): a native run sets all four.
    // No microprogram of the game reads them (no FSAND / FSOR / FSEQ / FSSET in any image) and the EE cannot read VU1's
    // status, so tracking every FMAC's flags exactly would cost time for nothing; the AOT sets them too, only later
    // (underflow / overflow need rare geometry: ~20 s into 1-1 in K's Nov 20 profile).
    constexpr uint32_t kStatusStickyFlags = 0x3C0u;

#if defined(KAMITE_VU_SSE)
    struct Vec4
    {
        __m128 v;
    };
    inline Vec4 load(const void *p) { return {_mm_loadu_ps(static_cast<const float *>(p))}; }
    inline void store(void *p, Vec4 a) { _mm_storeu_ps(static_cast<float *>(p), a.v); }
    inline Vec4 bitsOf(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
    {
        return {_mm_castsi128_ps(_mm_setr_epi32(static_cast<int>(x), static_cast<int>(y), static_cast<int>(z), static_cast<int>(w)))};
    }
    inline Vec4 splat(float f) { return {_mm_set1_ps(f)}; }
    template <int L>
    inline Vec4 splat(Vec4 a) { return {_mm_shuffle_ps(a.v, a.v, _MM_SHUFFLE(L, L, L, L))}; }
    template <int L>
    inline float lane(Vec4 a)
    {
        if constexpr (L == 0)
            return _mm_cvtss_f32(a.v);
        else
            return _mm_cvtss_f32(_mm_shuffle_ps(a.v, a.v, _MM_SHUFFLE(L, L, L, L)));
    }
    template <int L>
    inline uint32_t laneBits(Vec4 a) { return static_cast<uint32_t>(_mm_extract_ps(a.v, L)); }
    // Every lane's magnitude bits (sign cleared) below limit (limit <= 0x7F800000): an entry check for huge values.
    inline bool magnitudesBelow(Vec4 a, uint32_t limit)
    {
        const __m128i m = _mm_and_si128(_mm_castps_si128(a.v), _mm_set1_epi32(0x7FFFFFFF));
        return _mm_movemask_ps(_mm_castsi128_ps(_mm_cmplt_epi32(m, _mm_set1_epi32(static_cast<int>(limit))))) == 0xF;
    }
    inline Vec4 add(Vec4 a, Vec4 b) { return {_mm_add_ps(a.v, b.v)}; }
    inline Vec4 sub(Vec4 a, Vec4 b) { return {_mm_sub_ps(a.v, b.v)}; }
    inline Vec4 mul(Vec4 a, Vec4 b) { return {_mm_mul_ps(a.v, b.v)}; }
    // The VU's MINI / MAX: a < b ? a : b and a > b ? a : b (so +0 / -0 give b), as minps / maxps do.
    inline Vec4 vmin(Vec4 a, Vec4 b) { return {_mm_min_ps(a.v, b.v)}; }
    inline Vec4 vmax(Vec4 a, Vec4 b) { return {_mm_max_ps(a.v, b.v)}; }
    // Lanes of b where Mask has the bit (bit 0 = x ... bit 3 = w), else a.
    template <int Mask>
    inline Vec4 blend(Vec4 a, Vec4 b) { return {_mm_blend_ps(a.v, b.v, Mask)}; }
    // VMR32: every lane takes the next one's value (x <- y, y <- z, z <- w, w <- x).
    inline Vec4 rotateLanes(Vec4 a) { return {_mm_shuffle_ps(a.v, a.v, _MM_SHUFFLE(0, 3, 2, 1))}; }
    // r = (a[X], a[Y], a[Z], a[W]): VOPMULA / VOPMSUB's yzx / zxy forms.
    template <int X, int Y, int Z, int W>
    inline Vec4 shuffle(Vec4 a) { return {_mm_shuffle_ps(a.v, a.v, _MM_SHUFFLE(W, Z, Y, X))}; }
    // A VU operand: exponent 0 -> signed zero, exponent 255 -> signed FLT_MAX.
    inline Vec4 norm(Vec4 a)
    {
        const __m128i bits = _mm_castps_si128(a.v);
        const __m128i exponent = _mm_and_si128(bits, _mm_set1_epi32(0x7F800000));
        const __m128 sign = _mm_castsi128_ps(_mm_and_si128(bits, _mm_set1_epi32(static_cast<int>(0x80000000u))));
        const __m128 zero = _mm_castsi128_ps(_mm_cmpeq_epi32(exponent, _mm_setzero_si128()));
        const __m128 huge = _mm_castsi128_ps(_mm_cmpeq_epi32(exponent, _mm_set1_epi32(0x7F800000)));
        const __m128 result = _mm_blendv_ps(a.v, sign, zero);
        return {_mm_blendv_ps(result, _mm_or_ps(sign, _mm_castsi128_ps(_mm_set1_epi32(0x7F7FFFFF))), huge)};
    }
    // ITOF0: the lanes' bits as int32, converted (rounded by the float environment).
    inline Vec4 itof(Vec4 a) { return {_mm_cvtepi32_ps(_mm_castps_si128(a.v))}; }
    // FTOI0 of a normalized value: truncated, saturating (2^31 and up -> 0x7FFFFFFF, below -2^31 -> 0x80000000).
    inline Vec4 ftoi(Vec4 a)
    {
        const __m128i truncated = _mm_cvttps_epi32(a.v);
        const __m128i big = _mm_castps_si128(_mm_cmpge_ps(a.v, _mm_set1_ps(2147483648.0f)));
        return {_mm_castsi128_ps(_mm_or_si128(_mm_andnot_si128(big, truncated), _mm_and_si128(big, _mm_set1_epi32(0x7FFFFFFF))))};
    }

#elif defined(KAMITE_VU_NEON)
    struct Vec4
    {
        float32x4_t v;
    };
    inline Vec4 load(const void *p) { return {vld1q_f32(static_cast<const float *>(p))}; }
    inline void store(void *p, Vec4 a) { vst1q_f32(static_cast<float *>(p), a.v); }
    inline Vec4 bitsOf(uint32_t x, uint32_t y, uint32_t z, uint32_t w)
    {
        const uint32_t b[4] = {x, y, z, w};
        return {vreinterpretq_f32_u32(vld1q_u32(b))};
    }
    inline Vec4 splat(float f) { return {vdupq_n_f32(f)}; }
    template <int L>
    inline Vec4 splat(Vec4 a) { return {vdupq_laneq_f32(a.v, L)}; }
    template <int L>
    inline float lane(Vec4 a) { return vgetq_lane_f32(a.v, L); }
    template <int L>
    inline uint32_t laneBits(Vec4 a) { return vgetq_lane_u32(vreinterpretq_u32_f32(a.v), L); }
    inline bool magnitudesBelow(Vec4 a, uint32_t limit)
    {
        const uint32x4_t m = vandq_u32(vreinterpretq_u32_f32(a.v), vdupq_n_u32(0x7FFFFFFFu));
        return vminvq_u32(vcltq_u32(m, vdupq_n_u32(limit))) != 0u;
    }
    inline Vec4 add(Vec4 a, Vec4 b) { return {vaddq_f32(a.v, b.v)}; }
    inline Vec4 sub(Vec4 a, Vec4 b) { return {vsubq_f32(a.v, b.v)}; }
    inline Vec4 mul(Vec4 a, Vec4 b) { return {vmulq_f32(a.v, b.v)}; }
    // Not vminq/vmaxq: those order -0 below +0, the VU's compare does not.
    inline Vec4 vmin(Vec4 a, Vec4 b) { return {vbslq_f32(vcltq_f32(a.v, b.v), a.v, b.v)}; }
    inline Vec4 vmax(Vec4 a, Vec4 b) { return {vbslq_f32(vcgtq_f32(a.v, b.v), a.v, b.v)}; }
    template <int Mask>
    inline Vec4 blend(Vec4 a, Vec4 b)
    {
        const uint32_t m[4] = {(Mask & 1) ? ~0u : 0u, (Mask & 2) ? ~0u : 0u, (Mask & 4) ? ~0u : 0u, (Mask & 8) ? ~0u : 0u};
        return {vbslq_f32(vld1q_u32(m), b.v, a.v)};
    }
    inline Vec4 rotateLanes(Vec4 a) { return {vextq_f32(a.v, a.v, 1)}; }
    template <int X, int Y, int Z, int W>
    inline Vec4 shuffle(Vec4 a)
    {
        float in[4], out[4];
        vst1q_f32(in, a.v);
        out[0] = in[X], out[1] = in[Y], out[2] = in[Z], out[3] = in[W];
        return {vld1q_f32(out)};
    }
    inline Vec4 norm(Vec4 a)
    {
        const uint32x4_t bits = vreinterpretq_u32_f32(a.v);
        const uint32x4_t exponent = vandq_u32(bits, vdupq_n_u32(0x7F800000u));
        const uint32x4_t sign = vandq_u32(bits, vdupq_n_u32(0x80000000u));
        uint32x4_t r = vbslq_u32(vceqq_u32(exponent, vdupq_n_u32(0u)), sign, bits);
        r = vbslq_u32(vceqq_u32(exponent, vdupq_n_u32(0x7F800000u)), vorrq_u32(sign, vdupq_n_u32(0x7F7FFFFFu)), r);
        return {vreinterpretq_f32_u32(r)};
    }
    inline Vec4 itof(Vec4 a) { return {vcvtq_f32_s32(vreinterpretq_s32_f32(a.v))}; }
    // FCVTZS saturates both ways, as the VU does.
    inline Vec4 ftoi(Vec4 a) { return {vreinterpretq_f32_s32(vcvtq_s32_f32(a.v))}; }

#else
    struct Vec4
    {
        float v[4];
    };
    inline Vec4 load(const void *p)
    {
        Vec4 r;
        std::memcpy(r.v, p, 16);
        return r;
    }
    inline void store(void *p, Vec4 a) { std::memcpy(p, a.v, 16); }
    inline uint32_t bitsOfFloat(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
    inline float floatOfBits(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }
    inline Vec4 bitsOf(uint32_t x, uint32_t y, uint32_t z, uint32_t w) { return {{floatOfBits(x), floatOfBits(y), floatOfBits(z), floatOfBits(w)}}; }
    inline Vec4 splat(float f) { return {{f, f, f, f}}; }
    template <int L>
    inline Vec4 splat(Vec4 a) { return splat(a.v[L]); }
    template <int L>
    inline float lane(Vec4 a) { return a.v[L]; }
    template <int L>
    inline uint32_t laneBits(Vec4 a) { return bitsOfFloat(a.v[L]); }
    inline bool magnitudesBelow(Vec4 a, uint32_t limit)
    {
        for (int i = 0; i < 4; ++i)
            if ((bitsOfFloat(a.v[i]) & 0x7FFFFFFFu) >= limit)
                return false;
        return true;
    }
    // volatile keeps each result rounded on its own (no contraction, no wider evaluation).
    inline Vec4 add(Vec4 a, Vec4 b)
    {
        Vec4 r;
        for (int i = 0; i < 4; ++i)
        {
            volatile float x = a.v[i] + b.v[i];
            r.v[i] = x;
        }
        return r;
    }
    inline Vec4 sub(Vec4 a, Vec4 b)
    {
        Vec4 r;
        for (int i = 0; i < 4; ++i)
        {
            volatile float x = a.v[i] - b.v[i];
            r.v[i] = x;
        }
        return r;
    }
    inline Vec4 mul(Vec4 a, Vec4 b)
    {
        Vec4 r;
        for (int i = 0; i < 4; ++i)
        {
            volatile float x = a.v[i] * b.v[i];
            r.v[i] = x;
        }
        return r;
    }
    inline Vec4 vmin(Vec4 a, Vec4 b)
    {
        Vec4 r;
        for (int i = 0; i < 4; ++i)
            r.v[i] = a.v[i] < b.v[i] ? a.v[i] : b.v[i];
        return r;
    }
    inline Vec4 vmax(Vec4 a, Vec4 b)
    {
        Vec4 r;
        for (int i = 0; i < 4; ++i)
            r.v[i] = a.v[i] > b.v[i] ? a.v[i] : b.v[i];
        return r;
    }
    template <int Mask>
    inline Vec4 blend(Vec4 a, Vec4 b)
    {
        Vec4 r = a;
        for (int i = 0; i < 4; ++i)
            if (Mask & (1 << i))
                r.v[i] = b.v[i];
        return r;
    }
    inline Vec4 rotateLanes(Vec4 a) { return {{a.v[1], a.v[2], a.v[3], a.v[0]}}; }
    template <int X, int Y, int Z, int W>
    inline Vec4 shuffle(Vec4 a) { return {{a.v[X], a.v[Y], a.v[Z], a.v[W]}}; }
    inline Vec4 norm(Vec4 a)
    {
        Vec4 r;
        for (int i = 0; i < 4; ++i)
        {
            uint32_t b = bitsOfFloat(a.v[i]);
            const uint32_t e = b & 0x7F800000u;
            if (e == 0u)
                b &= 0x80000000u;
            else if (e == 0x7F800000u)
                b = (b & 0x80000000u) | 0x7F7FFFFFu;
            r.v[i] = floatOfBits(b);
        }
        return r;
    }
    inline Vec4 itof(Vec4 a)
    {
        Vec4 r;
        for (int i = 0; i < 4; ++i)
        {
            int32_t n;
            std::memcpy(&n, &a.v[i], 4);
            volatile float x = static_cast<float>(n);
            r.v[i] = x;
        }
        return r;
    }
    inline Vec4 ftoi(Vec4 a)
    {
        Vec4 r;
        for (int i = 0; i < 4; ++i)
        {
            const float f = a.v[i];
            const int32_t n = f >= 2147483648.0f ? 0x7FFFFFFF : f < -2147483648.0f ? INT32_MIN : static_cast<int32_t>(f);
            std::memcpy(&r.v[i], &n, 4);
        }
        return r;
    }
#endif

    // ITOF12 / FTOI4: the fixed-point forms (exact power-of-two scales).
    inline Vec4 itof12(Vec4 a) { return mul(itof(a), splat(1.0f / 4096.0f)); }
    inline Vec4 ftoi4(Vec4 a) { return ftoi(mul(a, splat(16.0f))); }

    inline uint32_t floatBits(float f)
    {
        uint32_t b;
        std::memcpy(&b, &f, 4);
        return b;
    }
    inline float bitsFloat(uint32_t b)
    {
        float f;
        std::memcpy(&f, &b, 4);
        return f;
    }

    // A scalar square root in the float environment (round toward zero: sqrtss / fsqrt honour MXCSR / FPCR, and so does
    // a correctly rounded std::sqrt); one rounding, no contraction. For FPU / VU0 divide-unit code (rung 0's scalar
    // helpers).
    inline float sqrtScalar(float x)
    {
        volatile float r = std::sqrt(x);
        return r;
    }
    // A scalar quotient in the float environment (one rounding): VU0's VDIV for ordinary operands.
    inline float divScalar(float a, float b)
    {
        volatile float r = a / b;
        return r;
    }

    // The VU's float environment for the duration of a native program: round toward zero, denormals flushed (inputs
    // and results). The x86 runtime already runs its VU jobs this way (runLoopWith); set it here for every path, so a
    // program also runs right when called from elsewhere (the replay tool, other platforms).
    class VuFloatEnv
    {
    public:
#if defined(__x86_64__) || defined(_M_X64)
        VuFloatEnv() : m_saved(_mm_getcsr()) { _mm_setcsr((m_saved & ~0x6000u) | 0x6000u | 0x8040u); } // RZ, FTZ, DAZ
        ~VuFloatEnv() { _mm_setcsr(m_saved); }

    private:
        unsigned m_saved;
#elif defined(__aarch64__)
        VuFloatEnv()
        {
            __asm__ volatile("mrs %0, fpcr" : "=r"(m_saved));
            const uint64_t env = (m_saved & ~(3ull << 22)) | (3ull << 22) | (1ull << 24); // RMode = RZ, FZ
            __asm__ volatile("msr fpcr, %0" : : "r"(env));
        }
        ~VuFloatEnv() { __asm__ volatile("msr fpcr, %0" : : "r"(m_saved)); }

    private:
        uint64_t m_saved;
#else
        // Elsewhere: rounding from <cfenv>; flushing denormals has no portable switch, so such a platform must add its
        // own here (the VU never sees a denormal).
        VuFloatEnv() : m_saved(std::fegetround()) { std::fesetround(FE_TOWARDZERO); }
        ~VuFloatEnv() { std::fesetround(m_saved); }

    private:
        int m_saved;
#endif
    };
}

// The VU1 programs' name for it (they were written in kamite::render).
namespace kamite::render
{
    using namespace kamite::native;
}
