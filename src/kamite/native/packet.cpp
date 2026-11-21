// Native packet building (rung 3, claude/native-next.md): the EE code that builds the DMA chain the three native VU1
// programs read. Step a writes the same packets natively, function by function from the leaves up (K's order, Nov 21:
// claude/rung3-step-a-handoff.md); switch KAMITE_NATIVE_PACKET (unset = the checked ones, 1 = all, 0 = none). Each
// run<Env>() is the recompiled function's effect (SLUS_215.03): the same loads and stores in the same order, the same
// registers left behind (ee_ctx.h: how the generated code writes them), the guest calls' cycles; footprints recorded.

#include "kamite/native/native.h"
#include "kamite/native/ee_ctx.h"

namespace kamite::native::packet
{
    namespace
    {
        namespace e = kamite::native::ee;

        // ---- func_0014FF00: link a packet into an ordering-table bucket ----------------------------------------------
        // a0 = the bucket list's descriptor (+0 the slots, +0x20 the first index), a1 = the index, a2 = the packet,
        // a3 = its last tag. The slot (index clamped to the first) becomes a NEXT tag to the packet; the packet's last
        // tag becomes a NEXT to what the slot linked to before (its QWC / flags kept). The four accesses' offsets:
        struct Link
        {
            uint32_t desc0, desc20, slot, tail;
        };
        constexpr uint32_t kLink = 0x14FF00u;

        // Where the link reads and writes, from the registers it will be called with (a0, a1 whole, a3); false when
        // any of it is outside plain RAM.
        template <class Env>
        bool linkPlan(Env &env, uint32_t a0, uint64_t a1, uint32_t a3, Link &link)
        {
            if (!plainRam(a0 + 0x20u, 4u, link.desc20) || !plainRam(a0, 4u, link.desc0) || !plainRam(a3, 8u, link.tail))
                return false;
            const uint64_t first = static_cast<uint64_t>(static_cast<int64_t>(static_cast<int32_t>(e::read32(env, link.desc20))));
            const uint32_t slots = e::read32(env, link.desc0);
            const uint64_t lowest = first < a1 ? first : a1;
            const uint32_t slot = slots + ((static_cast<uint32_t>(first) - static_cast<uint32_t>(lowest)) << 4);
            return plainRam(slot, 8u, link.slot); // lwu slot + 4, sd slot
        }

        // The link on the context (ctx->pc = ra after it), its plan made from the same registers.
        template <class Env>
        void linkOn(Env &env, R5900Context *ctx, const Link &link)
        {
            e::setS32(ctx, e::v1, e::read32(env, link.desc20));
            e::setS32(ctx, e::t1, 0x20000000u);
            e::setS32(ctx, e::t0, e::read32(env, link.desc0));
            e::setLow64(ctx, e::a2, e::u64(ctx, e::a2) << 32);
            e::setLow64(ctx, e::a0, e::u64(ctx, e::v1) < e::u64(ctx, e::a1) ? 1u : 0u);
            e::move(ctx, e::v0, e::v1);
            if (e::u64(ctx, e::a0) == 0u)
                e::moveVec(ctx, e::v0, e::a1);
            e::setLow64(ctx, e::a2, e::u64(ctx, e::a2) | e::u64(ctx, e::t1));
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) - e::u32(ctx, e::v0));
            e::setS32(ctx, e::a1, 0x0FFF0000u);
            e::setLow64(ctx, e::a1, e::u64(ctx, e::a1) | 0xFFFFu);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) << 4);
            e::setS32(ctx, e::t0, e::u32(ctx, e::t0) + e::u32(ctx, e::v1));
            const uint32_t slot = link.slot;
            e::setLow64(ctx, e::a0, e::read32(env, (slot + 4u) & PS2_RAM_MASK)); // lwu
            e::write64(env, slot, e::u64(ctx, e::a2));
            e::setLow64(ctx, e::a0, e::u64(ctx, e::a0) << 32);
            e::setLow64(ctx, e::v0, e::read64(env, link.tail));
            e::setLow64(ctx, e::v0, e::u64(ctx, e::v0) & e::u64(ctx, e::a1));
            e::setLow64(ctx, e::v0, e::u64(ctx, e::v0) | e::u64(ctx, e::t1));
            e::setLow64(ctx, e::v0, e::u64(ctx, e::v0) | e::u64(ctx, e::a0));
            const uint32_t target = e::u32(ctx, e::ra);
            e::write64(env, link.tail, e::u64(ctx, e::v0)); // the jr's delay slot
            e::jrRa(ctx, 0x14FF50u, target, true);
        }

        template <class Env>
        bool link(Env &env, R5900Context *ctx)
        {
            Link plan;
            if (!linkPlan(env, e::u32(ctx, e::a0), e::u64(ctx, e::a1), e::u32(ctx, e::a3), plan))
                return false;
            linkOn(env, ctx, plan);
            return true;
        }

        // ---- func_0031A650 / func_0031A600: queue a packet into bucket a1 (< 10) of the frame's table a0 ----------
        // The frame's half (the word at 0x7476B0 & 1) picks the descriptor at a0 + 0xB00 + a1 * 8 + half * 4; then
        // 0x14FF00. 0x31A650 (a2 = the index, a3 = the packet, t0 = its last tag, or a3's when t0 is 0); 0x31A600
        // (a2 = the index, a3 = the packet and its last tag).
        constexpr uint32_t kQueue = 0x31A650u, kQueueOne = 0x31A600u, kFrameHalf = 0x7476B0u;

        // The frame's [sp - 16, sp) slot: plain and clear of what the link touches.
        bool queueFrame(const R5900Context *ctx, uint32_t &frame)
        {
            return plainRam(e::u32(ctx, e::sp) - 16u, 8u, frame);
        }
        bool clearOf(uint32_t frame, const Link &link)
        {
            return !e::overlaps(frame, 8u, link.desc0, 4u) && !e::overlaps(frame, 8u, link.desc20, 4u) &&
                   !e::overlaps(frame, 8u, link.slot, 8u) && !e::overlaps(frame, 8u, link.tail, 8u);
        }
        // The descriptor: the lw in the jal's delay slot, at a0 + 0xB00 + half * 4 + a1 * 8 (the code's 32-bit sums).
        // Both reads come after the frame's sd ra: keep them clear of it.
        template <class Env>
        bool descriptorOf(Env &env, uint32_t frame, uint32_t a0, uint32_t a1, uint32_t &descriptor)
        {
            uint32_t half, at;
            if (!plainRam(kFrameHalf, 4u, half) || e::overlaps(frame, 8u, half, 4u))
                return false;
            const uint32_t where = a0 + (((e::read32(env, half) & 1u) << 2) + (a1 << 3));
            if (!plainRam(where + 0xB00u, 4u, at) || e::overlaps(frame, 8u, at, 4u))
                return false;
            descriptor = e::read32(env, at);
            return true;
        }
        template <class Env>
        void leaveQueue(Env &env, R5900Context *ctx, uint32_t frame, uint32_t jrSite)
        {
            e::setLow64(ctx, e::ra, e::read64(env, frame));
            const uint32_t target = e::u32(ctx, e::ra);
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) + 16u);
            e::jrRa(ctx, jrSite, target, true);
        }

        template <class Env>
        bool queue(Env &env, R5900Context *ctx)
        {
            uint32_t frame;
            if (!queueFrame(ctx, frame))
                return false;
            const bool inRange = e::s64(ctx, e::a1) < 10;
            Link plan{};
            if (inRange)
            {
                // the registers 0x14FF00 will get: a0 = the descriptor, a1 = a2, a2 = a3, a3 = t0 (a3 when t0 is 0)
                uint32_t descriptor;
                if (!descriptorOf(env, frame, e::u32(ctx, e::a0), e::u32(ctx, e::a1), descriptor))
                    return false;
                const uint32_t tail = e::u64(ctx, e::t0) == 0u ? e::u32(ctx, e::a3) : e::u32(ctx, e::t0);
                if (!linkPlan(env, descriptor, e::u64(ctx, e::a2), tail, plan) || !clearOf(frame, plan))
                    return false;
            }
            e::move(ctx, e::t1, e::a2);
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) - 16u);
            e::move(ctx, e::a2, e::a3);
            e::write64(env, frame, e::u64(ctx, e::ra));
            e::move(ctx, e::t2, e::a0);
            e::setLow64(ctx, e::v0, inRange ? 1u : 0u);
            e::move(ctx, e::a3, e::t0); // the beqz's delay slot
            e::delaySlot(ctx, 0x31A668u);
            if (!inRange)
            {
                leaveQueue(env, ctx, frame, 0x31A6A4u);
                return true;
            }
            e::setS32(ctx, e::v0, 0x00740000u);
            e::setS32(ctx, e::a0, e::u32(ctx, e::a1) << 3);
            e::setS32(ctx, e::v1, e::read32(env, kFrameHalf & PS2_RAM_MASK));
            if (e::u64(ctx, e::a3) == 0u)
                e::moveVec(ctx, e::t0, e::a2);
            e::move(ctx, e::a3, e::t0);
            e::move(ctx, e::a1, e::t1);
            e::setLow64(ctx, e::v1, e::u64(ctx, e::v1) & 1u);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) << 2);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) + e::u32(ctx, e::a0));
            e::setS32(ctx, e::v1, e::u32(ctx, e::t2) + e::u32(ctx, e::v1));
            e::jal(ctx, 0x31A698u, kLink, true);
            e::setS32(ctx, e::a0, e::read32(env, (e::u32(ctx, e::v1) + 0xB00u) & PS2_RAM_MASK));
            if (env.call())
                return true;
            linkOn(env, ctx, plan);
            leaveQueue(env, ctx, frame, 0x31A6A4u);
            return true;
        }

        template <class Env>
        bool queueOne(Env &env, R5900Context *ctx)
        {
            uint32_t frame;
            if (!queueFrame(ctx, frame))
                return false;
            const bool inRange = e::s64(ctx, e::a1) < 10;
            Link plan{};
            if (inRange)
            {
                // 0x14FF00 gets a0 = the descriptor, a1 = a2, a2 = a3, a3 = a3
                uint32_t descriptor;
                if (!descriptorOf(env, frame, e::u32(ctx, e::a0), e::u32(ctx, e::a1), descriptor))
                    return false;
                if (!linkPlan(env, descriptor, e::u64(ctx, e::a2), e::u32(ctx, e::a3), plan) || !clearOf(frame, plan))
                    return false;
            }
            e::setS32(ctx, e::sp, e::u32(ctx, e::sp) - 16u);
            e::move(ctx, e::t0, e::a2);
            e::write64(env, frame, e::u64(ctx, e::ra));
            e::move(ctx, e::t1, e::a0);
            e::setLow64(ctx, e::v0, inRange ? 1u : 0u);
            e::move(ctx, e::a2, e::a3); // the beqz's delay slot
            e::delaySlot(ctx, 0x31A614u);
            if (!inRange)
            {
                leaveQueue(env, ctx, frame, 0x31A648u);
                return true;
            }
            e::setS32(ctx, e::v0, 0x00740000u);
            e::setS32(ctx, e::a0, e::u32(ctx, e::a1) << 3);
            e::setS32(ctx, e::v1, e::read32(env, kFrameHalf & PS2_RAM_MASK));
            e::move(ctx, e::a1, e::t0);
            e::setLow64(ctx, e::v1, e::u64(ctx, e::v1) & 1u);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) << 2);
            e::setS32(ctx, e::v1, e::u32(ctx, e::v1) + e::u32(ctx, e::a0));
            e::setS32(ctx, e::v1, e::u32(ctx, e::t1) + e::u32(ctx, e::v1));
            e::jal(ctx, 0x31A63Cu, kLink, true);
            e::setS32(ctx, e::a0, e::read32(env, (e::u32(ctx, e::v1) + 0xB00u) & PS2_RAM_MASK));
            if (env.call())
                return true;
            linkOn(env, ctx, plan);
            leaveQueue(env, ctx, frame, 0x31A648u);
            return true;
        }
    }

    void registerFunctions()
    {
        // Step a, batch 1: checked since K's 16:44 build of e881eeb (shadowed: 0 mismatches over ~20M calls, every
        // .ram.bin the reference's); recorded footprints.
        registerFunction(NativeFunction{"func_0014FF00", kLink, "packet", true, &link<RealEnv>, &link<ShadowEnv>, nullptr});
        registerFunction(NativeFunction{"func_0031A650", kQueue, "packet", true, &queue<RealEnv>, &queue<ShadowEnv>, nullptr});
        registerFunction(NativeFunction{"func_0031A600", kQueueOne, "packet", true, &queueOne<RealEnv>, &queueOne<ShadowEnv>,
                                        nullptr});
    }
}
