// Native VU1 program: the scenery strip (spec: claude/native-vu1.md "Scenery strip"). The game's microprogram at
// 0x20C0 of VU1 micro memory (uploaded at 0x2000 with its init program; every image the game builds has it) draws one
// batch of a static mesh as a triangle strip: transform, perspective, fog, colour, texture coordinates, and the ADC bit
// (vertex kicks no triangle) for triangles that face away, cross the clip test or are flagged in the data. It never
// clips; the scenery that needs clipping goes through the other scenery program (0x00C8).
//
// Layers: decode (the registers the init program loaded, the batch at TOP) / transform (pure maths) / emit (the GS
// packet in VU memory, then XGKICK). Exactness: every float operation is the microprogram's own, in its order, in the
// VU's float environment (vu_vec.h), so the results are the VU's bit for bit; the only extra is the back-face sign,
// taken from the exact cross product (as the runtime's VU does). After the batch the VU state is left exactly as the
// microprogram leaves it, including what its software pipeline already computed from the data past the batch: later
// programs read some of those registers.
#include "kamite/render/vu1_programs.h"
#include "kamite/render/vu_vec.h"

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"

#include <cmath>
#include <cstring>
#include <initializer_list>

namespace kamite::render
{
    namespace
    {
        // ---- decode -------------------------------------------------------------------------------------------------

        constexpr uint32_t kFcandPc = 0x2270;      // "fcand vi01, mask": the clip test's planes (near_geometry patches it)
        constexpr uint32_t kOutputOffset = 0xD0;   // the packet is built at TOP + 0xD0
        constexpr int32_t kFogBaseQword = 8;       // data qwords the program reads every run
        constexpr int32_t kFogScaleQword = 9;
        constexpr int32_t kPrev1Qword = 20;        // the last two screen positions, carried from batch to batch
        constexpr int32_t kPrev2Qword = 21;
        constexpr int32_t kHeaderQword = 30;       // where the batch's second header qword is kept

        // Loaded by the init program (0x2000) from data qwords 0..15, kept in registers across runs.
        struct Constants
        {
            Vec4 toScreen[4];   // vf01-vf04: object -> homogeneous screen position
            Vec4 toClip[4];     // vf09-vf12: object -> clip space (the culling test only)
            Vec4 positionScale; // vf23.xyz (0.01: the positions are integers)
            Vec4 colourScale;   // vf21
            Vec4 stqOffset;     // vf26
            Vec4 colourMax;     // I (255), every lane
            Vec4 fogBase;       // vf27 = data qword 8 (w used)
            Vec4 fogScale;      // vf28 = data qword 9 (w used)
            Vec4 stqIn;         // vf06 before the batch: .z becomes 1, .w is left as it was (it reaches STQ.w)
            int32_t adcBit;     // vi06 (0x8000)
            int32_t countMask;  // vi08 (0x7FFF)
            uint32_t clipMask;  // the FCAND immediate: 0x03FFFF (every plane), 0x010410 (far only, near_geometry)
        };

        // A vertex as uploaded: three quadwords.
        struct InVertex
        {
            Vec4 position; // int32 x, y, z (scaled by positionScale); w: flags (bit 15: no triangle, bits 0-14:
                           // nonzero = the strip's winding is reversed at this vertex)
            Vec4 colour;   // int32 r, g, b, a
            Vec4 uv;       // int32 s, t (12-bit fraction)
        };

        struct Memory
        {
            uint8_t *data;
            uint8_t *qw(int32_t address) const { return data + ((static_cast<uint32_t>(address) & 0x3FFu) << 4); }
            Vec4 load(int32_t address) const { return kamite::render::load(qw(address)); }
            InVertex vertex(int32_t address) const { return {load(address), load(address + 1), load(address + 2)}; }
        };

        inline Vec4 reg(const VU1State &st, int r) { return kamite::render::load(st.vf[r]); }
        inline void setReg(VU1State &st, int r, Vec4 v) { kamite::render::store(st.vf[r], v); }
        inline int32_t s16(int32_t v) { return static_cast<int16_t>(v); }

        // ---- transform ----------------------------------------------------------------------------------------------

        // MULAx / MADDAy / MADDAz / MADDw with vf00w: ((m0 x + m1 y) + m2 z) + m3, each step rounded.
        inline Vec4 transform(const Vec4 m[4], Vec4 p)
        {
            Vec4 acc = mul(m[0], splat<0>(p));
            acc = add(acc, mul(m[1], splat<1>(p)));
            acc = add(acc, mul(m[2], splat<2>(p)));
            return add(acc, m[3]);
        }

        // DIV q, 1, w.
        inline float reciprocal(float w)
        {
            if (w == 0.0f)
                return std::signbit(w) ? -bitsFloat(0x7F7FFFFFu) : bitsFloat(0x7F7FFFFFu);
            const float q = 1.0f / w;
            return lane<0>(norm(splat(q)));
        }

        // CLIP c.xyz, c.w: six bits (+x -x +y -y +z -z), compared as sign-magnitude integers.
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

        // OPMULA acc, a, b: (a.y b.z, a.z b.x, a.x b.y).
        inline Vec4 crossProducts(Vec4 a, Vec4 b)
        {
            const float ax = lane<0>(a), ay = lane<1>(a), az = lane<2>(a);
            const float bx = lane<0>(b), by = lane<1>(b), bz = lane<2>(b);
            const Vec4 l = bitsOf(floatBits(ay), floatBits(az), floatBits(ax), 0u);
            const Vec4 r = bitsOf(floatBits(bz), floatBits(bx), floatBits(by), 0u);
            return mul(l, r);
        }

        // The sign flag of OPMSUB's z lane after OPMULA acc, a, b: acc.z - b.x a.y, taken exactly (the runtime's VU
        // takes the flags of a cancelling or saturating multiply-subtract from the exact result).
        inline bool crossNegative(float accZ, Vec4 a, Vec4 b)
        {
            const double product = static_cast<double>(lane<0>(b)) * static_cast<double>(lane<1>(a));
            const double result = static_cast<double>(accZ) - product;
            return std::signbit(result);
        }

        struct OutVertex
        {
            Vec4 stq;    // s q, t q, q (float), w carried
            Vec4 rgba;   // int32
            Vec4 xyzf;   // int32: x, y, z in 1/16, fog in 1/16
            Vec4 screen; // float x, y, z of the perspective-divided position (the back-face test's input)
            Vec4 clip;   // clip-space position
            Vec4 colourF; // the colour before MINI (end state only)
            float q;
            bool divideByZero; // w = 0: DIV sets the status register's D flag
        };

        inline Vec4 objectPosition(const Constants &k, Vec4 position) { return mul(itof(position), k.positionScale); }

        inline Vec4 stqBase(const Constants &k, Vec4 uv)
        {
            return add(norm(blend<0x3>(k.stqIn, itof12(uv))), k.stqOffset); // ADD vf14, vf06, vf26
        }

        inline OutVertex transformVertex(const Constants &k, const InVertex &in)
        {
            OutVertex out;
            const Vec4 object = objectPosition(k, in.position);
            const Vec4 screenH = transform(k.toScreen, object);
            out.clip = transform(k.toClip, object);
            out.divideByZero = lane<3>(screenH) == 0.0f;
            out.q = reciprocal(lane<3>(screenH));
            const Vec4 q = splat(out.q);
            out.screen = mul(screenH, q);
            // Fog: fogBase.w + fogScale.w * w, clamped to [0, 255].
            Vec4 fog = add(k.fogBase, mul(k.fogScale, splat<3>(screenH)));
            fog = vmax(vmin(fog, k.colourMax), splat(0.0f));
            out.xyzf = ftoi4(blend<0x8>(out.screen, fog));
            out.colourF = mul(itof(in.colour), k.colourScale);
            out.rgba = ftoi(vmin(out.colourF, k.colourMax));
            const Vec4 stq = stqBase(k, in.uv);
            out.stq = blend<0x7>(stq, mul(stq, q));
            return out;
        }

        // ---- emit ---------------------------------------------------------------------------------------------------

        inline void emitVertex(const Memory &mem, int32_t at, const OutVertex &v, bool adc)
        {
            store(mem.qw(at), v.stq);
            store(mem.qw(at + 1), v.rgba);
            store(mem.qw(at + 2), v.xyzf);
            if (adc)
            {
                // ISW of vi09 = MTIR(xyzf.w) + 0x8000: the low 16 bits with the ADC bit added, zero-extended.
                const uint32_t w = (laneBits<3>(v.xyzf) + 0x8000u) & 0xFFFFu;
                std::memcpy(mem.qw(at + 2) + 12, &w, 4);
            }
        }

        // ---- the program --------------------------------------------------------------------------------------------

        inline uint64_t cyclesFor(uint32_t entryPc, int32_t count, int32_t flagged, int32_t backface)
        {
            return 50u + (entryPc != 0x20C0u ? 2u : 0u) + 39u * static_cast<uint64_t>(count) - 3u * static_cast<uint64_t>(flagged) +
                   static_cast<uint64_t>(backface);
        }

        bool run(Vu1NativeRun &r)
        {
            VU1State &st = *r.state;
            const Memory mem{r.data};
            if (r.dataSize != 0x4000u)
                return false;

            // decode: the batch at TOP
            const int32_t top = static_cast<int32_t>(st.top & 0x3FFu);
            uint32_t tagLo;
            std::memcpy(&tagLo, mem.qw(top), 4);
            const int32_t count = s16(static_cast<int32_t>(tagLo)) & st.vi[8];
            const int32_t base = top + 2;              // vertex 0
            const int32_t out = top + static_cast<int32_t>(kOutputOffset);
            // Declined (the AOT runs it): an empty batch (the VU would loop 65536 times), a batch or packet that wraps
            // round data memory or overlaps the other or the fixed qwords (the order of reads and writes would matter).
            if (count <= 0 || base + 3 * count + 6 > out || out + 1 + 3 * count > 0x400 || top <= kHeaderQword)
                return false;

            Constants k;
            for (int i = 0; i < 4; ++i)
            {
                k.toScreen[i] = norm(reg(st, 1 + i));
                k.toClip[i] = norm(reg(st, 9 + i));
            }
            k.positionScale = norm(reg(st, 23));
            k.colourScale = norm(reg(st, 21));
            k.stqOffset = norm(reg(st, 26));
            k.colourMax = norm(splat(st.i));
            const Vec4 fogBaseRaw = mem.load(kFogBaseQword), fogScaleRaw = mem.load(kFogScaleQword);
            k.fogBase = splat<3>(norm(fogBaseRaw));
            k.fogScale = splat<3>(norm(fogScaleRaw));
            k.stqIn = blend<0x4>(reg(st, 6), bitsOf(0u, 0u, 0x3F800000u, 0u)); // MR32.z vf06, vf00
            k.adcBit = st.vi[6];
            k.countMask = st.vi[8];
            std::memcpy(&k.clipMask, r.code + kFcandPc, 4);
            if ((k.clipMask & 0xFF000000u) != 0x24000000u) // FCAND vi01 (the code hash leaves this word out)
                return false;
            k.clipMask &= 0xFFFFFFu;

            // Declined: constants large enough that a product could saturate (the VU's multiply-add then differs
            // from rounding each step; never in the game's data).
            {
                float big = 0.0f;
                for (int i = 0; i < 4; ++i)
                    for (const Vec4 &m : {k.toScreen[i], k.toClip[i]})
                        big = std::fmax(big, std::fmax(std::fmax(std::fabs(lane<0>(m)), std::fabs(lane<1>(m))),
                                                       std::fmax(std::fabs(lane<2>(m)), std::fabs(lane<3>(m)))));
                const float scale = std::fmax(std::fmax(std::fabs(lane<0>(k.positionScale)), std::fabs(lane<1>(k.positionScale))),
                                              std::fabs(lane<2>(k.positionScale)));
                const double bound = static_cast<double>(big) * scale * 2147483648.0 * 4.0 + big;
                if (!(bound < 1e37) || !(std::fabs(lane<3>(k.fogScale)) * bound < 1e37))
                    return false;
            }

            // The VU cycles (the runtime's count, XGKICK transfer included; measured on every traced run, exact):
            // 50 + 39 per vertex, 3 fewer for a vertex flagged in the data, 1 more for one culled as facing away,
            // 2 more when entered by MSCNT (the "b 0x20C0" at 0x2338 / 0x2378).
            const uint32_t entryPc = st.pc;
            if (cyclesFor(entryPc, count, 0, count) > r.budget)
                return false;
            const VuFloatEnv env;

            // The carried state: the clip flags of the last vertices, the last two screen positions.
            uint32_t clipFlags = st.clip & 0xFFFFFFu;
            Vec4 prev1 = mem.load(kPrev1Qword);                 // vf18 <- data[20].xyz
            Vec4 prev2 = mem.load(kPrev2Qword);                 // vf19 <- data[21].xyz
            const Vec4 prev1Reg = reg(st, 18), prev2Reg = reg(st, 19);
            const uint32_t prev1W = laneBits<3>(prev1Reg);      // vf18.w goes to data[21].w
            Vec4 lastA{}, lastB{}, lastAcc{}, lastXyzf{};
            bool lastAdc = false;
            uint32_t lastClipHit = 0;
            int32_t lastFlags = 0;
            bool lastSign = false;
            Vec4 loadedPrev1 = prev1, loadedPrev2 = prev2;
            int32_t flaggedCount = 0, backfaceCount = 0;
            bool divideByZero = false;

            // header: the GIF tag goes first in the packet, the second qword is kept
            store(mem.qw(out), mem.load(top));
            const Vec4 header = mem.load(top + 1);

            for (int32_t i = 0; i < count; ++i)
            {
                const InVertex in = mem.vertex(base + 3 * i);
                const OutVertex v = transformVertex(k, in);

                // ADC: flagged in the data, the clip test of this triangle's three vertices, or facing away.
                clipFlags = ((clipFlags << 6) | clipCode(v.clip)) & 0xFFFFFFu;
                const uint32_t clipHit = (clipFlags & k.clipMask) != 0u ? 1u : 0u;
                uint32_t flagWord;
                std::memcpy(&flagWord, mem.qw(base + 3 * i) + 12, 4);
                const int32_t flags = s16(static_cast<int32_t>(flagWord));
                const Vec4 p1 = norm(prev1), p2 = norm(prev2);
                const Vec4 a = sub(p2, p1);       // vf20 = prev2 - prev1
                const Vec4 b = sub(v.screen, p1); // vf22 = this - prev1
                const bool reversed = (flags & k.countMask) != 0;
                const Vec4 acc = reversed ? crossProducts(b, a) : crossProducts(a, b);
                const bool negative = reversed ? crossNegative(lane<2>(acc), b, a) : crossNegative(lane<2>(acc), a, b);
                const bool flagged = (flags & k.adcBit) != 0;
                const bool adc = flagged || clipHit != 0u || negative;
                // The microprogram tests the flag, then the clip test, then the facing: the branch taken sets the cost.
                if (flagged)
                    ++flaggedCount;
                else if (clipHit == 0u && negative)
                    ++backfaceCount;

                emitVertex(mem, out + 1 + 3 * i, v, adc);

                loadedPrev1 = prev1;
                loadedPrev2 = prev2;
                prev2 = prev1;           // data[21] <- vf18 (w: vf18.w)
                prev1 = v.screen;        // data[20].xyz <- the screen position
                lastA = a;
                lastB = b;
                lastAcc = acc;
                lastXyzf = v.xyzf;
                lastAdc = adc;
                lastClipHit = clipHit;
                lastFlags = flags;
                lastSign = negative;
                divideByZero = divideByZero || v.divideByZero;
            }

            // The microprogram's software pipeline ran two vertices ahead: what it computed from the data past the
            // batch stays in registers.
            const InVertex next = mem.vertex(base + 3 * count);
            const Vec4 afterNext = mem.load(base + 3 * count + 3);
            const Vec4 nextObject = objectPosition(k, next.position);
            const Vec4 nextScreenH = transform(k.toScreen, nextObject);
            const float nextQ = reciprocal(lane<3>(nextScreenH));
            const Vec4 nextUv = blend<0x3>(k.stqIn, itof12(next.uv));

            // data memory: the carried positions and the header
            {
                Vec4 d20 = mem.load(kPrev1Qword);
                store(mem.qw(kPrev1Qword), blend<0x7>(d20, prev1));
                store(mem.qw(kPrev2Qword), blend<0x8>(prev2, bitsOf(0u, 0u, 0u, prev1W)));
                store(mem.qw(kHeaderQword), header);
            }

            // registers
            setReg(st, 5, afterNext);
            setReg(st, 6, nextUv);
            setReg(st, 7, mul(itof(next.colour), k.colourScale));
            setReg(st, 8, mul(nextScreenH, splat(nextQ)));
            setReg(st, 13, transform(k.toClip, nextObject));
            setReg(st, 14, add(norm(nextUv), k.stqOffset));
            setReg(st, 18, blend<0x7>(prev1Reg, loadedPrev1));
            setReg(st, 19, blend<0x7>(prev2Reg, loadedPrev2));
            setReg(st, 20, blend<0x7>(reg(st, 20), lastA));
            setReg(st, 22, blend<0x7>(reg(st, 22), lastB));
            setReg(st, 24, nextScreenH);
            setReg(st, 25, blend<0x7>(reg(st, 25), nextObject));
            setReg(st, 27, fogBaseRaw);
            setReg(st, 28, fogScaleRaw);
            setReg(st, 30, header);
            store(st.acc, blend<0x8>(lastAcc, norm(fogBaseRaw)));
            st.q = nextQ;
            st.vi[1] = static_cast<int32_t>(lastClipHit);
            st.vi[2] = lastFlags & k.countMask;
            st.vi[3] = s16(out + 1 + 3 * count);
            st.vi[4] = s16(base + 3 * count);
            st.vi[5] = s16(out);
            st.vi[7] = 0;
            st.vi[9] = s16(s16(static_cast<int32_t>(laneBits<3>(lastXyzf))) + k.adcBit);
            st.vi[11] = lastFlags & k.adcBit;
            st.vi[13] = static_cast<int32_t>(lastClipHit);
            st.vi[14] = lastFlags;
            st.vi[15] = (lastFlags & k.adcBit) != 0 || lastSign ? 0x20 : 0;
            st.clip = clipFlags;
            // Flags: the last multiply is MULA.w acc, vf27, vf00 (zero / sign of the fog base); the last DIV the one
            // for the vertex past the batch; the sticky bits gather every DIV's divide-by-zero (the others are set).
            {
                const float fogW = lane<3>(norm(fogBaseRaw));
                const bool zero = fogW == 0.0f, sign = std::signbit(fogW);
                const bool nextDivZero = lane<3>(nextScreenH) == 0.0f;
                st.mac = (zero ? 0x1u : 0u) | (sign ? 0x10u : 0u);
                st.status = (st.status & 0xFC0u) | kStatusStickyFlags | (zero ? 0x1u : 0u) | (sign ? 0x2u : 0u) |
                            (nextDivZero ? 0x20u : 0u) | (divideByZero || nextDivZero ? 0x800u : 0u);
            }
            st.pc = lastAdc ? 0x2378u : 0x2338u;

            r.kick(r.user, static_cast<uint32_t>(out), lastAdc ? 0x2360u : 0x2320u);
            r.cycles = cyclesFor(entryPc, count, flaggedCount, backfaceCount);
            return true;
        }
    }

    Vu1NativeProgram sceneryStripProgram() { return &run; }
}
