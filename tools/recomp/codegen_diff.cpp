// codegen_diff: the classic and the register-locals generated code of the same functions (ps2xRecomp
// function_emitter.cpp, PS2X_CODEGEN=locals), run on the same random guest state (GPRs, FPU / VU0 registers, 128 MB of
// RAM) with deterministic stand-ins for calls (a fake callee reads the argument / saved registers and clobbers the
// caller-saved ones; past a budget the call unwinds), MMIO (hashed), jumps and the loop checkpoint (past a budget the full
// checkpoint runs, so the locals are flushed and the function returns). Every context field, RAM and the call / MMIO log
// must come out the same. Linux (~1 s per file to build), see codegen_diff.sh. Nov 20: 2109 functions (600 random, 900
// with 128-bit / jump-table / hook / HI1-LO1 / SA / loop code, the profile's hot ones) x 4 seeds, 0 differences once
// ps2_cg.h stopped writing the vector's halves with memcpy (GCC 13 -O2 read a register back as 0 in 3 of them); a copy
// with the PS2_FLUSH before calls removed fails 145 of 160 runs.
#include "ps2_runtime_macros.h"
#include "ps2_runtime.h"
#include <dlfcn.h>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include <fstream>
#include <atomic>

std::atomic<uint32_t> g_ps2xPatchFlags{0u};
uint32_t ps2xPatchFlagMask(const char *) { return 0u; }
PS2Runtime::RecompiledFunction ps2xHostHook(const char *) { return nullptr; }

static uint64_t mix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
static uint64_t lo(const R5900Context *c, int r) { uint64_t v; std::memcpy(&v, &c->r[r], 8); return v; }
static void setlo(R5900Context *c, int r, uint64_t v) { std::memcpy(&c->r[r], &v, 8); }

static uint64_t g_quietCount, g_quietBudget, g_callCount, g_callBudget, g_log;
bool PS2Runtime::eeCheckpointQuiet(uint32_t) noexcept { return ++g_quietCount < g_quietBudget; }
bool PS2Runtime::eeCheckpointFull(uint32_t) noexcept { return true; }
bool PS2Runtime::eeCheckpointDue(uint32_t c) noexcept { return !eeCheckpointQuiet(c) && eeCheckpointFull(c); }

static bool fakeCallee(R5900Context *ctx, uint32_t target, uint32_t fallthrough)
{
    // a callee that reads the argument / saved registers and clobbers the caller-saved ones
    uint64_t h = mix(target);
    for (int r : {4, 5, 6, 7, 16, 17, 18, 19, 20, 21, 22, 23, 28, 29, 30, 31})
        h = mix(h ^ lo(ctx, r) * (r + 1));
    g_log = mix(g_log ^ h);
    setlo(ctx, 2, (uint64_t)(int64_t)(int32_t)(0x00200000u + ((h & 0xFFFFF) << 4)));
    setlo(ctx, 3, mix(h + 1));
    for (int r = 8; r <= 15; ++r)
        setlo(ctx, r, mix(h + r));
    setlo(ctx, 1, mix(h + 99));
    ctx->f[0] = (float)(h & 0xFFFF) * 0.25f;
    ctx->pc = fallthrough;
    return ++g_callCount < g_callBudget; // past the budget: an unwind (the caller returns)
}
bool PS2Runtime::callGuestFunction(uint8_t *, R5900Context *ctx, uint32_t t, uint32_t, uint32_t f) { return fakeCallee(ctx, t, f); }
bool PS2Runtime::callGuestFunctionIndirect(uint8_t *, R5900Context *ctx, uint32_t t, uint32_t, uint32_t f) { return fakeCallee(ctx, t ^ 0x5555u, f); }
bool PS2Runtime::dispatchGuestBranch(uint8_t *, R5900Context *ctx, uint32_t t, uint32_t s, uint32_t f, GuestBranchKind k, const char *)
{
    g_log = mix(g_log ^ t ^ ((uint64_t)s << 32) ^ (uint64_t)k);
    if (k == GuestBranchKind::DirectCall || k == GuestBranchKind::IndirectCall)
        return fakeCallee(ctx, t, f);
    ctx->pc = t;
    return false;
}
uint8_t PS2Runtime::Load8(uint8_t *, R5900Context *, uint32_t a) { return (uint8_t)mix(a); }
uint16_t PS2Runtime::Load16(uint8_t *, R5900Context *, uint32_t a) { return (uint16_t)mix(a); }
uint32_t PS2Runtime::Load32(uint8_t *, R5900Context *, uint32_t a) { return (uint32_t)mix(a); }
uint64_t PS2Runtime::Load64(uint8_t *, R5900Context *, uint32_t a) { return mix(a); }
__m128i PS2Runtime::Load128(uint8_t *, R5900Context *, uint32_t a) { return _mm_set_epi64x((long long)mix(a + 1), (long long)mix(a)); }
void PS2Runtime::Store8(uint8_t *, R5900Context *, uint32_t a, uint8_t v) { g_log = mix(g_log ^ a ^ ((uint64_t)v << 32)); }
void PS2Runtime::Store16(uint8_t *, R5900Context *, uint32_t a, uint16_t v) { g_log = mix(g_log ^ a ^ ((uint64_t)v << 32)); }
void PS2Runtime::Store32(uint8_t *, R5900Context *, uint32_t a, uint32_t v) { g_log = mix(g_log ^ a ^ ((uint64_t)v << 32)); }
void PS2Runtime::Store64(uint8_t *, R5900Context *, uint32_t a, uint64_t v) { g_log = mix(g_log ^ a ^ mix(v)); }
void PS2Runtime::Store128(uint8_t *, R5900Context *, uint32_t a, __m128i v) { g_log = mix(g_log ^ a ^ mix((uint64_t)_mm_cvtsi128_si64(v)) ^ (uint64_t)_mm_extract_epi64(v, 1)); }

using Fn = void (*)(uint8_t *, R5900Context *, PS2Runtime *);
static constexpr size_t kRam = PS2_RDRAM_SIZE;

int main(int argc, char **argv)
{
    if (argc < 4) { std::fprintf(stderr, "harness classic.so locals.so names.txt [seeds]\n"); return 2; }
    void *hc = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL), *hl = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
    if (!hc || !hl) { std::fprintf(stderr, "dlopen: %s\n", dlerror()); return 2; }
    const int seeds = argc > 4 ? std::atoi(argv[4]) : 4;
    std::vector<std::string> names;
    { std::ifstream in(argv[3]); std::string s; while (std::getline(in, s)) if (!s.empty()) names.push_back(s); }
    uint8_t *base = (uint8_t *)std::aligned_alloc(64, kRam), *ma = (uint8_t *)std::aligned_alloc(64, kRam),
            *mb = (uint8_t *)std::aligned_alloc(64, kRam);
    for (size_t i = 0; i < kRam; i += 4)
    {
        const uint64_t h = mix(i);
        uint32_t w = (h & 3) ? (0x00100000u + (uint32_t)((h >> 8) % 0x01E00000u)) & ~15u : (uint32_t)(h >> 32);
        if ((h & 7) == 4) { float f = (float)((h >> 16) & 0xFFFF) / 64.0f - 300.0f; std::memcpy(&w, &f, 4); }
        std::memcpy(base + i, &w, 4);
    }
    std::memcpy(ma, base, kRam); std::memcpy(mb, base, kRam);
    alignas(64) static unsigned char rtStorage[1 << 16]; // never touched by the stand-ins
    PS2Runtime *rt = reinterpret_cast<PS2Runtime *>(rtStorage);
    int failures = 0, runs = 0, missing = 0;
    for (const std::string &name : names)
    {
        Fn fc = (Fn)dlsym(hc, name.c_str()), fl = (Fn)dlsym(hl, name.c_str());
        if (!fc || !fl) { ++missing; continue; }
        for (int seed = 0; seed < seeds; ++seed)
        {
            R5900Context init;
            uint64_t s = mix(std::hash<std::string>{}(name) ^ (uint64_t)seed * 0x1234567ull);
            for (int r = 1; r < 32; ++r)
            {
                s = mix(s);
                const uint32_t p = 0x00100000u + (uint32_t)(s % 0x01E00000u);
                const uint64_t low = (seed & 1) && (s & 8) ? mix(s + 7) : (uint64_t)(int64_t)(int32_t)(p & ~3u);
                init.r[r] = _mm_set_epi64x((long long)mix(s + 3), (long long)low);
            }
            setlo(&init, 29, 0x01F00000u - 0x1000u * (seed + 1));
            for (int i = 0; i < 32; ++i) { s = mix(s); init.f[i] = (float)((int64_t)(s & 0xFFFFF) - 0x80000) / 256.0f; }
            for (int i = 1; i < 32; ++i) { s = mix(s); init.vu0_vf[i] = _mm_set_ps((float)(s & 255), (float)((s >> 8) & 255) - 128, (float)((s >> 16) & 1023) / 7, 1.5f); }
            init.hi = mix(s + 11); init.lo = mix(s + 12);
            R5900Context ca = init, cb = init;
            ca.pc = cb.pc = 0; // entry: the resume switch's default
            uint64_t logA, logB, qa, qb, callsA, callsB;
            g_quietBudget = 3000; g_callBudget = 20 + seed * 7;
            g_quietCount = g_callCount = 0; g_log = 0; fc(ma, &ca, rt); logA = g_log; qa = g_quietCount; callsA = g_callCount;
            g_quietCount = g_callCount = 0; g_log = 0; fl(mb, &cb, rt); logB = g_log; qb = g_quietCount; callsB = g_callCount;
            ++runs;
            bool same = std::memcmp(&ca, &cb, sizeof(ca)) == 0 && logA == logB && qa == qb && callsA == callsB;
            if (same && std::memcmp(ma, mb, kRam) != 0) same = false;
            if (!same)
            {
                ++failures;
                std::printf("DIFF %s seed %d:", name.c_str(), seed);
                for (int r = 0; r < 32; ++r)
                    if (std::memcmp(&ca.r[r], &cb.r[r], 16)) std::printf(" r%d", r);
                if (ca.pc != cb.pc) std::printf(" pc %x/%x", ca.pc, cb.pc);
                if (logA != logB) std::printf(" log");
                if (qa != qb || callsA != callsB) std::printf(" counts %lu/%lu %lu/%lu", qa, qb, callsA, callsB);
                if (std::memcmp(ca.f, cb.f, sizeof(ca.f))) std::printf(" fpu");
                if (std::memcmp(ma, mb, kRam)) std::printf(" ram");
                std::printf("\n");
            }
            // restore RAM for the next run (only where it changed)
            for (size_t i = 0; i < kRam; i += 4096)
            {
                if (std::memcmp(ma + i, base + i, 4096)) std::memcpy(ma + i, base + i, 4096);
                if (std::memcmp(mb + i, base + i, 4096)) std::memcpy(mb + i, base + i, 4096);
            }
        }
    }
    std::printf("functions %zu (missing %d), runs %d, failures %d\n", names.size(), missing, runs, failures);
    return failures ? 1 : 0;
}
