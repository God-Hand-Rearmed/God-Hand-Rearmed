// Native libvu0 helpers (Sony's VU0 macro-mode matrix library, linked into the game): the rung 2 pilot
// (claude/native-next.md). Leaf functions, called everywhere (rung 1: libvu0 4.9% of the game thread's busy samples, own
// time), and part of rung 3's packet building. Switch: KAMITE_NATIVE_LIBVU0 (unset = the checked ones, 1 = all, 0 =
// none); a native that is not checked yet runs in the shadow harness during dev replays only.
//
// Each run<Env>() is the recompiled function's effect (SLUS_215.03, generated/sceVu0*.cpp) without the per-op
// bookkeeping: same loads, same ops in the same order (libvu0_core.h), same stores, same registers left behind (the
// VU0 temporaries vf4-vf9 and ACC, the argument registers it steps, a3 as its loop counter), the same checkpoints.

#include "kamite/native/native.h"
#include "kamite/native/libvu0_core.h"

#include <cstring>

namespace kamite::native::libvu0
{
    namespace
    {
        uint32_t gpr32(const R5900Context *ctx, int reg)
        {
            uint32_t value;
            std::memcpy(&value, &ctx->r[reg], sizeof(value));
            return value;
        }

        // SET_GPR_S32: the low doubleword, sign-extended; the upper doubleword stays.
        void setGprS32(R5900Context *ctx, int reg, uint32_t value)
        {
            const int64_t wide = static_cast<int32_t>(value);
            std::memcpy(&ctx->r[reg], &wide, sizeof(wide));
        }

        // ADDI's overflow trap (SignalException): never taken by a pointer stepped through plain RAM, but a native
        // declines the call rather than differ.
        bool addiOverflows(uint32_t base, uint32_t step)
        {
            const int64_t sum = static_cast<int64_t>(static_cast<int32_t>(base)) + static_cast<int32_t>(step);
            return sum != static_cast<int32_t>(static_cast<uint32_t>(sum));
        }

        bool overlapsUnequal(uint32_t a, uint32_t b, uint32_t size) { return a != b && a < b + size && b < a + size; }

        void setVf(R5900Context *ctx, int reg, Vec4 v) { store(&ctx->vu0_vf[reg], v); }

        // ---- sceVu0MulMatrix 0x30A2E0 (a0 = out, a1 = A, a2 = B): out = B x A (row vectors) -----------------
        // vf4-vf7 = A's rows; per row: vf8 = B row k, ACC / vf9 = mulRow, out row k = vf9, a3 / a2 / a0 stepped, a
        // backward branch (checkpoint, resume entry 0x30A2F4) after rows 0-2.
        constexpr uint32_t kMulMatrix = 0x30A2E0u, kMulMatrixLoop = 0x30A2F4u;

        template <class Env>
        bool mulMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t a0 = gpr32(ctx, 4), a1 = gpr32(ctx, 5), a2 = gpr32(ctx, 6);
            uint32_t out, ma, mb;
            if (!plainRam(a0 & ~0xFu, 64u, out) || !plainRam(a1 & ~0xFu, 64u, ma) || !plainRam(a2 & ~0xFu, 64u, mb))
                return false;
            // The original reads B row k after writing out row k - 1: an out that overlaps B (but is not B) would feed
            // its own rows back; it never happens in the game's calls, so decline it rather than mimic it.
            if (overlapsUnequal(out, mb, 64u) || addiOverflows(a0, 64u) || addiOverflows(a2, 64u))
                return false;
            uint8_t *const ram = env.ram;
            const Vec4 m0 = load(ram + ma), m1 = load(ram + ma + 16), m2 = load(ram + ma + 32), m3 = load(ram + ma + 48);
            if (!finiteInputs(m0) || !finiteInputs(m1) || !finiteInputs(m2) || !finiteInputs(m3) ||
                !finiteInputs(load(ram + mb)) || !finiteInputs(load(ram + mb + 16)) || !finiteInputs(load(ram + mb + 32)) ||
                !finiteInputs(load(ram + mb + 48)))
                return false;
            setVf(ctx, 4, m0);
            setVf(ctx, 5, m1);
            setVf(ctx, 6, m2);
            setVf(ctx, 7, m3);
            for (uint32_t k = 0;; ++k)
            {
                const Vec4 b = load(ram + mb + 16u * k); // out == B: row k is still B's (row k - 1 went before it)
                const RowResult row = mulRow(m0, m1, m2, m3, b);
                store(ram + out + 16u * k, row.r);
                if (k == 3u || env.checkpoint(32u))
                {
                    setVf(ctx, 8, b);
                    store(&ctx->vu0_acc, row.acc);
                    setVf(ctx, 9, row.r);
                    setGprS32(ctx, 7, 3u - k);
                    setGprS32(ctx, 6, a2 + 16u * (k + 1u));
                    setGprS32(ctx, 4, a0 + 16u * (k + 1u));
                    ctx->pc = k == 3u ? gpr32(ctx, 31) : kMulMatrixLoop;
                    return true;
                }
            }
        }

        void mulMatrixFootprint(const R5900Context *ctx, Footprint &fp)
        {
            fp.add(gpr32(ctx, 5) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 6) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 4) & ~0xFu, 64u);
        }

        // ---- sceVu0ApplyMatrix 0x30A2B0 (a0 = out vector, a1 = matrix, a2 = vector) --------------------------------
        constexpr uint32_t kApplyMatrix = 0x30A2B0u;

        template <class Env>
        bool applyMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t a0 = gpr32(ctx, 4), a1 = gpr32(ctx, 5), a2 = gpr32(ctx, 6);
            uint32_t out, ma, vb;
            if (!plainRam(a0 & ~0xFu, 16u, out) || !plainRam(a1 & ~0xFu, 64u, ma) || !plainRam(a2 & ~0xFu, 16u, vb))
                return false;
            uint8_t *const ram = env.ram;
            const Vec4 m0 = load(ram + ma), m1 = load(ram + ma + 16), m2 = load(ram + ma + 32), m3 = load(ram + ma + 48);
            const Vec4 b = load(ram + vb);
            if (!finiteInputs(m0) || !finiteInputs(m1) || !finiteInputs(m2) || !finiteInputs(m3) || !finiteInputs(b))
                return false;
            const RowResult row = mulRow(m0, m1, m2, m3, b);
            store(ram + out, row.r);
            setVf(ctx, 4, m0);
            setVf(ctx, 5, m1);
            setVf(ctx, 6, m2);
            setVf(ctx, 7, m3);
            setVf(ctx, 8, b);
            store(&ctx->vu0_acc, row.acc);
            setVf(ctx, 9, row.r);
            ctx->pc = gpr32(ctx, 31);
            return true;
        }

        void applyMatrixFootprint(const R5900Context *ctx, Footprint &fp)
        {
            fp.add(gpr32(ctx, 5) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 6) & ~0xFu, 16u);
            fp.add(gpr32(ctx, 4) & ~0xFu, 16u);
        }

        // ---- _sceVu0ecossin 0x30A5D0 and sceVu0RotMatrixX / Y / Z 0x30A700 / 0x30A7B8 / 0x30A650 -----------------
        // RotMatrix*(a0 = out, a1 = matrix, f12 = angle): f12 = pi/2 -+ angle (r7 = which), vf6 = all of r8 (its low
        // word the angle's bits), ra kept in a2 and the call to ecossin (a guest call: 8 cycles, may come due with
        // ctx->pc = 0x30A5D0), then the rotation rows vf6-vf9 and out = matrix x rotation row by row (a1 stepped, the
        // backward-branch checkpoints as MulMatrix's).
        constexpr uint32_t kEcossin = 0x30A5D0u, kCoefficients = 0x3C3C90u;
        constexpr uint32_t kPiOver2 = 0x3FC90FDBu;

        void setGprLow64(R5900Context *ctx, int reg, uint64_t value) { std::memcpy(&ctx->r[reg], &value, sizeof(value)); }
        uint64_t gpr64(const R5900Context *ctx, int reg)
        {
            uint64_t value;
            std::memcpy(&value, &ctx->r[reg], sizeof(value));
            return value;
        }
        Vec4 vf(const R5900Context *ctx, int reg) { return load(&ctx->vu0_vf[reg]); }

        // A garbage lane multiplied by 0 (vf4.yzw in ecossin) must not be the host's Inf / NaN (the original redoes such
        // a lane exactly).
        bool noExponent255(Vec4 v) { return magnitudesBelow(v, 0x7F800000u); }

        // ecossin on the context: what the original leaves (vf4-vf8, Q, the status flags, r8, pc = ra).
        void ecossinOn(R5900Context *ctx, const uint8_t *ram, uint32_t coefficients)
        {
            VuRegs v{vf(ctx, 4), vf(ctx, 5), vf(ctx, 6), vf(ctx, 7), vf(ctx, 8), ctx->vu0_q, ctx->vu0_status};
            ecossin(vf(ctx, 0), load(ram + coefficients), gpr64(ctx, 7) != 0u, v);
            setVf(ctx, 4, v.vf4);
            setVf(ctx, 5, v.vf5);
            setVf(ctx, 6, v.vf6);
            setVf(ctx, 7, v.vf7);
            setVf(ctx, 8, v.vf8);
            ctx->vu0_q = v.q;
            ctx->vu0_status = v.status;
            setGprS32(ctx, 8, kCoefficients); // lui / addiu
            ctx->pc = gpr32(ctx, 31);
        }

        template <class Env>
        bool ecossinNative(Env &env, R5900Context *ctx)
        {
            uint32_t coefficients;
            if (!plainRam(kCoefficients, 16u, coefficients))
                return false;
            // |angle| < 32: the polynomial's powers stay far below FLT_MAX.
            if ((laneBits<0>(vf(ctx, 6)) & 0x7FFFFFFFu) >= 0x42000000u || !noExponent255(vf(ctx, 4)) ||
                !finiteInputs(load(env.ram + coefficients)))
                return false;
            ecossinOn(ctx, env.ram, coefficients);
            return true;
        }

        void ecossinFootprint(const R5900Context *, Footprint &fp) { fp.add(kCoefficients, 16u); }

        struct RotEntry
        {
            uint32_t entry, back, loop; // the function, the return from ecossin, the loop head (resume entries)
        };
        constexpr RotEntry kRot[3] = {{0x30A700u, 0x30A74Cu, 0x30A780u}, {0x30A7B8u, 0x30A804u, 0x30A838u},
                                      {0x30A650u, 0x30A69Cu, 0x30A6CCu}};

        template <int Axis, class Env>
        bool rotMatrix(Env &env, R5900Context *ctx)
        {
            const uint32_t a0 = gpr32(ctx, 4), a1 = gpr32(ctx, 5);
            uint32_t out, in, coefficients;
            if (!plainRam(a0 & ~0xFu, 64u, out) || !plainRam(a1 & ~0xFu, 64u, in) || !plainRam(kCoefficients, 16u, coefficients))
                return false;
            if (overlapsUnequal(out, in, 64u) || addiOverflows(a0, 64u) || addiOverflows(a1, 64u))
                return false;
            uint8_t *const ram = env.ram;
            const float angle = ctx->f[12];
            // |angle| < 16 (pi/2 -+ it is ecossin's input), a plain-sized matrix, a vf4 that * 0 stays 0.
            if ((floatBits(angle) & 0x7FFFFFFFu) >= 0x41800000u || !noExponent255(vf(ctx, 4)) ||
                !finiteInputs(load(ram + coefficients)) || !finiteInputs(load(ram + in)) || !finiteInputs(load(ram + in + 16)) ||
                !finiteInputs(load(ram + in + 32)) || !finiteInputs(load(ram + in + 48)))
                return false;
            const RotEntry &at = kRot[Axis];

            // c.olt.s f12, f0 (f0 = 0): a negative angle with a non-zero exponent sets C.
            const uint32_t angleBits = floatBits(angle);
            const bool negativeAngle = (angleBits & 0x80000000u) && (angleBits & 0x7F800000u);
            ctx->fcr31 = negativeAngle ? (ctx->fcr31 | 0x800000u) : (ctx->fcr31 & ~0x800000u);
            setGprLow64(ctx, 1, kPiOver2);
            const float piOver2 = bitsFloat(kPiOver2);
            ctx->f[0] = piOver2;
            ctx->f[12] = negativeAngle ? piOver2 + angle : piOver2 - angle;
            setGprLow64(ctx, 7, negativeAngle ? 1u : 0u);
            setGprS32(ctx, 8, floatBits(ctx->f[12]));
            std::memcpy(&ctx->vu0_vf[6], &ctx->r[8], 16); // qmtc2: the whole register, its upper half included
            setGprLow64(ctx, 6, gpr64(ctx, 31));
            setGprS32(ctx, 31, at.back);
            ctx->pc = kEcossin;
            if (env.call())
                return true; // the scheduler resumes at ecossin, which returns to the original's resume entry
            ecossinOn(ctx, ram, coefficients);
            setGprLow64(ctx, 31, gpr64(ctx, 6));

            Vec4 vf4 = vf(ctx, 4);
            const RotationRows m = rotationRows<Axis>(vf(ctx, 0), vf(ctx, 5), vf4);
            setVf(ctx, 6, m.r0);
            setVf(ctx, 7, m.r1);
            setVf(ctx, 8, m.r2);
            setVf(ctx, 9, m.r3);
            for (uint32_t k = 0;; ++k)
            {
                const Vec4 row = load(ram + in + 16u * k); // out == in: row k is still the input's
                const RowResult r = mulRow(m.r0, m.r1, m.r2, m.r3, row);
                store(ram + out + 16u * k, r.r);
                if (k == 3u || env.checkpoint(32u))
                {
                    setVf(ctx, 4, row);
                    store(&ctx->vu0_acc, r.acc);
                    setVf(ctx, 5, r.r);
                    setGprS32(ctx, 7, 3u - k);
                    setGprS32(ctx, 5, a1 + 16u * (k + 1u));
                    setGprS32(ctx, 4, a0 + 16u * (k + 1u));
                    ctx->pc = k == 3u ? gpr32(ctx, 31) : at.loop;
                    return true;
                }
            }
        }

        void rotMatrixFootprint(const R5900Context *ctx, Footprint &fp)
        {
            fp.add(gpr32(ctx, 5) & ~0xFu, 64u);
            fp.add(gpr32(ctx, 4) & ~0xFu, 64u);
            fp.add(kCoefficients, 16u);
        }
    }

    void registerFunctions()
    {
        // All checked (on by default; claude/native-next.md "Rung 2 as built"): 0 mismatches and 0 declines in K's
        // shadowed replays (MulMatrix / ApplyMatrix ~75M calls on f733c30; ecossin 31.8M and the RotMatrix trio 31.8M on
        // 21f25ea, both rates), every .ram.bin identical, the first two measured ~3x cheaper in the sampler.
        registerFunction(NativeFunction{"sceVu0MulMatrix", kMulMatrix, "libvu0", true, &mulMatrix<RealEnv>,
                                        &mulMatrix<ShadowEnv>, &mulMatrixFootprint});
        registerFunction(NativeFunction{"sceVu0ApplyMatrix", kApplyMatrix, "libvu0", true, &applyMatrix<RealEnv>,
                                        &applyMatrix<ShadowEnv>, &applyMatrixFootprint});
        registerFunction(NativeFunction{"_sceVu0ecossin", kEcossin, "libvu0", true, &ecossinNative<RealEnv>,
                                        &ecossinNative<ShadowEnv>, &ecossinFootprint});
        registerFunction(NativeFunction{"sceVu0RotMatrixX", kRot[0].entry, "libvu0", true, &rotMatrix<0, RealEnv>,
                                        &rotMatrix<0, ShadowEnv>, &rotMatrixFootprint});
        registerFunction(NativeFunction{"sceVu0RotMatrixY", kRot[1].entry, "libvu0", true, &rotMatrix<1, RealEnv>,
                                        &rotMatrix<1, ShadowEnv>, &rotMatrixFootprint});
        registerFunction(NativeFunction{"sceVu0RotMatrixZ", kRot[2].entry, "libvu0", true, &rotMatrix<2, RealEnv>,
                                        &rotMatrix<2, ShadowEnv>, &rotMatrixFootprint});
    }
}
