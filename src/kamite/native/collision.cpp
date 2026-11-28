// Native collision functions (rung 4, claude/rung4-collision-map.md): ChkLine's tree from the leaves up (K, Nov 27:
// C1 the probes' leaves, C2 the grid, C3 the solids and func_0012ED60, C4 ChkLine). Switch: KAMITE_NATIVE_COLLISION
// (unset = the checked ones, 1 = all, 0 = none); a native that is not checked yet runs in the shadow harness during dev
// replays only.
//
// Every native here is kRenderPath | kChargeAtEnd (rung 4's rules, rung4-motion-map.md "Grilled" 8 and 9): the harness
// compares what lasts (RAM outside the dead stack, the return value, the callee-saved registers, pc, in_delay_slot, the
// EE cycles); temporaries are don't-care. Allowed per batch by the caller scan (tools/native/caller_scan.py over the ELF
// and the 81 room DLLs: no caller reads a scratch register after any of them). C1's read and check everything first,
// compute, then write; C2's (the grid) run in the asm's order on a journal that puts their writes back on a decline:
// either way a decline changes nothing. A function with a frame declines any pointer into its frames' window (the
// original's saves would be read or overwritten).

#include "kamite/native/native.h"
#include "kamite/native/collision_core.h"
#include "kamite/native/ee_ctx.h"

#include <cstring>
#include <vector>

namespace kamite::native::collision
{
    namespace
    {
        namespace e = kamite::native::ee;

        // jr ra with a real delay slot, or a path through one (the generated code clears in_delay_slot after it)
        void returnFrom(R5900Context *ctx)
        {
            ctx->pc = e::u32(ctx, e::ra);
            ctx->in_delay_slot = false;
        }
        // lqc2 / sqc2: the quadword at address & ~0xF, plain RAM
        bool plain16(uint32_t address, uint32_t &offset) { return plainRam(address & ~0xFu, 16u, offset); }
        // A function's frames: sp 16-aligned, [sp - window, sp) plain RAM (offset `low`)
        bool frameWindow(const R5900Context *ctx, uint32_t window, uint32_t &low)
        {
            const uint32_t sp0 = e::u32(ctx, e::sp);
            return (sp0 & 0xFu) == 0u && plainRam(sp0 - window, window, low);
        }
        // An output written by swc1 x, z, y (words): 0 (none), or word-aligned plain RAM clear of the window
        bool outWords(uint32_t address, uint32_t low, uint32_t window, uint32_t &offset)
        {
            if (address == 0u)
                return true;
            return (address & 3u) == 0u && plainRam(address, 12u, offset) && !e::overlaps(offset, 12u, low, window);
        }
        template <class Env>
        void writeXzy(Env &env, uint32_t offset, Vec4 p)
        {
            e::write32(env, offset, laneBits<0>(p));
            e::write32(env, offset + 8u, laneBits<2>(p));
            e::write32(env, offset + 4u, laneBits<1>(p));
        }

        // ==== C1: the probes' leaves (claude/rung4-collision-map.md "C1") =================================================
        // caller_scan (ELF + 81 DLLs): 0x133948 2, 0x133408 5, 0x135EC8 3, 0x1361A0 1, HitProbe 4 sites; none reads a
        // scratch register after the call, no j, no code pointer. sceVu0InnerProduct is libvu0.cpp's (whole context).

        // ---- func_00133948 (a0 unused, a1, a2; src/cod/00133948.c; a leaf, jr ra + nop): a1 = vmini, a2 = vmax (xyzw) ----
        constexpr uint32_t kBoxSort = 0x133948u;
        template <class Env>
        bool boxSortNative(Env &env, R5900Context *ctx)
        {
            uint32_t a, b;
            if (!env.atEnd || !plain16(e::u32(ctx, e::a1), a) || !plain16(e::u32(ctx, e::a2), b))
                return false;
            const Vec4 va = e::read128(env, a), vb = e::read128(env, b);
            e::write128(env, a, vuMin(va, vb));
            e::write128(env, b, vuMax(va, vb)); // a1 == a2: the max, written last
            ctx->pc = e::u32(ctx, e::ra);       // a nop delay slot: in_delay_slot as it was
            return true;
        }

        // ---- func_00133408 (a0 unused, a1 = amin, a2 = amax, a3 = bmin, t0 = bmax; src/cod/00133408.c): v0 = overlap -----
        // lwc1 throughout (word-aligned, 12 bytes each). Every exit passes a real delay slot.
        constexpr uint32_t kOverlap = 0x133408u;
        template <class Env>
        bool overlapNative(Env &env, R5900Context *ctx)
        {
            const uint32_t p[4] = {e::u32(ctx, e::a1), e::u32(ctx, e::a2), e::u32(ctx, e::a3), e::u32(ctx, e::t0)};
            float v[4][3];
            if (!env.atEnd)
                return false;
            for (int k = 0; k < 4; ++k)
            {
                uint32_t o;
                if ((p[k] & 3u) != 0u || !plainRam(p[k], 12u, o))
                    return false;
                for (int i = 0; i < 3; ++i)
                    v[k][i] = bitsFloat(e::read32(env, o + 4u * i));
            }
            int loops = 0;
            const bool hit = boxesOverlap(v[0], v[1], v[2], v[3], loops);
            if (loops)
                env.checkpoint(32u);
            e::setS32(ctx, e::v0, hit ? 1u : 0u);
            returnFrom(ctx);
            return true;
        }

        // ---- func_00135EC8 (a0 = start, a1 = end, a2 = normal, a3 = a point of the plane, t0 = out; src/cod/00135ec8.c) ----
        // A 0xB0 frame (s0-s3, ra, f20 saved first); start / end / normal / point by lqc2 (& ~0xF), out by swc1 x, z, y.
        // Calls: Normalize, InnerProduct x3 (leaves: no frames of their own).
        constexpr uint32_t kPlaneHit = 0x135EC8u, kPlaneHitFrame = 0xB0u;
        template <class Env>
        bool planeHitNative(Env &env, R5900Context *ctx)
        {
            uint32_t low, s, en, n, pt, out = 0;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kPlaneHitFrame, low) ||
                !plain16(e::u32(ctx, e::a0), s) || !plain16(e::u32(ctx, e::a1), en) || !plain16(e::u32(ctx, e::a2), n) ||
                !plain16(e::u32(ctx, e::a3), pt) || !outWords(e::u32(ctx, e::t0), low, kPlaneHitFrame, out))
                return false;
            for (uint32_t o : {s, en, n, pt})
                if (e::overlaps(o, 16u, low, kPlaneHitFrame))
                    return false;
            const PlaneHit h = planeHit(e::vf(ctx, 0), e::read128(env, s), e::read128(env, en), e::read128(env, n),
                                        e::read128(env, pt), ctx->vu0_status);
            if (!h.ok)
                return false;
            for (int k = 0; k < 4; ++k)
                env.call();
            if (h.hit && e::u32(ctx, e::t0) != 0u)
                writeXzy(env, out, h.point);
            e::setS32(ctx, e::v0, static_cast<uint32_t>(h.hit));
            returnFrom(ctx);
            return true;
        }

        // ---- func_001361A0 (a0 = the quad: four quadwords, a1 = the point; asm): v0 = inside ------------------------------
        // A 0x150 frame (s0-s7, fp, ra, f20 saved first; the vf0 clear loop: one backward branch). The quad's rows and the
        // point by lqc2. Calls: Normalize x6, OuterProduct, then OuterProduct + InnerProduct per edge tested.
        constexpr uint32_t kQuadInside = 0x1361A0u, kQuadInsideFrame = 0x150u;
        template <class Env>
        bool quadInsideNative(Env &env, R5900Context *ctx)
        {
            uint32_t low, quad, pt;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kQuadInsideFrame, low) ||
                !plainRam(e::u32(ctx, e::a0) & ~0xFu, 64u, quad) || !plain16(e::u32(ctx, e::a1), pt) ||
                e::overlaps(quad, 64u, low, kQuadInsideFrame) || e::overlaps(pt, 16u, low, kQuadInsideFrame))
                return false;
            Vec4 v[4];
            for (uint32_t k = 0; k < 4u; ++k)
                v[k] = e::read128(env, quad + 16u * k);
            const QuadInside q = quadInside(e::vf(ctx, 0), v, e::read128(env, pt), ctx->vu0_status);
            if (!q.ok)
                return false;
            env.checkpoint(32u);
            for (int k = 0; k < q.calls; ++k)
                env.call();
            e::setS32(ctx, e::v0, static_cast<uint32_t>(q.inside));
            returnFrom(ctx);
            return true;
        }

        // ---- HitProbe_probeHitPoint (a0 = start, a1 = end, a2 = the quad, a3 = its normal, t0 = out; src/cod/001365b8.c) --
        // A 0x40 frame: hit = vf0 at sp, func_00135EC8(start, end, normal, quad row 0, hit), then func_001361A0(quad, hit)
        // (the deeper frame: 0x150), out = hit (x, z, y) when both say so. v0 = 1 / 0.
        constexpr uint32_t kHitProbe = 0x1365B8u, kHitProbeWindow = 0x40u + kQuadInsideFrame;
        template <class Env>
        bool hitProbeNative(Env &env, R5900Context *ctx)
        {
            uint32_t low, s, en, n, quad, out = 0;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kHitProbeWindow, low) ||
                !plain16(e::u32(ctx, e::a0), s) || !plain16(e::u32(ctx, e::a1), en) || !plain16(e::u32(ctx, e::a3), n) ||
                !plainRam(e::u32(ctx, e::a2) & ~0xFu, 64u, quad) ||
                !outWords(e::u32(ctx, e::t0), low, kHitProbeWindow, out) || e::overlaps(quad, 64u, low, kHitProbeWindow))
                return false;
            for (uint32_t o : {s, en, n})
                if (e::overlaps(o, 16u, low, kHitProbeWindow))
                    return false;
            const Vec4 vf0 = e::vf(ctx, 0);
            Vec4 v[4];
            for (uint32_t k = 0; k < 4u; ++k)
                v[k] = e::read128(env, quad + 16u * k);
            const PlaneHit h = planeHit(vf0, e::read128(env, s), e::read128(env, en), e::read128(env, n), v[0],
                                        ctx->vu0_status);
            if (!h.ok)
                return false;
            int inside = 0, calls = 1 + 4;
            if (h.hit)
            {
                // the hit buffer: vf0 (w = 1.0), x / z / y from 0x135EC8
                const QuadInside q = quadInside(vf0, v, blend<7>(vf0, h.point), h.status);
                if (!q.ok)
                    return false;
                env.checkpoint(32u);
                calls += 1 + q.calls;
                inside = q.inside;
            }
            for (int k = 0; k < calls; ++k)
                env.call();
            if (inside && e::u32(ctx, e::t0) != 0u)
                writeXzy(env, out, h.point);
            e::setS32(ctx, e::v0, static_cast<uint32_t>(inside));
            returnFrom(ctx);
            return true;
        }

        // ==== C2: the grid (claude/rung4-collision-map.md "C2") ==========================================================
        // caller_scan (ELF + 81 DLLs): 0x12F288 3, 0x12F4C8 3, 0x12F600 1, 0x12F750 3, 0x136458 1, 0x136030 1 sites; none
        // reads a scratch register after the call, no j, no code pointer.
        //
        // The grid functions run collision_core.h's bodies on GridMem: every access in the asm's order through the env,
        // declining (ok false) one outside plain RAM, misaligned for its width or in the frames' window; each write keeps
        // its old bytes and a decline puts them back newest first (the original then runs on RAM as it was). Reads keep
        // nothing. The callees run inline as their natives' bodies, with their checks.
        struct GridJournal
        {
            struct Entry
            {
                uint32_t at;
                uint32_t size;
                uint32_t old; // offset into bytes
            };
            std::vector<Entry> entries;
            std::vector<uint8_t> bytes;
            bool busy = false;
        };
        GridJournal &gridJournal()
        {
            thread_local GridJournal journal;
            return journal;
        }
        template <class Env>
        struct GridMem
        {
            Env &env;
            uint32_t low, size; // the frames' window (RAM offsets); size 0: a leaf, none
            bool ok = true;
            GridJournal *journal = nullptr;
            GridMem(Env &e, uint32_t windowLow, uint32_t windowSize) : env(e), low(windowLow), size(windowSize)
            {
                GridJournal &j = gridJournal();
                if (j.busy)
                    ok = false;
                else
                {
                    j.busy = true, journal = &j;
                    j.entries.clear(), j.bytes.clear();
                }
            }
            ~GridMem()
            {
                if (!journal)
                    return;
                if (!ok)
                    for (size_t i = journal->entries.size(); i-- > 0u;)
                    {
                        const GridJournal::Entry &w = journal->entries[i];
                        std::memcpy(env.at(w.at, w.size), journal->bytes.data() + w.old, w.size);
                    }
                journal->busy = false;
            }
            GridMem(const GridMem &) = delete;
            GridMem &operator=(const GridMem &) = delete;
            bool where(uint32_t address, uint32_t bytes, uint32_t &offset)
            {
                if (!ok)
                    return false;
                if ((address & (bytes - 1u)) != 0u) // misaligned for its width
                    ok = false;
                else if (!plainRam(address, bytes, offset)) // not plain RAM
                    ok = false;
                else if (size != 0u && e::overlaps(offset, bytes, low, size)) // in the frames' window
                    ok = false;
                return ok;
            }
            uint8_t *keep(uint32_t offset, uint32_t bytes)
            {
                uint8_t *p = env.at(offset, bytes);
                journal->entries.push_back({offset, bytes, static_cast<uint32_t>(journal->bytes.size())});
                journal->bytes.insert(journal->bytes.end(), p, p + bytes);
                return p;
            }
            uint32_t lw(uint32_t a)
            {
                uint32_t o;
                return where(a, 4u, o) ? e::read32(env, o) : 0u;
            }
            uint32_t lhu(uint32_t a)
            {
                uint32_t o;
                uint16_t v = 0;
                if (where(a, 2u, o))
                    std::memcpy(&v, env.at(o, 2u), 2);
                return v;
            }
            uint32_t lbu(uint32_t a)
            {
                uint32_t o;
                return where(a, 1u, o) ? *env.at(o, 1u) : 0u;
            }
            Vec4 lq(uint32_t a)
            {
                uint32_t o;
                return where(a & ~0xFu, 16u, o) ? e::read128(env, o) : splat(0.0f);
            }
            void sw(uint32_t a, uint32_t v)
            {
                uint32_t o;
                if (where(a, 4u, o))
                    std::memcpy(keep(o, 4u), &v, 4);
            }
            void sb(uint32_t a, uint32_t v)
            {
                uint32_t o;
                if (where(a, 1u, o))
                    *keep(o, 1u) = static_cast<uint8_t>(v);
            }
            void sq(uint32_t a, Vec4 v)
            {
                uint32_t o;
                if (where(a & ~0xFu, 16u, o))
                    store(keep(o, 16u), v);
            }
            void call() { env.call(); }
            void loop() { env.checkpoint(32u); }
        };

        // ---- func_0012F288 (scroll a0, boxMin a1, boxMax a2; src/cod/0012f288.c): v0 = the query box covers cells ------
        // A 0x40 frame (its callees are leaves).
        constexpr uint32_t kGridRange = 0x12F288u, kGridRangeFrame = 0x40u;
        template <class Env>
        bool gridRangeNative(Env &env, R5900Context *ctx)
        {
            uint32_t low;
            if (!env.atEnd || !frameWindow(ctx, kGridRangeFrame, low))
                return false;
            GridMem<Env> m(env, low, kGridRangeFrame);
            const uint32_t r = gridRange(m, e::u32(ctx, e::a0), e::u64(ctx, e::a1), e::u64(ctx, e::a2));
            if (!m.ok)
                return false;
            e::setS32(ctx, e::v0, r);
            returnFrom(ctx);
            return true;
        }

        // ---- func_0012F4C8 (scroll a0, x0 a1, x1 a2, y0 a3, y1 t0, z0 t1, z1 t2, mask t3; src/cod/0012f4c8.c) ------------
        // A 0xC0 frame (0x12F600 is a leaf). The loops compare the 64-bit registers: words sign-extended (as lw leaves
        // them) are u32 compares, anything else declines; a walk past kMaxGridCells declines.
        constexpr uint32_t kGridClear = 0x12F4C8u, kGridClearFrame = 0xC0u;
        template <class Env>
        bool gridClearNative(Env &env, R5900Context *ctx)
        {
            uint32_t low;
            if (!env.atEnd || !frameWindow(ctx, kGridClearFrame, low))
                return false;
            static constexpr int kRegs[6] = {e::a1, e::a2, e::a3, e::t0, e::t1, e::t2};
            uint32_t r[6];
            for (int k = 0; k < 6; ++k)
            {
                r[k] = e::u32(ctx, kRegs[k]);
                if (e::u64(ctx, kRegs[k]) != signExtend(r[k]))
                    return false;
            }
            if (!gridWalkBounded(r[0], r[1], r[2], r[3], r[4], r[5]))
                return false;
            GridMem<Env> m(env, low, kGridClearFrame);
            clearRange(m, e::u32(ctx, e::a0), r[0], r[1], r[2], r[3], r[4], r[5], e::u32(ctx, e::t3));
            if (!m.ok)
                return false;
            returnFrom(ctx);
            return true;
        }

        // ---- func_0012F600 (scroll a0, cell a1, mask a2; src/cod/0012f600.c): a leaf, no stack access -------------------
        constexpr uint32_t kGridSeen = 0x12F600u;
        template <class Env>
        bool gridSeenNative(Env &env, R5900Context *ctx)
        {
            if (!env.atEnd)
                return false;
            GridMem<Env> m(env, 0u, 0u);
            clearSeen(m, e::u32(ctx, e::a0), e::u32(ctx, e::a1), e::u32(ctx, e::a2));
            if (!m.ok)
                return false;
            returnFrom(ctx); // its first branch passed a real delay slot
            return true;
        }

        // ---- func_0012F750 (scroll a0, polygon id a1; asm): one polygon of the query ----------------------------------
        // A 0x150 frame; the deepest callee HitProbe 0x40 + 0x150 (0x136458's 0xC0 + 0xC0 less).
        constexpr uint32_t kPolygon = 0x12F750u, kPolygonWindow = 0x150u + kHitProbeWindow;
        template <class Env>
        bool polygonNative(Env &env, R5900Context *ctx)
        {
            uint32_t low;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kPolygonWindow, low))
                return false;
            GridMem<Env> m(env, low, kPolygonWindow);
            polygonTest(m, e::vf(ctx, 0), ctx->vu0_status, e::u32(ctx, e::a0), e::u32(ctx, e::a1));
            if (!m.ok)
                return false;
            returnFrom(ctx);
            return true;
        }

        // ---- func_00136458 (start a0, end a1, tri a2: three quadwords, out a3; asm): v0 = the segment hits the triangle -
        // A 0xC0 frame, its deepest callee 0x136030's 0xC0. Inputs by lqc2 (& ~0xF), out by swc1 x, z, y.
        constexpr uint32_t kTriProbe = 0x136458u, kTriProbeWindow = 0xC0u + 0xC0u;
        template <class Env>
        bool triProbeNative(Env &env, R5900Context *ctx)
        {
            uint32_t low, s, en, tri, out = 0;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kTriProbeWindow, low) ||
                !plain16(e::u32(ctx, e::a0), s) || !plain16(e::u32(ctx, e::a1), en) ||
                !plainRam(e::u32(ctx, e::a2) & ~0xFu, 48u, tri) || !outWords(e::u32(ctx, e::a3), low, kTriProbeWindow, out) ||
                e::overlaps(tri, 48u, low, kTriProbeWindow))
                return false;
            for (uint32_t o : {s, en})
                if (e::overlaps(o, 16u, low, kTriProbeWindow))
                    return false;
            Vec4 v[3];
            for (uint32_t k = 0; k < 3u; ++k)
                v[k] = e::read128(env, tri + 16u * k);
            const TriProbe t = triangleProbe(e::vf(ctx, 0), e::read128(env, s), e::read128(env, en), v, ctx->vu0_status);
            if (!t.ok)
                return false;
            for (int k = 0; k < t.calls; ++k)
                env.call();
            if (t.hit && e::u32(ctx, e::a3) != 0u)
                writeXzy(env, out, t.point);
            e::setS32(ctx, e::v0, static_cast<uint32_t>(t.hit));
            returnFrom(ctx);
            return true;
        }

        // ---- func_00136030 (tri a0: three quadwords, p a1; asm): v0 = p inside the triangle ---------------------------
        // A 0xC0 frame (f20 saved); its callees are leaves.
        constexpr uint32_t kTriInside = 0x136030u, kTriInsideFrame = 0xC0u;
        template <class Env>
        bool triInsideNative(Env &env, R5900Context *ctx)
        {
            uint32_t low, tri, pt;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kTriInsideFrame, low) ||
                !plainRam(e::u32(ctx, e::a0) & ~0xFu, 48u, tri) || !plain16(e::u32(ctx, e::a1), pt) ||
                e::overlaps(tri, 48u, low, kTriInsideFrame) || e::overlaps(pt, 16u, low, kTriInsideFrame))
                return false;
            Vec4 v[3];
            for (uint32_t k = 0; k < 3u; ++k)
                v[k] = e::read128(env, tri + 16u * k);
            const TriInside t = triangleInside(e::vf(ctx, 0), v, e::read128(env, pt), ctx->vu0_status);
            if (!t.ok)
                return false;
            for (int k = 0; k < t.calls; ++k)
                env.call();
            e::setS32(ctx, e::v0, static_cast<uint32_t>(t.inside));
            returnFrom(ctx);
            return true;
        }

        // ==== C3: the solids and the query (claude/rung4-collision-map.md "C3") =========================================
        // caller_scan (ELF + 81 DLLs): 0x130700 1, 0x12ED60 5 sites; none reads a scratch register after the call, no j,
        // no code pointer. Both run collision_core.h's bodies on GridMem with their callees inline (each step as its
        // native checks it), so a step reads what the earlier ones wrote and a decline puts every write back.

        // ---- func_00130700 (scroll a0, the solids' outputs: list a1, capacity a2, count a3; asm): nothing returned -----
        // A 0x160 frame; its deepest callee HitProbe 0x40 + 0x150.
        constexpr uint32_t kSolids = 0x130700u, kSolidsWindow = 0x160u + kHitProbeWindow;
        template <class Env>
        bool solidsNative(Env &env, R5900Context *ctx)
        {
            uint32_t low;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kSolidsWindow, low))
                return false;
            GridMem<Env> m(env, low, kSolidsWindow);
            solidsTest(m, e::vf(ctx, 0), ctx->vu0_status, e::u32(ctx, e::a0), e::u32(ctx, e::a1), e::u32(ctx, e::a2),
                       e::u32(ctx, e::a3));
            if (!m.ok)
                return false;
            returnFrom(ctx);
            return true;
        }

        // ---- func_0012ED60 (scroll a0, start a1, end a2, outPoint a3, outNormal t0, mask t1, exclude t2, hits t3, the
        // stack: capacity, outCount, the solids' outputs, byte stepped; asm): v0 = the hit byte ------------------------------
        // A 0xE0 frame; its deepest callee func_00130700 (0x160 + HitProbe's 0x190; 0x12F750's 0x150 + 0x190, the thunk's 0x10
        // less).
        constexpr uint32_t kQuery = 0x12ED60u, kQueryWindow = 0xE0u + kSolidsWindow;
        template <class Env>
        bool queryNative(Env &env, R5900Context *ctx)
        {
            uint32_t low;
            if (!env.atEnd || !e::vf0Constant(ctx) || !frameWindow(ctx, kQueryWindow, low))
                return false;
            GridMem<Env> m(env, low, kQueryWindow);
            const uint32_t r = queryTest(m, e::vf(ctx, 0), ctx->vu0_status, e::u32(ctx, e::a0), e::u64(ctx, e::a1),
                                         e::u64(ctx, e::a2), e::u32(ctx, e::a3), e::u32(ctx, e::t0), e::u32(ctx, e::t1),
                                         e::u32(ctx, e::t2), e::u32(ctx, e::t3), e::u32(ctx, e::sp));
            if (!m.ok)
                return false;
            e::setS32(ctx, e::v0, r);
            returnFrom(ctx);
            return true;
        }
    }

    void registerFunctions()
    {
        // C1 (Nov 27): the probes' leaves; checked since K's 21:48 Build and Replay of 3151fdc shadowed them (0x133408
        // 15.22M, 0x133948 10.96M, 0x135EC8 2.56M, HitProbe 2.29M, 0x1361A0 1.61M calls compared, 0 mismatches). The
        // 564 0x135EC8 / HitProbe declines (the roulette cases) are the normalize's own: a quad with a zero normal (not a
        // stage polygon, whose facing test skips it: most likely a hit box's face, func_001F92C0's HitProbe call).
        auto add = [](const char *name, uint32_t address, bool (*real)(RealEnv &, R5900Context *),
                      bool (*shadow)(ShadowEnv &, R5900Context *), bool checked, uint32_t returns, uint32_t maxRanges = 0u,
                      uint32_t maxBytes = 0u) {
            NativeFunction fn{name, address, "collision", checked, real, shadow, nullptr};
            fn.flags = NativeFunction::kRenderPath | NativeFunction::kChargeAtEnd;
            fn.returns = returns;
            fn.maxRanges = maxRanges;
            fn.maxBytes = maxBytes;
            registerFunction(fn);
        };
        add("func_00133948", kBoxSort, &boxSortNative<RealEnv>, &boxSortNative<ShadowEnv>, true, 0u);
        add("func_00133408", kOverlap, &overlapNative<RealEnv>, &overlapNative<ShadowEnv>, true, NativeFunction::kReturnsV0);
        add("func_00135EC8", kPlaneHit, &planeHitNative<RealEnv>, &planeHitNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
        add("func_001361A0", kQuadInside, &quadInsideNative<RealEnv>, &quadInsideNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
        add("HitProbe_probeHitPoint", kHitProbe, &hitProbeNative<RealEnv>, &hitProbeNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
        // C2 (Nov 28): the grid; checked (K's plain Build and Replay of 87a7f9f, the 06:56 build: 25.96M calls compared,
        // 0 mismatches, 0 declines).
        add("func_00136030", kTriInside, &triInsideNative<RealEnv>, &triInsideNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
        add("func_00136458", kTriProbe, &triProbeNative<RealEnv>, &triProbeNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
        add("func_0012F288", kGridRange, &gridRangeNative<RealEnv>, &gridRangeNative<ShadowEnv>, true,
            NativeFunction::kReturnsV0);
        // their recorded footprints: a polygon's flags word per range (a cell lists up to 3 x 255; a room's whole grid
        // ~14,000 ids in limit_break_test_room_loads), so the shadow harness keeps more than the default 256 ranges
        add("func_0012F600", kGridSeen, &gridSeenNative<RealEnv>, &gridSeenNative<ShadowEnv>, true, 0u, 1024u);
        add("func_0012F4C8", kGridClear, &gridClearNative<RealEnv>, &gridClearNative<ShadowEnv>, true, 0u, 65536u,
            1024u * 1024u);
        add("func_0012F750", kPolygon, &polygonNative<RealEnv>, &polygonNative<ShadowEnv>, true, 0u, 1024u);
        // C3 (Nov 28): the solids and the query; checked (K's call). His plain Build and Replay of ab99e29 (the 11:45 build):
        // 3.80M calls compared, 0 mismatches, 0 declines, but 7 case lines' chains moved while they were shadowed, the same
        // in two runs, cause not found (claude/rung4-collision-map.md "C3: shadowed"); this live run decides. 0x12ED60's
        // footprint: a whole room's seen bits (as 0x12F4C8's) and the polygons, vertices and solids it reads.
        add("func_00130700", kSolids, &solidsNative<RealEnv>, &solidsNative<ShadowEnv>, true, 0u, 1024u);
        add("func_0012ED60", kQuery, &queryNative<RealEnv>, &queryNative<ShadowEnv>, true, NativeFunction::kReturnsV0,
            65536u, 1024u * 1024u);
    }
}
