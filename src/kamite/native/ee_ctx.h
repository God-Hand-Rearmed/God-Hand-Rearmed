#pragma once

// The guest context as the generated code changes it, for native EE functions (rung 3): GPR writes the way the
// macros do them (ps2_runtime_macros.h), VU0 float registers as Vec4s, and guest RAM reached through env.at().
//   - SET_GPR_S32 / SET_GPR_U64 (addiu, lui, lw, daddu, or ...): the low doubleword; the upper doubleword stays.
//   - SET_GPR_VEC (lq, movz / movn, qmfc2): all 128 bits.
//   - JAL: ra = the return address, sign-extended (SET_GPR_U32); a branch or jump with a real (non-nop) delay slot
//     leaves ctx->branch_pc = its address and in_delay_slot = false (both compared with the whole context).

#include "kamite/native/native_function.h"
#include "kamite/native/vu_vec.h"

#include <cstring>

namespace kamite::native::ee
{
    enum Reg : int
    {
        zero = 0, at = 1, v0 = 2, v1 = 3, a0 = 4, a1 = 5, a2 = 6, a3 = 7, t0 = 8, t1 = 9, t2 = 10, t3 = 11, t4 = 12,
        t5 = 13, t6 = 14, t7 = 15, s0 = 16, s1 = 17, s2 = 18, s3 = 19, s4 = 20, sp = 29, ra = 31,
    };

    inline uint32_t u32(const R5900Context *ctx, int reg)
    {
        uint32_t value;
        std::memcpy(&value, &ctx->r[reg], sizeof(value));
        return value;
    }
    inline uint64_t u64(const R5900Context *ctx, int reg)
    {
        uint64_t value;
        std::memcpy(&value, &ctx->r[reg], sizeof(value));
        return value;
    }
    inline int64_t s64(const R5900Context *ctx, int reg) { return static_cast<int64_t>(u64(ctx, reg)); }
    inline void setLow64(R5900Context *ctx, int reg, uint64_t value)
    {
        if (reg != 0)
            std::memcpy(&ctx->r[reg], &value, sizeof(value));
    }
    inline void setS32(R5900Context *ctx, int reg, uint32_t value)
    {
        setLow64(ctx, reg, static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(value))));
    }
    // daddu rd, rs, zero
    inline void move(R5900Context *ctx, int rd, int rs) { setLow64(ctx, rd, u64(ctx, rs)); }
    inline void setVec(R5900Context *ctx, int reg, const void *bytes16)
    {
        if (reg != 0)
            std::memcpy(&ctx->r[reg], bytes16, 16);
    }
    // movz / movn: the whole register
    inline void moveVec(R5900Context *ctx, int rd, int rs)
    {
        if (rd == 0)
            return;
        if (rs == 0)
            std::memset(&ctx->r[rd], 0, 16);
        else
            std::memcpy(&ctx->r[rd], &ctx->r[rs], 16);
    }

    inline Vec4 vf(const R5900Context *ctx, int reg) { return load(&ctx->vu0_vf[reg]); }
    inline void setVf(R5900Context *ctx, int reg, Vec4 v) { store(&ctx->vu0_vf[reg], v); }

    // VU0's constant register as the hardware holds it; natives that use vf0 check it first (cheap).
    inline bool vf0Constant(const R5900Context *ctx)
    {
        static const uint32_t expect[4] = {0u, 0u, 0u, 0x3F800000u};
        return std::memcmp(&ctx->vu0_vf[0], expect, 16) == 0;
    }

    // A branch / jump at `site` whose delay slot is a real instruction: what the generated code leaves behind it.
    inline void delaySlot(R5900Context *ctx, uint32_t site)
    {
        ctx->branch_pc = site;
        ctx->in_delay_slot = false;
    }
    // JAL from `site` (ra = site + 8), its delay slot real (branch_pc = site) or a nop.
    inline void jal(R5900Context *ctx, uint32_t site, uint32_t target, bool realDelaySlot)
    {
        setS32(ctx, ra, site + 8u);
        if (realDelaySlot)
            delaySlot(ctx, site);
        ctx->pc = target;
    }
    // jr ra with a real delay slot already applied by the caller: branch_pc = site, pc = the target read before it.
    inline void jrRa(R5900Context *ctx, uint32_t site, uint32_t target, bool realDelaySlot)
    {
        if (realDelaySlot)
            delaySlot(ctx, site);
        ctx->pc = target;
    }

    // RAM access through an env (RealEnv / ShadowEnv): offsets from plainRam(). lq / sq / lqc2 / sqc2 take the
    // address with its low 4 bits cleared; the callers pass such offsets.
    template <class Env>
    inline uint32_t read32(Env &env, uint32_t offset)
    {
        uint32_t v;
        std::memcpy(&v, env.at(offset, 4u), 4);
        return v;
    }
    template <class Env>
    inline uint64_t read64(Env &env, uint32_t offset)
    {
        uint64_t v;
        std::memcpy(&v, env.at(offset, 8u), 8);
        return v;
    }
    template <class Env>
    inline void write32(Env &env, uint32_t offset, uint32_t v)
    {
        std::memcpy(env.at(offset, 4u), &v, 4);
    }
    template <class Env>
    inline void write64(Env &env, uint32_t offset, uint64_t v)
    {
        std::memcpy(env.at(offset, 8u), &v, 8);
    }
    template <class Env>
    inline Vec4 read128(Env &env, uint32_t offset)
    {
        return load(env.at(offset, 16u));
    }
    template <class Env>
    inline void write128(Env &env, uint32_t offset, Vec4 v)
    {
        store(env.at(offset, 16u), v);
    }
    // lq into a GPR / sq from one (the whole register)
    template <class Env>
    inline void lq(Env &env, R5900Context *ctx, int reg, uint32_t offset)
    {
        setVec(ctx, reg, env.at(offset, 16u));
    }
    template <class Env>
    inline void sq(Env &env, const R5900Context *ctx, int reg, uint32_t offset)
    {
        uint8_t *p = env.at(offset, 16u);
        if (reg == 0)
            std::memset(p, 0, 16);
        else
            std::memcpy(p, &ctx->r[reg], 16);
    }

    // A guest address -> its RAM offset, or false (outside plain RAM).
    inline bool offsetOf(uint32_t address, uint32_t size, uint32_t &offset) { return plainRam(address, size, offset); }
    inline bool overlaps(uint32_t a, uint32_t aSize, uint32_t b, uint32_t bSize) { return a < b + bSize && b < a + aSize; }
}
