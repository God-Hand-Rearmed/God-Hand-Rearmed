#ifndef PS2_CG_H
#define PS2_CG_H
// Support header for the "register locals" code generator mode (PS2X_CODEGEN=locals, ps2xRecomp function_emitter.cpp;
// build.py sets it). From Vnnc1/gow-native-pc (GPL-3.0, engine/patches/08-codegen-register-locals.patch), adapted there
// from the Killzone port's patch 0017 (drakolordx7/KillzoneRecomp).
//
// A generated function in this mode keeps the low 64 bits of every guest GPR it touches in a C++ local (gprN; gprhN
// holds the upper 64 bits of registers used by 128-bit operations), so the compiler can keep them in host registers.
// The function prologue loads them from ctx->r[] and defines, for that function:
//   PS2_FLUSH()  / PS2_FLUSH_E()   locals that the function writes -> ctx (statement / expression form)
//   PS2_RELOAD() / PS2_RELOAD_E()  ctx -> every local (after code that may change ctx)
// Statements that hand ctx to someone else (calls, returns, syscalls, stubs, exceptions, hooks) are emitted with the
// classic macros between PS2_FLUSH() and PS2_RELOAD(); ctx->pc is only stored where somebody reads it.

#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"

#include <cstdint>

#define L_GPR_U32(n) (static_cast<uint32_t>(gpr##n))
#define L_GPR_S32(n) (static_cast<int32_t>(gpr##n))
#define L_GPR_U64(n) (static_cast<uint64_t>(gpr##n))
#define L_GPR_S64(n) (static_cast<int64_t>(gpr##n))
#define L_GPR_VEC(n) _mm_set_epi64x(static_cast<long long>(gprh##n), static_cast<long long>(gpr##n))

// Same values as the classic setters (ps2_runtime_macros.h): 32-bit results sign- (ZE32: zero-) extend into the low
// 64 bits; the upper half is untouched.
#define L_SET_GPR_U32(n, v) do { gpr##n = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(v))); } while (0)
#define L_SET_GPR_S32(n, v) L_SET_GPR_U32(n, v)
#define L_SET_GPR_ZE32(n, v) do { gpr##n = static_cast<uint64_t>(static_cast<uint32_t>(v)); } while (0)
#define L_SET_GPR_U64(n, v) do { gpr##n = static_cast<uint64_t>(static_cast<int64_t>(v)); } while (0)
#define L_SET_GPR_S64(n, v) L_SET_GPR_U64(n, v)
#define L_SET_GPR_VEC(n, v)                                                       \
    do                                                                            \
    {                                                                             \
        const __m128i _ps2cg_v = (v);                                             \
        gpr##n = static_cast<uint64_t>(PS2_EXTRACT_EPI64_0(_ps2cg_v));            \
        gprh##n = static_cast<uint64_t>(PS2_EXTRACT_EPI64_1(_ps2cg_v));           \
    } while (0)
#define PS2_DISCARD(v) ((void)(v))

// ctx->r[n] is a __m128i: its halves as two uint64_t. Read with the runtime's own extract; the low half written as the
// classic setters do (Ps2SetGprScalar64), the high half with an insert. (A memcpy at byte offset 8 of the vector was
// miscompiled by GCC 13 -O2 in tools/recomp/codegen_diff: a register read back as 0.)
#define PS2_CG_LO(ctx_ptr, n) static_cast<uint64_t>(PS2_EXTRACT_EPI64_0((ctx_ptr)->r[n]))
#define PS2_CG_HI(ctx_ptr, n) static_cast<uint64_t>(PS2_EXTRACT_EPI64_1((ctx_ptr)->r[n]))
#define PS2_CG_SET_LO(ctx_ptr, n, v) Ps2SetGprScalar64((ctx_ptr), (n), static_cast<int64_t>(v))
#define PS2_CG_SET_HI(ctx_ptr, n, v) ((void)((ctx_ptr)->r[n] = _mm_insert_epi64((ctx_ptr)->r[n], static_cast<long long>(v), 1)))

// Loop back edge checkpoint (classic: runtime->eeCheckpointDue()). The quiet path only defers the cycles and needs
// nothing from ctx; when the full checkpoint runs, the registers are written back first (it may run interrupt handlers
// or switch threads), and read again when execution continues here.
#define PS2_CG_DUE()                                                                                                   \
    (runtime->eeCheckpointQuiet(32u) ? false                                                                           \
                                     : (PS2_FLUSH_E(), runtime->eeCheckpointFull(32u) ? true : (PS2_RELOAD_E(), false)))

#endif // PS2_CG_H
