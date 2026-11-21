// Native VU1 program: the clip scenery program (spec: claude/native-vu1.md "Clip scenery"). The game's microprogram at
// 0x00C8 of the scenery image (b847; uploaded with its init program at 0x0000) draws one batch of a static mesh as a
// triangle strip, like the scenery strip (scenery_strip.cpp), and cuts the triangles that cross the clip volume with
// a polygon clipper of its own (0x0448: Sutherland-Hodgman against the six planes, then the piece as a fan in a packet
// of its own at data qword 30). A triangle entirely outside one plane, or with a corner past the far plane, is dropped.
//
// Two passes: the strip (pure maths: every vertex's outputs and its path through the program, so a run can still be
// declined before anything changes), then the writes in the program's order, with the clipper for the triangles that
// need it. The clipper is mirrored step by step on VU data memory (its polygon buffers, register stack and packet are
// left there as the microprogram leaves them); it runs for a few percent of the vertices.
//
// Layers: decode (the init program's registers, the batch at TOP) / transform (pure maths) / emit (GS packets in VU
// memory, then XGKICK). Exactness: every float operation is the microprogram's own, in its order, in the VU's float
// environment (vu_vec.h); the back-face signs and the flags the run ends with are taken from the exact results, as the
// runtime's VU does. After the batch the VU state is left exactly as the microprogram leaves it.
#include "kamite/render/vu1_programs.h"
#include "kamite/native/vu_vec.h"

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"

#include <cfloat>
#include <cmath>
#include <cstring>
#include <utility>

namespace kamite::render
{
    namespace
    {
        // ---- decode -------------------------------------------------------------------------------------------------

        constexpr int32_t kOutputOffset = 0xD0;   // the strip's packet is built at TOP + 0xD0
        constexpr int32_t kFogBaseQword = 8;      // data qwords the program reads (the init program loads 0..15)
        constexpr int32_t kFogScaleQword = 9;
        constexpr int32_t kPolyMatrixQword = 16;  // 16..19: clip space -> screen, for the clipped pieces
        constexpr int32_t kPrev1Qword = 20;       // the last two screen positions, carried from batch to batch
        constexpr int32_t kPrev2Qword = 21;
        constexpr int32_t kPolyPacketQword = 30;  // the batch's second header qword (a GIF tag); the pieces' packet
        constexpr int32_t kPolyA = 0x3D;          // the clipper's two polygon buffers (3 qwords a vertex)
        constexpr int32_t kPolyB = 0x65;
        constexpr int32_t kStackTop = 0x400;      // the register stack (0x3F0..0x3FF)
        constexpr int32_t kLowEnd = 0x84;         // qwords below this, and the stack, are the program's own
        constexpr uint32_t kKickPc = 0x0340u;     // the strip's XGKICK
        constexpr uint32_t kTagKickPc = 0x0CE0u;  // the clipper's first XGKICK (the tag alone)
        constexpr uint32_t kPolyKickPc = 0x0F90u; // the clipper's piece
        constexpr uint32_t kEndPc = 0x0358u;      // after [E] (0x0348): "b 0x00C8", the MSCNT entry
        constexpr float kPositionScale = 0.01f;   // LOI 0.01 (the positions are integers)
        constexpr float kColourMax = 255.0f;      // LOI 255

        // Cycles (the runtime's count; pairs issued plus the stalls the interpreter takes, which are fixed per
        // instruction in this program): see "Cycles" in the spec.
        constexpr uint64_t kCyclesPrologue = 44;      // 0x00C8..0x01E0
        constexpr uint64_t kCyclesMscnt = 2;          // 0x0358 "b 0x00C8" + its delay slot
        constexpr uint64_t kCyclesDraw = 40;          // a vertex whose triangle is drawn
        constexpr uint64_t kCyclesFlagged = 39;       // ADC from the data
        constexpr uint64_t kCyclesBackface = 43;      // ADC: facing away
        constexpr uint64_t kCyclesClipCull = 41;      // + 3 per clip test made, + 1 from the third test on
        constexpr uint64_t kCyclesToClipper = 91;     // 0x01E8..0x0410 with all six tests and the register save
        constexpr uint64_t kCyclesFromClipper = 43;   // 0x0418..0x0338 with the register restore
        constexpr uint64_t kCyclesClipperBound = 2400; // one clipper call at most (convex pieces; see the spec)

        struct Memory
        {
            uint8_t *data;
            uint8_t *qw(int32_t address) const { return data + ((static_cast<uint32_t>(address) & 0x3FFu) << 4); }
            Vec4 load(int32_t address) const { return kamite::render::load(qw(address)); }
            void put(int32_t address, Vec4 v) const { kamite::render::store(qw(address), v); }
            uint32_t word(int32_t address, int lane) const
            {
                uint32_t w;
                std::memcpy(&w, qw(address) + 4 * lane, 4);
                return w;
            }
            void setWord(int32_t address, int lane, uint32_t w) const { std::memcpy(qw(address) + 4 * lane, &w, 4); }
        };

        inline Vec4 reg(const VU1State &st, int r) { return kamite::render::load(st.vf[r]); }
        inline void setReg(VU1State &st, int r, Vec4 v) { kamite::render::store(st.vf[r], v); }
        inline int32_t s16(int32_t v) { return static_cast<int16_t>(v); }

        // Loaded by the init program (0x0000) from data qwords 0..15 and kept in registers across runs.
        struct Constants
        {
            Vec4 toScreen[4];  // vf01-vf04: object -> homogeneous screen position
            Vec4 toClip[4];    // vf09-vf12: object -> clip space
            Vec4 colourScale;  // vf21
            Vec4 stqOffset;    // vf26
            Vec4 colourMax;    // I = 255, every lane
            Vec4 positionScale; // I = 0.01, every lane
            Vec4 fogBase;      // data qword 8 (w, every lane)
            Vec4 fogScale;     // data qword 9 (w, every lane)
            Vec4 stqIn;        // vf14: .z = 1 (MR32.z vf14, vf00), .w as it was; .xy the vertex's texture coordinates
            Vec4 zeroW;        // vf31 * 0 (MULx vf31, vf31, vf00x): what the clipper adds to every value it copies
            int32_t adcBit;    // vi06 (0x8000)
            int32_t countMask; // vi08 (0x7FFF)
        };

        // GIF packet length in qwords (tag included) from the tag at that address, as the runtime's XGKICK reads it.
        inline uint32_t packetQwords(const Memory &mem, int32_t at)
        {
            const uint64_t tag = static_cast<uint64_t>(mem.word(at, 0)) | (static_cast<uint64_t>(mem.word(at, 1)) << 32);
            const uint64_t nloop = tag & 0x7FFFu;
            const uint32_t format = static_cast<uint32_t>((tag >> 58) & 3u);
            uint64_t nreg = (tag >> 60) & 0xFu;
            if (nreg == 0u)
                nreg = 16u;
            if (format == 0u)
                return static_cast<uint32_t>(1u + nloop * nreg);
            if (format == 1u)
                return static_cast<uint32_t>(1u + ((nloop * nreg + 1u) & ~1ull) / 2u);
            return static_cast<uint32_t>(1u + nloop);
        }

        // ---- transform ----------------------------------------------------------------------------------------------

        // MULAx / MADDAy / MADDAz: (m0 x + m1 y) + m2 z, each step rounded.
        inline Vec4 partial(const Vec4 m[4], Vec4 p)
        {
            Vec4 acc = mul(m[0], splat<0>(p));
            acc = add(acc, mul(m[1], splat<1>(p)));
            return add(acc, mul(m[2], splat<2>(p)));
        }
        // ... then MADDw with vf00w: + m3.
        inline Vec4 transform(const Vec4 m[4], Vec4 p) { return add(partial(m, p), m[3]); }
        // ... or MADDw with the point's own w (the clipper's 4x4 transform).
        inline Vec4 transform4(const Vec4 m[4], Vec4 p) { return add(partial(m, p), mul(m[3], splat<3>(p))); }

        // DIV: num / den; den = 0 gives +-FLT_MAX and the D flag (I for 0 / 0). sticky gathers both.
        inline float divide(float num, float den, uint32_t &sticky)
        {
            num = lane<0>(norm(splat(num)));
            den = lane<0>(norm(splat(den)));
            if (den == 0.0f)
            {
                sticky |= num == 0.0f ? 0x400u : 0x800u;
                return std::signbit(num) != std::signbit(den) ? -FLT_MAX : FLT_MAX;
            }
            return lane<0>(norm(splat(num / den)));
        }
        inline float reciprocal(float w, uint32_t &sticky) { return divide(1.0f, w, sticky); }

        // CLIP c.xyz, c.w: six bits (+x -x +y -y +z -z), compared as sign-magnitude integers (raw bits).
        inline uint32_t clipCode(Vec4 c)
        {
            const uint32_t w = laneBits<3>(c);
            const int32_t limit = (w & 0x7F800000u) != 0u ? static_cast<int32_t>(w & 0x7FFFFFFFu) : 0x007FFFFF;
            const uint32_t lanes[3] = {laneBits<0>(c), laneBits<1>(c), laneBits<2>(c)};
            uint32_t code = 0u;
            for (int i = 0; i < 3; ++i)
            {
                if (static_cast<int32_t>(lanes[i]) > limit)
                    code |= 1u << (2 * i);
                if (static_cast<int32_t>(lanes[i] ^ 0x80000000u) > limit)
                    code |= 2u << (2 * i);
            }
            return code;
        }
        inline uint32_t shiftClip(uint32_t reg, uint32_t code) { return ((reg << 6) | code) & 0xFFFFFFu; }

        // OPMULA acc, a, b: (a.y b.z, a.z b.x, a.x b.y).
        inline Vec4 crossProducts(Vec4 a, Vec4 b)
        {
            const Vec4 l = bitsOf(laneBits<1>(a), laneBits<2>(a), laneBits<0>(a), 0u);
            const Vec4 r = bitsOf(laneBits<2>(b), laneBits<0>(b), laneBits<1>(b), 0u);
            return mul(l, r);
        }
        // The sign flag of OPMSUB's z lane after OPMULA acc, a, b: acc.z - b.x a.y, exactly.
        inline bool crossNegative(float accZ, Vec4 a, Vec4 b)
        {
            return std::signbit(static_cast<double>(accZ) - static_cast<double>(lane<0>(b)) * static_cast<double>(lane<1>(a)));
        }

        // The flags of the run's last FMAC, from its exact result (MAC: zero / sign / underflow / overflow nibbles,
        // x the high bit of each; status: the low four bits), computed once at the end.
        struct LastFmac
        {
            enum Kind : uint8_t
            {
                Add,    // a + b
                Mul,    // a b
                Madd,   // a + b c
                Opmsub, // acc - (fs.y ft.z, fs.z ft.x, fs.x ft.y), xyz
            };
            Kind kind = Add;
            Vec4 a{}, b{}, c{};

            uint32_t flags(uint32_t &statusLow) const
            {
                const float av[4] = {lane<0>(a), lane<1>(a), lane<2>(a), lane<3>(a)};
                const float bv[4] = {lane<0>(b), lane<1>(b), lane<2>(b), lane<3>(b)};
                const float cv[4] = {lane<0>(c), lane<1>(c), lane<2>(c), lane<3>(c)};
                uint32_t mac = 0u;
                for (int l = 0; l < 4; ++l)
                {
                    double e;
                    switch (kind)
                    {
                    case Add:
                        e = static_cast<double>(av[l]) + static_cast<double>(bv[l]);
                        break;
                    case Mul:
                        e = static_cast<double>(av[l]) * static_cast<double>(bv[l]);
                        break;
                    case Madd:
                        e = static_cast<double>(av[l]) + static_cast<double>(bv[l]) * static_cast<double>(cv[l]);
                        break;
                    default:
                    {
                        if (l == 3)
                            continue; // xyz only
                        static constexpr int left[3] = {1, 2, 0}, right[3] = {2, 0, 1};
                        e = static_cast<double>(av[l]) - static_cast<double>(bv[left[l]]) * static_cast<double>(cv[right[l]]);
                        break;
                    }
                    }
                    const double m = std::fabs(e);
                    const uint32_t bit = 8u >> l;
                    const bool small = m < static_cast<double>(FLT_MIN);
                    if (small)
                        mac |= bit;
                    if (std::signbit(e))
                        mac |= bit << 4;
                    if (small && m != 0.0)
                        mac |= bit << 8;
                    if (m > static_cast<double>(FLT_MAX))
                        mac |= bit << 12;
                }
                statusLow = ((mac & 0x000Fu) != 0u ? 1u : 0u) | ((mac & 0x00F0u) != 0u ? 2u : 0u) |
                            ((mac & 0x0F00u) != 0u ? 4u : 0u) | ((mac & 0xF000u) != 0u ? 8u : 0u);
                return mac;
            }
        };

        // A vertex of the strip after the first pass.
        enum Path : uint8_t
        {
            Draw,
            Flagged,  // ADC from the data (position.w bit 15)
            Backface, // ADC: facing away
            Cull1,    // ADC: outside one plane (all three corners -y, +y, -x, +x, -z; or one corner +z): Cull1..Cull6
            Clipped = Cull1 + 6, // cut by the clipper (ADC in the strip)
        };

        struct Placed
        {
            Vec4 stq;     // s q, t q, q; w: the base's
            Vec4 rgba;    // int32
            Vec4 xyzf;    // int32: x, y, z in 1/16 pixel, fog
            Vec4 stqBase; // (s, t, 1, w) + offset: .xy is written back over the input's texture coordinates
            Vec4 screen;  // the perspective-divided position (xyzw), carried in data qword 20
            Vec4 clip;    // the clip-space position
            uint32_t code; // its CLIP flags
            int32_t flags; // the position's w, as ILW reads it
            uint8_t path;
        };

        // ---- the clipper (0x0448), mirrored on VU memory -----------------------------------------------------------

        struct Clipper
        {
            const Memory &mem;
            const Constants &k;
            Vu1NativeRun &r;
            uint64_t &cycles;   // before the current instruction pair
            uint64_t &kickDone; // when the last kicked packet's transfer ends (a later XGKICK waits for it)
            uint32_t &divSticky;
            uint32_t &clip;     // the clip flag register
            Clipper(const Memory &m, const Constants &c, Vu1NativeRun &run, uint64_t &cy, uint64_t &kd, uint32_t &ds, uint32_t &cl)
                : mem(m), k(c), r(run), cycles(cy), kickDone(kd), divSticky(ds), clip(cl)
            {
            }
            // What it leaves in registers it does not restore (the strip overwrites them, but not past the batch):
            Vec4 vf06{}, vf18{}, vf19{}, vf20{}, vf22{}, acc{};
            bool wroteVf06 = false, wroteVf20 = false, wroteAcc = false;
            LastFmac last;

            void kick(int32_t at, uint32_t pc)
            {
                if (cycles < kickDone)
                    cycles = kickDone;
                kickDone = cycles + 2u * packetQwords(mem, at) - 1u;
                r.kick(r.user, static_cast<uint32_t>(at), pc);
            }

            // 0x0BB0: the point between cur (vf17-19) and next (vf21-23) on the plane: d = p - side w, t = |d_cur /
            // (d_next - d_cur)| in the plane's lane, then cur + (next - cur) t for all three qwords.
            void intersect(const Vec4 cur[3], const Vec4 next[3], float side, int lanePlane, Vec4 out[3])
            {
                const Vec4 n0 = norm(next[0]);
                const float wCur = lane<0>(mul(splat<3>(cur[0]), splat(side)));
                const float wNext = lane<0>(mul(splat<3>(n0), splat(side)));
                const Vec4 dCur = sub(cur[0], splat(wCur));
                const Vec4 dNext = sub(n0, splat(wNext));
                float num, nextD;
                switch (lanePlane)
                {
                case 0:
                    num = lane<0>(dCur), nextD = lane<0>(dNext);
                    break;
                case 1:
                    num = lane<1>(dCur), nextD = lane<1>(dNext);
                    break;
                default:
                    num = lane<2>(dCur), nextD = lane<2>(dNext);
                    break;
                }
                const float den = lane<0>(sub(splat(nextD), splat(num)));
                const float q = divide(num, den, divSticky);
                const Vec4 t = splat(std::fabs(lane<0>(add(splat(0.0f), splat(q)))));
                for (int j = 0; j < 3; ++j)
                {
                    const Vec4 scaled = mul(sub(norm(next[j]), cur[j]), t);
                    out[j] = add(scaled, cur[j]);
                    if (j == 2)
                        last = {LastFmac::Add, scaled, cur[j]};
                }
                cycles += 39u + 8u * static_cast<uint64_t>(lanePlane); // MR32 once per lane past x
            }

            // One pass (0x0588 etc.): the polygon at `in` (count vertices, then the closing copy) against one plane,
            // into `out`, then the closing copy of the output. curMask / nextMask: the plane's bit in FCGET's low 12.
            int32_t pass(int32_t in, int32_t out, int32_t count, uint32_t curMask, uint32_t nextMask, int lanePlane,
                         float side)
            {
                cycles += 11;
                Vec4 next[3] = {mem.load(in), mem.load(in + 1), mem.load(in + 2)};
                int32_t at = in + 3, put = out, made = 0;
                for (int32_t e = 0; e < count; ++e)
                {
                    cycles += 20;
                    Vec4 cur[3];
                    for (int j = 0; j < 3; ++j)
                        cur[j] = add(k.zeroW, norm(next[j]));
                    last = {LastFmac::Add, k.zeroW, norm(next[2])};
                    for (int j = 0; j < 3; ++j)
                        next[j] = mem.load(at + j);
                    at += 3;
                    clip = shiftClip(shiftClip(clip, clipCode(cur[0])), clipCode(next[0]));
                    const uint32_t flags = clip & 0xFFFu;
                    const bool curOut = (flags & curMask) != 0u, nextOut = (flags & nextMask) != 0u;
                    Vec4 cut[3];
                    if (curOut)
                    {
                        if (nextOut)
                            cycles += 8;
                        else
                        {
                            intersect(cur, next, side, lanePlane, cut);
                            for (int j = 0; j < 3; ++j)
                                mem.put(put + j, cut[j]);
                            put += 3, made += 1;
                            cycles += 14;
                        }
                    }
                    else
                    {
                        for (int j = 0; j < 3; ++j)
                            mem.put(put + j, cur[j]);
                        put += 3, made += 1;
                        if (nextOut)
                        {
                            intersect(cur, next, side, lanePlane, cut);
                            for (int j = 0; j < 3; ++j)
                                mem.put(put + j, cut[j]);
                            put += 3, made += 1;
                            cycles += 17;
                        }
                        else
                            cycles += 10;
                    }
                    vf18 = cur[1];
                    vf19 = cur[2];
                }
                // 0x09E8: the output's first vertex again after its last (loaded, then stored)
                const Vec4 first[3] = {mem.load(out), mem.load(out + 1), mem.load(out + 2)};
                for (int j = 0; j < 3; ++j)
                    mem.put(put + j, first[j]);
                vf22 = first[1];
                cycles += 14;
                return made;
            }

            // 0x0CB8: the piece as a fan, unless it faces away.
            void emit(int32_t count)
            {
                cycles += 5;
                mem.setWord(kPolyPacketQword, 0, 0x8000u); // NLOOP 0, EOP: the tag alone (its PRIM)
                kick(kPolyPacketQword, kTagKickPc);
                Vec4 m[4];
                for (int j = 0; j < 4; ++j)
                    m[j] = norm(mem.load(kPolyMatrixQword + j));
                const Vec4 v0 = norm(mem.load(kPolyA)), v1 = norm(mem.load(kPolyA + 3)), v2 = norm(mem.load(kPolyA + 6));
                const Vec4 h0 = transform4(m, v0), h1 = transform4(m, v1);
                const Vec4 part2 = partial(m, v2);
                const Vec4 h2 = add(part2, mul(m[3], splat<3>(v2)));
                const Vec4 s0 = mul(h0, splat(reciprocal(lane<3>(h0), divSticky)));
                const Vec4 s1 = mul(h1, splat(reciprocal(lane<3>(h1), divSticky)));
                const Vec4 s2 = mul(h2, splat(reciprocal(lane<3>(h2), divSticky)));
                const Vec4 a = blend<0x8>(sub(s0, s1), s0); // vf20 (xyz; w: s0's)
                const Vec4 b = blend<0x8>(sub(s2, s1), s2); // vf05
                const Vec4 cross = crossProducts(b, a);     // OPMULA acc, vf05, vf20
                wroteAcc = true, wroteVf20 = true, wroteVf06 = true;
                cycles += 51;
                if (crossNegative(lane<2>(cross), b, a))
                {
                    // OPMSUB vf00, vf20, vf05: the z sign says it faces away.
                    acc = blend<0x7>(part2, cross);
                    vf20 = a;
                    vf06 = s1;
                    last = {LastFmac::Opmsub, cross, a, b};
                    cycles += 7;
                    return;
                }
                mem.setWord(kPolyPacketQword, 0, static_cast<uint32_t>(s16(count + 0x8000)) & 0xFFFFu);
                const Vec4 fogBase = splat<3>(norm(mem.load(kFogBaseQword)));
                const Vec4 fogScaleRaw = mem.load(kFogScaleQword);
                const Vec4 fogScale = splat<3>(norm(fogScaleRaw));
                const uint32_t stqW = laneBits<3>(mem.load(kFogScaleQword)); // vf28.w: data qword 9's (loaded at the start)
                Vec4 h = h0;
                float q = reciprocal(lane<3>(h0), divSticky);
                cycles += 10;
                Vec4 nextPart{}, nextPos{};
                for (int32_t j = 0; j < count; ++j)
                {
                    const int32_t at = kPolyA + 3 * j;
                    const Vec4 colour = norm(mem.load(at + 1));
                    const Vec4 stqIn = norm(blend<0x4>(mem.load(at + 2), splat(1.0f)));
                    const Vec4 qq = splat(q);
                    Vec4 fog = add(fogBase, mul(fogScale, splat<3>(h)));
                    fog = vmax(vmin(fog, k.colourMax), splat(0.0f));
                    const Vec4 xyzf = ftoi4(blend<0x8>(mul(h, qq), fog));
                    const Vec4 rgba = ftoi(colour);
                    const Vec4 stq = blend<0x8>(mul(stqIn, qq), bitsOf(0u, 0u, 0u, stqW));
                    nextPos = mem.load(at + 3);
                    const Vec4 p = norm(nextPos);
                    nextPart = partial(m, p);
                    const Vec4 hn = add(nextPart, mul(m[3], splat<3>(p)));
                    mem.put(kPolyPacketQword + 2 + 3 * j, rgba);
                    mem.put(kPolyPacketQword + 1 + 3 * j, stq);
                    mem.put(kPolyPacketQword + 3 + 3 * j, xyzf);
                    q = reciprocal(lane<3>(hn), divSticky);
                    last = {LastFmac::Madd, nextPart, m[3], splat<3>(p)};
                    h = hn;
                    cycles += 25;
                }
                kick(kPolyPacketQword, kPolyKickPc);
                cycles += 8;
                acc = nextPart;
                vf20 = nextPos;
                vf06 = fogScaleRaw;
            }

            // The whole call for the triangle ending at vertex i: the three corners into buffer A, six passes, emit.
            void run(int32_t i, int32_t base, int32_t out, const Placed *placed, bool reversed)
            {
                wroteVf06 = wroteVf20 = wroteAcc = false;
                const int32_t vertexAt = base + 3 * i, outAt = out + 1 + 3 * i;
                const Vec4 c0 = placed[i].clip, c1 = placed[i - 1].clip, c2 = placed[i - 2].clip;
                const Vec4 uv0 = mem.load(vertexAt + 2), uv1 = mem.load(vertexAt - 1), uv2 = mem.load(vertexAt - 4);
                const Vec4 col0 = itof(mem.load(outAt + 1)), col1 = itof(mem.load(outAt - 2)), col2 = itof(mem.load(outAt - 5));
                // Corners in the strip's winding: (i-1, i-2, i), or (i-2, i-1, i) where it is reversed.
                const Vec4 *pa[3] = {&c1, &uv1, &col1}, *pb[3] = {&c2, &uv2, &col2};
                if (reversed)
                    std::swap(pa, pb);
                mem.put(kPolyA + 6, c0);
                mem.put(kPolyA + 0, *pa[0]);
                mem.put(kPolyA + 3, *pb[0]);
                mem.put(kPolyA + 9, *pa[0]);
                mem.put(kPolyA + 2, *pa[1]);
                mem.put(kPolyA + 5, *pb[1]);
                mem.put(kPolyA + 8, uv0);
                mem.put(kPolyA + 11, *pa[1]);
                mem.put(kPolyA + 1, *pa[2]);
                mem.put(kPolyA + 4, *pb[2]);
                mem.put(kPolyA + 7, col0);
                mem.put(kPolyA + 10, *pa[2]);
                cycles += 4 + (reversed ? 29 : 31);

                struct Plane
                {
                    uint32_t cur, next;
                    int lane;
                    float side;
                };
                static constexpr Plane kPlanes[6] = {{0x800u, 0x20u, 2, -1.0f}, {0x400u, 0x10u, 2, 1.0f}, {0x080u, 0x02u, 0, -1.0f},
                                                     {0x040u, 0x01u, 0, 1.0f},  {0x200u, 0x08u, 1, -1.0f}, {0x100u, 0x04u, 1, 1.0f}};
                int32_t count = 3;
                for (int p = 0; p < 6; ++p)
                {
                    const bool toB = (p & 1) == 0;
                    count = pass(toB ? kPolyA : kPolyB, toB ? kPolyB : kPolyA, count, kPlanes[p].cur, kPlanes[p].next,
                                 kPlanes[p].lane, kPlanes[p].side);
                    if (count == 0)
                        break;
                }
                if (count != 0)
                {
                    cycles += 6;
                    emit(count);
                }
                cycles += 2;
            }
        };

        // Upper bound on a vertex's cycles before the run (the clipper's own at most kCyclesClipperBound).
        inline uint64_t vertexCycles(uint8_t path)
        {
            switch (path)
            {
            case Draw:
                return kCyclesDraw;
            case Flagged:
                return kCyclesFlagged;
            case Backface:
                return kCyclesBackface;
            case Clipped:
                return kCyclesToClipper + kCyclesFromClipper;
            default:
            {
                const uint64_t tests = static_cast<uint64_t>(path - Cull1 + 1);
                return kCyclesClipCull + 3u * tests + (tests >= 3u ? 1u : 0u);
            }
            }
        }

        // ---- the program --------------------------------------------------------------------------------------------

        constexpr int32_t kMaxVertices = (kOutputOffset - 2 - 3) / 3; // the batch and vertex N's read below the packet

        bool run(Vu1NativeRun &r)
        {
            VU1State &st = *r.state;
            const Memory mem{r.data};
            if (r.dataSize != 0x4000u)
                return false;

            // decode: the batch at TOP
            const int32_t top = static_cast<int32_t>(st.top & 0x3FFu);
            const int32_t count = s16(static_cast<int32_t>(mem.word(top, 0))) & st.vi[8];
            const int32_t base = top + 2; // vertex 0
            const int32_t out = top + kOutputOffset;
            // Declined (the AOT runs it): an empty batch (the VU would loop 65536 times), one whose input (vertex N
            // included, read ahead) or packet wraps round data memory, overlaps the other, or reaches the program's
            // own qwords (fixed data, polygon buffers, register stack), a strip packet with no EOP.
            if (count <= 0 || count > kMaxVertices || top < kLowEnd || out + 1 + 3 * count > kStackTop - 16 ||
                (mem.word(top, 0) & 0x8000u) == 0u || ((mem.word(top, 1) >> 26) & 3u) == 3u)
                return false;
            const VuFloatEnv env;
            Constants k;
            for (int i = 0; i < 4; ++i)
            {
                k.toScreen[i] = norm(reg(st, 1 + i));
                k.toClip[i] = norm(reg(st, 9 + i));
            }
            k.colourScale = norm(reg(st, 21));
            k.stqOffset = norm(reg(st, 26));
            k.colourMax = splat(kColourMax);
            k.positionScale = splat(kPositionScale);
            const Vec4 fogBaseRaw = mem.load(kFogBaseQword), fogScaleRaw = mem.load(kFogScaleQword);
            k.fogBase = splat<3>(norm(fogBaseRaw));
            k.fogScale = splat<3>(norm(fogScaleRaw));
            k.stqIn = blend<0x4>(reg(st, 14), splat(1.0f));
            k.zeroW = mul(norm(reg(st, 31)), splat(0.0f));
            k.adcBit = st.vi[6];
            k.countMask = st.vi[8];
            const Vec4 header = mem.load(top + 1);

            // ---- pass 1: the strip, pure ----
            Placed placed[kMaxVertices];
            uint32_t divSticky = 0u;
            Vec4 prev1 = mem.load(kPrev1Qword), prev2 = mem.load(kPrev2Qword); // raw: data qwords 20 / 21
            Vec4 lastPrev1 = prev1, lastPrev2 = prev2, lastA{}, lastB{};
            int32_t out2 = -2, out1 = -2, lastVi11 = 0;
            bool lastNegative = false;
            uint32_t clipped = 0;
            uint64_t bound = (st.pc != 0x00C8u ? kCyclesMscnt : 0u) + kCyclesPrologue;
            for (int32_t i = 0; i < count; ++i)
            {
                Placed &v = placed[i];
                const int32_t at = base + 3 * i;
                const Vec4 pos = mem.load(at), colour = mem.load(at + 1), uv = mem.load(at + 2);
                const Vec4 object = mul(itof(pos), k.positionScale);
                const Vec4 screenH = transform(k.toScreen, object);
                v.clip = transform(k.toClip, object);
                const float q = reciprocal(lane<3>(screenH), divSticky);
                const Vec4 qq = splat(q);
                v.screen = mul(screenH, qq);
                Vec4 fog = add(k.fogBase, mul(k.fogScale, splat<3>(screenH)));
                fog = vmax(vmin(fog, k.colourMax), splat(0.0f));
                v.xyzf = ftoi4(blend<0x8>(v.screen, fog));
                v.rgba = ftoi(vmin(mul(itof(colour), k.colourScale), k.colourMax));
                v.stqBase = add(norm(blend<0x3>(k.stqIn, itof12(uv))), k.stqOffset);
                v.stq = blend<0x7>(v.stqBase, mul(v.stqBase, qq));
                v.code = clipCode(v.clip);
                v.flags = s16(static_cast<int32_t>(mem.word(at, 3)));

                // ADC: flagged in the data, then the clip tests, then the facing (the branch taken sets the cost).
                const Vec4 p1 = norm(prev1), p2 = norm(prev2);
                const Vec4 a = sub(p2, p1);       // vf20 = prev2 - prev1
                const Vec4 b = sub(v.screen, p1); // vf22 = this - prev1
                const bool reversed = (v.flags & k.countMask) != 0;
                const Vec4 cross = reversed ? crossProducts(b, a) : crossProducts(a, b);
                const bool negative = reversed ? crossNegative(lane<2>(cross), b, a) : crossNegative(lane<2>(cross), a, b);
                const int32_t outside = (v.code & 0x3Fu) != 0u ? 1 : 0; // FCAND vi01, 0x3f
                const int32_t vi11 = s16(out2 + out1 + outside);
                if ((v.flags & k.adcBit) != 0)
                    v.path = Flagged;
                else if (vi11 > 0)
                {
                    // 0x0368: dropped when all three corners are outside one plane (FCOR), or one is past +z (FCAND)
                    const uint32_t c0 = v.code, c1 = placed[i - 1].code, c2 = placed[i - 2].code;
                    const uint32_t all = c0 & c1 & c2;
                    v.path = (all & 0x08u) ? Cull1
                             : (all & 0x04u) ? Cull1 + 1
                             : (all & 0x02u) ? Cull1 + 2
                             : (all & 0x01u) ? Cull1 + 3
                             : (all & 0x20u) ? Cull1 + 4
                             : ((c0 | c1 | c2) & 0x10u) ? Cull1 + 5
                                                        : Clipped;
                    if (v.path == Clipped)
                    {
                        ++clipped;
                        bound += kCyclesClipperBound;
                    }
                }
                else
                    v.path = negative ? Backface : Draw;
                bound += vertexCycles(v.path);

                lastPrev1 = prev1, lastPrev2 = prev2, lastA = a, lastB = b, lastNegative = negative, lastVi11 = vi11;
                prev2 = prev1;
                prev1 = v.screen;
                out2 = out1;
                out1 = outside;
            }
            const uint32_t stripQwords = packetQwords(mem, top);
            bound += 2u * stripQwords + 64u;
            if (bound > r.budget)
                return false;
            // The clipper's pieces use the header's tag; it puts vf01-vf04 back from data qwords 0..3 (declined if
            // they differ: the strip's transform would change mid-batch).
            if (clipped != 0u)
            {
                if (((mem.word(top + 1, 1) >> 26) & 3u) == 3u)
                    return false;
                for (int j = 0; j < 4; ++j)
                    if (std::memcmp(st.vf[1 + j], mem.qw(j), 16) != 0)
                        return false;
            }

            // ---- pass 2: the writes in the program's order, the clipper where it runs ----
            uint64_t cycles = (st.pc != 0x00C8u ? kCyclesMscnt : 0u) + kCyclesPrologue;
            uint64_t kickDone = 0u;
            uint32_t clipReg = st.clip & 0xFFFFFFu;
            const auto outside = [&placed](int32_t i) { return i < 0 ? -2 : ((placed[i].code & 0x3Fu) != 0u ? 1 : 0); };
            const auto codeOf = [&placed](int32_t i) { return i < 0 ? 0u : placed[i].code; };
            mem.put(out, mem.load(top));        // the tag goes first in the packet
            mem.put(kPolyPacketQword, header);  // the second header qword is kept (the pieces' tag)
            Clipper clipper(mem, k, r, cycles, kickDone, divSticky, clipReg);
            Vec4 vf20W = reg(st, 20), vf22W = reg(st, 22); // .w: the strip never writes it, the clipper does
            bool anyClipTest = false;
            for (int32_t i = 0; i < count; ++i)
            {
                const Placed &v = placed[i];
                const int32_t at = base + 3 * i, o = out + 1 + 3 * i;
                const Vec4 previous = mem.load(kPrev1Qword);
                mem.put(kPrev1Qword, v.screen);
                mem.put(kPrev2Qword, previous);
                mem.put(at + 2, blend<0x3>(mem.load(at + 2), v.stqBase));
                mem.put(o, v.stq);
                mem.put(o + 1, v.rgba);
                mem.put(o + 2, v.xyzf);
                anyClipTest = anyClipTest || v.path >= Cull1;
                if (v.path == Clipped)
                {
                    cycles += kCyclesToClipper;
                    // 0x0FD0: the registers onto the stack (as they are at the call), the clip register as the
                    // tests left it: this corner, the two before it, this corner.
                    const Vec4 nextPos = mem.load(at + 3);
                    const Vec4 nextObject = blend<0x7>(nextPos, mul(itof(nextPos), k.positionScale));
                    const Vec4 saved[12] = {nextObject, reg(st, 17), reg(st, 21), transform(k.toScreen, nextObject),
                                            placed[i - 2].clip, placed[i - 1].clip, reg(st, 26), fogBaseRaw, fogScaleRaw,
                                            reg(st, 29), header, k.zeroW};
                    for (int j = 0; j < 12; ++j)
                        mem.put(kStackTop - 1 - j, saved[j]);
                    const int32_t savedVi[14] = {v.flags & 0x7FFF, o, at, out, k.adcBit, count - 1 - i, k.countMask,
                                                 s16(static_cast<int32_t>(laneBits<3>(v.xyzf))), st.vi[10], 0x7FFF,
                                                 outside(i - 1), outside(i), kStackTop - 16, 0x81};
                    for (int j = 0; j < 14; ++j)
                        mem.setWord(kStackTop - 13 - j / 4, j % 4, static_cast<uint32_t>(savedVi[j]) & 0xFFFFu);
                    clipReg = (v.code << 18) | (codeOf(i - 2) << 12) | (codeOf(i - 1) << 6) | v.code;
                    clipper.run(i, base, out, placed, (v.flags & 0x7FFF) != 0);
                    mem.setWord(kStackTop - 16, 1, 0x85u); // 0x10D8 stores vi14 / vi15 again (the last BAL's link)
                    cycles += kCyclesFromClipper;
                    if (clipper.wroteVf20)
                        vf20W = clipper.vf20;
                    vf22W = clipper.vf22;
                }
                else
                    cycles += vertexCycles(v.path);
                if (v.path != Draw)
                    mem.setWord(o + 2, 3, static_cast<uint32_t>(s16(static_cast<int32_t>(laneBits<3>(v.xyzf))) + k.adcBit) & 0xFFFFu);
            }

            // ---- the end state ----
            const Placed &lastV = placed[count - 1];
            const bool lastClipped = lastV.path == Clipped;
            const int32_t nextAt = base + 3 * count; // vertex N, past the batch: the pipeline read it
            const Vec4 nextPos = mem.load(nextAt);
            const Vec4 nextObject = blend<0x7>(nextPos, mul(itof(nextPos), k.positionScale));
            const Vec4 nextPart = partial(k.toScreen, nextObject);
            const Vec4 nextScreenH = add(nextPart, k.toScreen[3]);
            const float nextQ = reciprocal(lane<3>(nextScreenH), divSticky);
            const Vec4 vf00 = bitsOf(0u, 0u, 0u, 0x3F800000u);

            setReg(st, 5, nextObject);
            setReg(st, 6, lastClipped && clipper.wroteVf06 ? clipper.vf06 : lastV.stq);
            setReg(st, 7, itof(mem.load(nextAt + 1)));
            setReg(st, 8, lastV.xyzf);
            setReg(st, 13, lastV.clip);
            setReg(st, 14, blend<0x3>(k.stqIn, mem.load(nextAt + 2)));
            setReg(st, 18, lastClipped ? clipper.vf18 : lastPrev1);
            setReg(st, 19, lastClipped ? clipper.vf19 : lastPrev2);
            setReg(st, 20, lastClipped && clipper.wroteVf20 ? clipper.vf20 : blend<0x8>(lastA, vf20W));
            setReg(st, 22, lastClipped ? clipper.vf22 : blend<0x8>(lastB, vf22W));
            setReg(st, 23, nextScreenH);
            setReg(st, 24, count >= 3 ? placed[count - 3].clip : vf00);
            setReg(st, 25, count >= 2 ? placed[count - 2].clip : vf00);
            setReg(st, 27, fogBaseRaw);
            setReg(st, 28, fogScaleRaw);
            setReg(st, 30, header);
            if (anyClipTest)
                setReg(st, 31, k.zeroW);
            store(st.acc, lastClipped && clipper.wroteAcc ? clipper.acc : nextPart);
            st.q = nextQ;
            st.i = kColourMax;
            st.clip = lastClipped ? clipReg
                                  : (lastV.code << 18) | (codeOf(count - 3) << 12) | (codeOf(count - 2) << 6) | lastV.code;

            const int32_t lastXyzfW = s16(static_cast<int32_t>(laneBits<3>(lastV.xyzf)));
            const bool lastAdc = lastV.path != Draw;
            const bool lastTested = lastV.path >= Cull1;
            const int32_t tests = lastTested && !lastClipped ? lastV.path - Cull1 + 1 : 6;
            st.vi[1] = lastClipped ? 0x85 : lastTested ? 1 : outside(count - 1);
            st.vi[2] = !lastTested || tests < 2 ? (lastV.flags & k.adcBit) : tests < 3 ? lastV.flags : (lastV.flags & 0x7FFF);
            st.vi[3] = s16(out + 1 + 3 * count);
            st.vi[4] = s16(nextAt);
            st.vi[5] = s16(out);
            st.vi[7] = 0;
            st.vi[9] = lastAdc ? s16(lastXyzfW + k.adcBit) : lastXyzfW;
            st.vi[11] = lastTested ? 0x7FFF : lastVi11;
            st.vi[12] = outside(count - 2);
            st.vi[13] = outside(count - 1);
            st.vi[14] = lastTested && tests >= 4 ? kStackTop : lastV.flags;
            st.vi[15] = lastClipped ? 0x85 : (lastNegative ? 0x20 : 0);

            // Flags: the run's last FMAC is the next vertex's MADDw (vf23), the clip tests' MULx vf31 when the last
            // triangle went to them, or the clipper's last; the last DIV is 1 / vf23.w; the sticky bits gather every
            // DIV's divide-by-zero / 0/0, and the sticky zero / sign / underflow / overflow are set (kStatusStickyFlags).
            LastFmac lastFmac{LastFmac::Madd, nextPart, k.toScreen[3], splat(1.0f)};
            if (lastClipped)
                lastFmac = clipper.last;
            else if (lastTested)
                lastFmac = {LastFmac::Mul, k.zeroW, splat(0.0f)};
            uint32_t statusLow = 0u;
            st.mac = lastFmac.flags(statusLow);
            st.status = (st.status & 0xFC0u) | kStatusStickyFlags | statusLow |
                        (lane<3>(norm(nextScreenH)) == 0.0f ? 0x20u : 0u) | divSticky;
            st.pc = kEndPc;

            // 0x0340: the strip's packet (after the clipper's last piece has gone)
            if (cycles < kickDone)
                cycles = kickDone;
            r.kick(r.user, static_cast<uint32_t>(out), kKickPc);
            r.cycles = cycles + 2u * stripQwords - 1u;
            return true;
        }
    }

    Vu1NativeProgram clipSceneryProgram() { return &run; }
}
