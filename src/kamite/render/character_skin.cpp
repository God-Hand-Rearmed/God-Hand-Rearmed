// Native VU1 program: the character program (spec: claude/native-vu1.md "Character"). The game's microprogram at
// 0x00B8 of the character images (c7e9 / d905; uploaded at 0x0000 with its init program) draws one batch of a skinned
// model as a triangle strip: each vertex blends three bone matrices by its weights, then perspective, lighting
// (one directional matrix, clamped, then a colour matrix plus ambient), texture coordinates, and the ADC bit (vertex
// kicks no triangle) for triangles flagged in the data, crossing the clip test or facing away. It never clips.
//
// Layers: decode (the registers the init program loaded, the constants at data qwords 200+, the batch at TOP) /
// transform (pure maths) / emit (the GS packet in VU memory, then XGKICK). Exactness: every float operation is the
// microprogram's own, in its order, in the VU's float environment (vu_vec.h), so the results are the VU's bit for bit;
// the back-face sign and the last multiply-add's flags are taken from the exact result (as the runtime's VU does).
// After the batch the VU state is left exactly as the microprogram leaves it, including what its software pipeline
// already computed from the data past the batch: later programs read some of those registers.
#include "kamite/render/vu1_programs.h"
#include "kamite/native/vu_vec.h"

#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"

#include <cfloat>
#include <cmath>
#include <cstring>

namespace kamite::render
{
    namespace
    {
        // ---- decode -------------------------------------------------------------------------------------------------

        constexpr uint32_t kCallPc = 0x00B8;     // MSCAL entry
        constexpr uint32_t kEndPc = 0x0520;      // where the program stops; MSCNT enters here ("b 0x00B8")
        constexpr uint32_t kFcandPc = 0x0378;    // "fcand vi01, mask": the clip test's planes (near_geometry patches it)
        constexpr uint32_t kKickPc = 0x0508;
        constexpr int32_t kOutputOffset = 0x100; // the packet is built at TOP + 0x100
        constexpr int32_t kVertexQwords = 6;
        constexpr int32_t kLightQword = 200;     // 200-202: light directions (rows); 201 also gives the fog (.w)
        constexpr int32_t kColourScaleQword = 203;
        constexpr int32_t kLightColourQword = 204; // 204-206
        constexpr int32_t kStqOffsetQword = 211;
        constexpr int32_t kPrev1Qword = 215;     // the last two screen positions, carried from batch to batch
        constexpr int32_t kPrev2Qword = 216;
        constexpr uint32_t kWeightScaleBits = 0x3C23D70Au; // LOI 0.01 (weights are percentages)
        constexpr uint32_t kNormalScaleBits = 0x3C010204u; // LOI 1/127
        constexpr uint32_t kColourMaxBits = 0x437F0000u;   // LOI 255 (vf14.w)

        struct Constants
        {
            Vec4 toScreen[4];    // vf01-vf04 (init 0x0000, data qwords 192-195)
            Vec4 toClip[4];      // vf09-vf12 (196-199; the culling test only)
            Vec4 ambient;        // vf25 (207): .xyz added to the light colour, .w scales alpha
            Vec4 lightDir[3];    // data 200-202
            Vec4 lightColour[3]; // data 204-206
            Vec4 colourScale;    // data 203
            Vec4 stqOffset;      // data 211
            Vec4 fogRaw;         // data 201 as loaded (vf22)
            float fog;           // XYZF.w: data 201.w + 0
            Vec4 weightScale, normalScale, colourMax;
            int32_t adcBit;      // vi03 (0x8000, sign-extended)
            int32_t countMask;   // vi08 (0x7FFF)
            uint32_t clipMask;   // the FCAND immediate: 0x03FFFF (every plane), 0x010410 (far only, near_geometry)
        };

        // A vertex as uploaded: six quadwords.
        struct InVertex
        {
            Vec4 position; // float x, y, z; w: flags (low 16 bits; bit 15: no triangle, bits 0-14: nonzero = the
                           // strip's winding is reversed at this vertex)
            Vec4 colour;   // int32 r, g, b, a
            Vec4 uv;       // int32 s, t (12-bit fraction)
            Vec4 bones;    // .y .z .w: data qword addresses of three 4-qword bone matrices (low 16 bits)
            Vec4 weights;  // int32 x, y, z: the bones' weights in percent
            Vec4 normal;   // int32 x, y, z (x 1/127)
        };

        struct Memory
        {
            uint8_t *data;
            uint8_t *qw(int32_t address) const { return data + ((static_cast<uint32_t>(address) & 0x3FFu) << 4); }
            Vec4 load(int32_t address) const { return kamite::render::load(qw(address)); }
            InVertex vertex(int32_t address) const
            {
                return {load(address), load(address + 1), load(address + 2), load(address + 3), load(address + 4), load(address + 5)};
            }
            int32_t word16(int32_t address, int lane) const // ILW: the low 16 bits, sign-extended
            {
                uint32_t w;
                std::memcpy(&w, qw(address) + 4 * lane, 4);
                return static_cast<int16_t>(w);
            }
        };

        inline Vec4 reg(const VU1State &st, int r) { return kamite::render::load(st.vf[r]); }
        inline void setReg(VU1State &st, int r, Vec4 v) { kamite::render::store(st.vf[r], v); }
        inline int32_t s16(int32_t v) { return static_cast<int16_t>(v); }

        inline Vec4 vf00() { return bitsOf(0u, 0u, 0u, 0x3F800000u); }

        // ---- transform ----------------------------------------------------------------------------------------------

        // MULAx / MADDAy / MADDAz: (m0 x + m1 y) + m2 z, each step rounded.
        inline Vec4 transform3(const Vec4 m[3], Vec4 p)
        {
            Vec4 acc = mul(m[0], splat<0>(p));
            acc = add(acc, mul(m[1], splat<1>(p)));
            return add(acc, mul(m[2], splat<2>(p)));
        }
        // ... then MADDw with vf00w: + m3.
        inline Vec4 transform(const Vec4 m[4], Vec4 p) { return add(transform3(m, p), m[3]); }

        // The three bone matrices a vertex names, blended by its weights: row r = (A_r wx + B_r wy) + C_r wz.
        struct Skin
        {
            Vec4 row[4];
        };

        struct BoneRefs
        {
            int32_t at[3]; // vi11, vi12, vi13
        };

        inline BoneRefs boneRefs(const Memory &mem, int32_t vertexAddress)
        {
            return {{mem.word16(vertexAddress + 3, 1), mem.word16(vertexAddress + 3, 2), mem.word16(vertexAddress + 3, 3)}};
        }

        // The bone matrices' rows as FMAC operands (normalized), kept while consecutive vertices name the same bones
        // (the program writes no bone matrix during a run: run() declines such batches).
        struct BoneCache
        {
            int32_t at[3] = {-1, -1, -1};
            Vec4 rows[3][4];

            const Vec4 *bone(const Memory &mem, int slot, int32_t address)
            {
                if (at[slot] != address)
                {
                    at[slot] = address;
                    for (int r = 0; r < 4; ++r)
                        rows[slot][r] = norm(mem.load(address + r));
                }
                return rows[slot];
            }
        };

        inline Skin blendBones(const Memory &mem, BoneCache &cache, const BoneRefs &b, Vec4 weights)
        {
            const Vec4 wx = splat<0>(weights), wy = splat<1>(weights), wz = splat<2>(weights);
            const Vec4 *a = cache.bone(mem, 0, b.at[0]);
            const Vec4 *bb = cache.bone(mem, 1, b.at[1]);
            const Vec4 *c = cache.bone(mem, 2, b.at[2]);
            Skin s;
            for (int r = 0; r < 4; ++r)
            {
                Vec4 acc = mul(a[r], wx);
                acc = add(acc, mul(bb[r], wy));
                s.row[r] = add(acc, mul(c[r], wz));
            }
            return s;
        }

        inline Vec4 weightsOf(const Constants &k, Vec4 raw) { return mul(itof(raw), k.weightScale); }
        inline Vec4 normalOf(const Constants &k, Vec4 raw) { return mul(itof(raw), k.normalScale); }

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

        // MAC and status (low four bits) of MADD a + s t, every lane, as the runtime's VU sets them: zero / sign /
        // underflow / overflow of the exact result (the same as the rounded one's except when it cancels or saturates).
        inline uint32_t maddFlags(Vec4 a, Vec4 s, Vec4 t, uint32_t &statusLow)
        {
            const float av[4] = {lane<0>(a), lane<1>(a), lane<2>(a), lane<3>(a)};
            const float sv[4] = {lane<0>(s), lane<1>(s), lane<2>(s), lane<3>(s)};
            const float tv[4] = {lane<0>(t), lane<1>(t), lane<2>(t), lane<3>(t)};
            uint32_t mac = 0u;
            for (int l = 0; l < 4; ++l)
            {
                const double e = static_cast<double>(av[l]) + static_cast<double>(sv[l]) * static_cast<double>(tv[l]);
                const double m = std::fabs(e);
                const uint32_t bit = 8u >> l; // x is the high bit of each nibble
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

        // ERLENG: 1 / sqrt(x x + y y + z z).
        inline float reciprocalLength(Vec4 v)
        {
            const Vec4 sq = mul(v, v);
            volatile float sum = lane<0>(sq) + lane<1>(sq);
            sum = sum + lane<2>(sq);
            volatile float length = std::sqrt(static_cast<float>(sum));
            const float r = length != 0.0f ? 1.0f / length : static_cast<float>(length);
            return lane<0>(norm(splat(r)));
        }

        // A vertex through the skinning and the screen / clip transforms.
        struct Placed
        {
            Skin skin;
            Vec4 position; // vf05: the blended position
            Vec4 screenH;  // vf26: homogeneous screen position
            Vec4 clip;     // vf13: clip-space position
            Vec4 normal;   // vf23: the blended normal (+ 0, 0, 0, 1)
            float q;       // 1 / screenH.w
        };

        inline Placed place(const Constants &k, const Memory &mem, BoneCache &cache, const InVertex &in, const BoneRefs &bones)
        {
            Placed p;
            p.skin = blendBones(mem, cache, bones, weightsOf(k, in.weights));
            p.position = transform(p.skin.row, norm(in.position));
            p.screenH = transform(k.toScreen, p.position);
            p.clip = transform(k.toClip, p.position);
            p.normal = add(transform3(p.skin.row, normalOf(k, in.normal)), vf00());
            p.q = reciprocal(lane<3>(p.screenH));
            return p;
        }

        struct OutVertex
        {
            Vec4 stq;    // s q, t q, q (float), w: the previous vertex's times q
            Vec4 rgba;   // int32
            Vec4 xyzf;   // int32: x, y, z in 1/16, fog in 1/16
            Vec4 screen; // float x, y, z of the perspective-divided position (the back-face test's input)
            Vec4 light;  // vf31 after the lighting: colour .xyz, clamped direction term .w
            Vec4 lightAcc; // the colour matrix's sum before ambient (.w: the end state's acc, unused)
        };

        inline OutVertex shade(const Constants &k, const InVertex &in, const Placed &p, float stqW)
        {
            OutVertex out;
            const Vec4 q = splat(p.q);
            out.screen = mul(p.screenH, q);
            out.xyzf = ftoi4(blend<0x8>(out.screen, splat(k.fog)));
            // Light: the clamped directional term, through the colour matrix, plus ambient.
            const Vec4 n = blend<0x7>(vf00(), p.normal); // vf31.xyz (the w lane is not read)
            const Vec4 dir = vmax(add(transform3(k.lightDir, n), vf00()), splat(0.0f));
            out.lightAcc = transform3(k.lightColour, dir);
            const Vec4 colourLight = add(out.lightAcc, k.ambient);
            out.light = blend<0x7>(dir, colourLight);
            // Colour: scaled, alpha by ambient.w, rgb by the light, clamped to 255.
            Vec4 colour = mul(itof(in.colour), k.colourScale);
            colour = blend<0x8>(colour, mul(colour, k.ambient));
            colour = blend<0x7>(colour, mul(colour, colourLight));
            out.rgba = ftoi(vmin(colour, k.colourMax));
            // STQ: (s, t, 1) + offset, all four lanes times q (w carries the previous vertex's w).
            const Vec4 st = blend<0x3>(bitsOf(0u, 0u, 0x3F800000u, 0u), itof12(in.uv));
            out.stq = mul(blend<0x8>(add(k.stqOffset, st), splat(stqW)), q);
            return out;
        }

        // ---- emit ---------------------------------------------------------------------------------------------------

        inline void emitVertex(const Memory &mem, int32_t at, const OutVertex &v, bool adc, int32_t adcBit)
        {
            store(mem.qw(at), v.stq);
            store(mem.qw(at + 1), v.rgba);
            store(mem.qw(at + 2), v.xyzf);
            if (adc)
            {
                // ISW of vi09 = MTIR(xyzf.w) + vi03: the low 16 bits with the ADC bit added, zero-extended.
                const uint32_t w = static_cast<uint32_t>(s16(s16(static_cast<int32_t>(laneBits<3>(v.xyzf))) + adcBit)) & 0xFFFFu;
                std::memcpy(mem.qw(at + 2) + 12, &w, 4);
            }
        }

        // ---- the program --------------------------------------------------------------------------------------------

        // Qword ranges [begin, end) in data memory (no wrapping: such runs are declined).
        inline bool overlaps(int32_t aBegin, int32_t aEnd, int32_t bBegin, int32_t bEnd) { return aBegin < bEnd && bBegin < aEnd; }

        // The VU cycles (the runtime's count, XGKICK transfer included; measured on every traced run, exact): 84 + 72
        // per vertex, 1 more when the last vertex has ADC (its ISW), 2 more when entered by MSCNT ("b 0x00B8").
        inline uint64_t cyclesFor(uint32_t entryPc, int32_t count, bool lastAdc)
        {
            return 84u + (entryPc != kCallPc ? 2u : 0u) + 72u * static_cast<uint64_t>(count) + (lastAdc ? 1u : 0u);
        }

        bool run(Vu1NativeRun &r)
        {
            VU1State &st = *r.state;
            const Memory mem{r.data};
            if (r.dataSize != 0x4000u)
                return false;

            // decode: the batch at TOP
            const int32_t top = static_cast<int32_t>(st.top & 0x3FFu);
            const int32_t count = mem.word16(top, 0) & st.vi[8];
            const int32_t base = top + 2;              // vertex 0
            const int32_t out = top + kOutputOffset;
            const int32_t outEnd = out + 1 + 3 * count;
            const int32_t batchEnd = base + kVertexQwords * (count + 1) + 5; // the pipeline reads 2 vertices ahead
            // Declined (the AOT runs it): an empty batch (the VU would loop 65536 times); a batch or packet that wraps
            // round data memory or overlaps the other or the qwords the program reads and writes (the order of reads
            // and writes would matter).
            if (count <= 0 || outEnd > 0x400 || batchEnd > 0x400 || overlaps(top, batchEnd, out, outEnd) ||
                overlaps(top, batchEnd, kPrev1Qword, kPrev2Qword + 1) || overlaps(out, outEnd, kLightQword, kPrev2Qword + 1))
                return false;

            Constants k;
            for (int i = 0; i < 4; ++i)
            {
                k.toScreen[i] = norm(reg(st, 1 + i));
                k.toClip[i] = norm(reg(st, 9 + i));
            }
            k.ambient = norm(reg(st, 25));
            for (int i = 0; i < 3; ++i)
            {
                k.lightDir[i] = norm(mem.load(kLightQword + i));
                k.lightColour[i] = norm(mem.load(kLightColourQword + i));
            }
            k.colourScale = norm(mem.load(kColourScaleQword));
            k.stqOffset = norm(mem.load(kStqOffsetQword));
            k.fogRaw = mem.load(kLightQword + 1);
            k.fog = lane<3>(add(norm(k.fogRaw), splat(0.0f)));
            k.weightScale = splat(bitsFloat(kWeightScaleBits));
            k.normalScale = splat(bitsFloat(kNormalScaleBits));
            k.colourMax = splat(bitsFloat(kColourMaxBits));
            k.adcBit = st.vi[3];
            k.countMask = st.vi[8];
            std::memcpy(&k.clipMask, r.code + kFcandPc, 4);
            if ((k.clipMask & 0xFF000000u) != 0x24000000u) // FCAND vi01 (the code hash leaves this word out)
                return false;
            k.clipMask &= 0xFFFFFFu;

            // Declined: a bone matrix the program would read after this run wrote it (the packet, the carried
            // positions). First the span of every bone address; the exact test only when that span reaches them.
            const auto boneClash = [&](const BoneRefs &b, int rows) {
                for (int j = 0; j < 3; ++j)
                    for (int row = 0; row < rows; ++row)
                    {
                        const int32_t a = (b.at[j] + row) & 0x3FF;
                        if ((a >= out && a < outEnd) || a == kPrev1Qword || a == kPrev2Qword)
                            return true;
                    }
                return false;
            };
            const BoneRefs afterNextBones = boneRefs(mem, base + kVertexQwords * (count + 1));
            {
                int32_t low = 0x3FF, high = 0;
                for (int32_t i = 0; i <= count + 1; ++i)
                {
                    const BoneRefs b = boneRefs(mem, base + kVertexQwords * i);
                    for (int j = 0; j < 3; ++j)
                    {
                        const int32_t a = b.at[j] & 0x3FF;
                        low = a < low ? a : low;
                        high = a > high ? a : high;
                    }
                }
                if (high + 4 > 0x400 || overlaps(low, high + 4, out, outEnd) || overlaps(low, high + 4, kPrev1Qword, kPrev2Qword + 1))
                {
                    for (int32_t i = 0; i <= count; ++i)
                        if (boneClash(boneRefs(mem, base + kVertexQwords * i), 4))
                            return false;
                    if (boneClash(afterNextBones, 1))
                        return false;
                }
            }

            const uint32_t entryPc = st.pc;
            if (cyclesFor(entryPc, count, true) > r.budget)
                return false;
            const VuFloatEnv env;

            uint32_t clipFlags = st.clip & 0xFFFFFFu;
            Vec4 prev1 = mem.load(kPrev1Qword); // data[215].xyz: the last screen position
            Vec4 prev2 = mem.load(kPrev2Qword); // data[216].xyz: the one before
            const Vec4 vf20In = reg(st, 20), vf21In = reg(st, 21);
            float stqW = lane<3>(norm(reg(st, 6))); // vf06.w: never loaded, multiplied by every vertex's q
            Vec4 lastA{}, lastB{}, loadedPrev1 = prev1;
            OutVertex last{};
            uint32_t lastClipHit = 0;
            int32_t lastFlags = 0;
            bool lastNegative = false, lastAdc = false, divideByZero = false;

            // header: the GIF tag goes first in the packet
            store(mem.qw(out), mem.load(top));

            BoneCache bones;
            InVertex in = mem.vertex(base);
            Placed placed = place(k, mem, bones, in, boneRefs(mem, base));
            const float firstReciprocalLength = reciprocalLength(placed.normal); // ERLENG p, vf31 (vertex 0)
            for (int32_t i = 0; i < count; ++i)
            {
                const int32_t at = base + kVertexQwords * i;
                const InVertex nextIn = mem.vertex(at + kVertexQwords);
                const Placed next = place(k, mem, bones, nextIn, boneRefs(mem, at + kVertexQwords));

                divideByZero = divideByZero || lane<3>(placed.screenH) == 0.0f;
                const OutVertex v = shade(k, in, placed, stqW);
                stqW = lane<3>(v.stq);

                // ADC: the clip test of this triangle's three vertices, flagged in the data, or facing away.
                clipFlags = ((clipFlags << 6) | clipCode(placed.clip)) & 0xFFFFFFu;
                const uint32_t clipHit = (clipFlags & k.clipMask) != 0u ? 1u : 0u;
                const int32_t flags = mem.word16(at, 3);
                const Vec4 p1 = norm(prev1), p2 = norm(prev2);
                const Vec4 a = sub(p2, p1);       // vf20 = prev2 - prev1
                const Vec4 b = sub(v.screen, p1); // vf21 = this - prev1
                const bool reversed = (flags & k.countMask) != 0;
                const Vec4 acc = reversed ? crossProducts(b, a) : crossProducts(a, b);
                const bool negative = reversed ? crossNegative(lane<2>(acc), b, a) : crossNegative(lane<2>(acc), a, b);
                const bool adc = clipHit != 0u || (flags & k.adcBit) != 0 || negative;

                emitVertex(mem, out + 1 + 3 * i, v, adc, k.adcBit);

                loadedPrev1 = prev1;
                prev2 = prev1;     // data[216].xyz <- data[215].xyz
                prev1 = v.screen;  // data[215].xyz <- the screen position
                lastA = a;
                lastB = b;
                last = v;
                lastAdc = adc;
                lastClipHit = clipHit;
                lastFlags = flags;
                lastNegative = negative;

                in = nextIn;
                placed = next;
            }

            // The microprogram's software pipeline ran ahead: what it computed from the data past the batch stays in
            // registers (placed: vertex N, past the batch; the weights and bone references of vertex N + 1).
            const int32_t afterNext = base + kVertexQwords * (count + 1);
            const Vec4 afterNextWeights = mem.load(afterNext + 4);
            divideByZero = divideByZero || lane<3>(placed.screenH) == 0.0f;

            // data memory: the carried positions
            store(mem.qw(kPrev1Qword), blend<0x7>(mem.load(kPrev1Qword), prev1));
            store(mem.qw(kPrev2Qword), blend<0x7>(mem.load(kPrev2Qword), prev2));

            // registers
            setReg(st, 5, placed.position);
            setReg(st, 6, last.stq);
            setReg(st, 7, last.rgba);
            setReg(st, 8, last.xyzf);
            setReg(st, 13, placed.clip);
            setReg(st, 14, blend<0x8>(weightsOf(k, afterNextWeights), k.colourMax));
            for (int row = 0; row < 4; ++row)
                setReg(st, 15 + row, placed.skin.row[row]);
            setReg(st, 19, blend<0x7>(mem.load(kColourScaleQword), loadedPrev1));
            setReg(st, 20, blend<0x7>(vf20In, lastA));
            setReg(st, 21, blend<0x7>(vf21In, lastB));
            setReg(st, 22, k.fogRaw);
            setReg(st, 23, placed.normal);
            setReg(st, 26, placed.screenH);
            for (int j = 0; j < 3; ++j)
                setReg(st, 27 + j, mem.load(afterNextBones.at[j]));
            {
                const BoneRefs nb = boneRefs(mem, base + kVertexQwords * count);
                setReg(st, 30, mem.load(nb.at[2] + 3));
            }
            setReg(st, 31, add(placed.normal, vf00()));
            // acc: the last MADDAz of vertex N's clip transform.
            const Vec4 clipAcc = transform3(k.toClip, placed.position);
            store(st.acc, clipAcc);
            st.q = placed.q;
            st.p = firstReciprocalLength;
            st.i = bitsFloat(kWeightScaleBits);
            st.vi[1] = static_cast<int32_t>(lastClipHit) | (lastFlags & k.adcBit) | (lastNegative ? 0x20 : 0);
            st.vi[2] = lastFlags & k.countMask;
            st.vi[4] = s16(base + kVertexQwords * count);
            st.vi[5] = s16(out);
            st.vi[6] = s16(outEnd);
            st.vi[7] = 0;
            st.vi[9] = s16(s16(static_cast<int32_t>(laneBits<3>(last.xyzf))) + k.adcBit);
            for (int j = 0; j < 3; ++j)
                st.vi[11 + j] = afterNextBones.at[j];
            st.vi[14] = lastNegative ? 0x20 : 0;
            st.vi[15] = lastFlags;
            st.clip = clipFlags;
            // Flags: the last multiply-add is MADDw vf13, vf12, vf00w (vertex N's clip position); the last DIV the
            // one for vertex N; the sticky bits gather every DIV's divide-by-zero (the others are set).
            {
                uint32_t statusLow = 0u;
                st.mac = maddFlags(clipAcc, k.toClip[3], splat(1.0f), statusLow);
                const bool nextDivZero = lane<3>(placed.screenH) == 0.0f;
                st.status = (st.status & 0xFC0u) | kStatusStickyFlags | statusLow | (nextDivZero ? 0x20u : 0u) |
                            (divideByZero ? 0x800u : 0u);
            }
            st.pc = kEndPc;

            r.kick(r.user, static_cast<uint32_t>(out), kKickPc);
            r.cycles = cyclesFor(entryPc, count, lastAdc);
            return true;
        }
    }

    Vu1NativeProgram characterSkinProgram() { return &run; }
}
