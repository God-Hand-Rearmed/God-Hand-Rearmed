// The libvu0 natives' maths (src/kamite/native/libvu0_core.h) on its own, without the runtime: replays the vectors
// shadow_test wrote (a matrix, a vector, and the recompiled sceVu0ApplyMatrix's ACC and result under the game thread's
// float rules) and compares bit for bit. Plain C++ and the core header only, so it builds for any target (README.md):
// SSE, plain C++ (-DKAMITE_VU_SCALAR) and AArch64 NEON under qemu.
#include "kamite/native/libvu0_core.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    using namespace kamite::native;
    if (argc < 2)
    {
        std::printf("usage: libvu0_check <vectors.bin>\n");
        return 2;
    }
    std::FILE *f = std::fopen(argv[1], "rb");
    if (!f)
        return 2;
    VuFloatEnv env; // round toward zero, denormals flushed: the game thread's MXCSR
    unsigned char record[112];
    int count = 0, bad = 0;
    while (std::fread(record, 1, sizeof(record), f) == sizeof(record))
    {
        const Vec4 m0 = load(record), m1 = load(record + 16), m2 = load(record + 32), m3 = load(record + 48);
        const Vec4 b = load(record + 64);
        if (!libvu0::finiteInputs(m0) || !libvu0::finiteInputs(m1) || !libvu0::finiteInputs(m2) ||
            !libvu0::finiteInputs(m3) || !libvu0::finiteInputs(b))
            continue;
        const libvu0::RowResult row = libvu0::mulRow(m0, m1, m2, m3, b);
        unsigned char acc[16], r[16];
        store(acc, row.acc);
        store(r, row.r);
        ++count;
        if (std::memcmp(acc, record + 80, 16) != 0 || std::memcmp(r, record + 96, 16) != 0)
        {
            if (++bad <= 5)
                std::printf("vector %d differs\n", count);
        }
    }
    std::fclose(f);

    // RotMatrixX / Y / Z (<vectors>.rot): ecossin, the rotation rows and the four rows of the product.
    int rotCount = 0, rotBad = 0;
    if (std::FILE *r = std::fopen((std::string(argv[1]) + ".rot").c_str(), "rb"))
    {
        unsigned char in[4 + 16 * 7 + 12 + 64], out[64 + 96 + 16 + 8];
        while (std::fread(in, 1, sizeof(in), r) == sizeof(in) && std::fread(out, 1, sizeof(out), r) == sizeof(out))
        {
            uint32_t axis, status, negative;
            float q;
            std::memcpy(&axis, in, 4);
            const unsigned char *p = in + 4;
            const Vec4 vf0 = load(p), coefficients = load(p + 16);
            libvu0::VuRegs v{load(p + 32), load(p + 48), load(p + 64), load(p + 80), load(p + 96), 0.0f, 0};
            std::memcpy(&q, p + 112, 4), std::memcpy(&status, p + 116, 4), std::memcpy(&negative, p + 120, 4);
            v.q = q;
            v.status = static_cast<uint16_t>(status);
            const unsigned char *matrix = p + 124;
            libvu0::ecossin(vf0, coefficients, negative != 0u, v);
            Vec4 vf4 = v.vf4;
            const libvu0::RotationRows m = axis == 0   ? libvu0::rotationRows<0>(vf0, v.vf5, vf4)
                                           : axis == 1 ? libvu0::rotationRows<1>(vf0, v.vf5, vf4)
                                                       : libvu0::rotationRows<2>(vf0, v.vf5, vf4);
            unsigned char got[sizeof(out)] = {};
            libvu0::RowResult row{};
            Vec4 last{};
            for (int k = 0; k < 4; ++k)
            {
                last = load(matrix + 16 * k);
                row = libvu0::mulRow(m.r0, m.r1, m.r2, m.r3, last);
                store(got + 16 * k, row.r);
            }
            store(got + 64, last);    // vf4
            store(got + 80, row.r);   // vf5
            store(got + 96, m.r0);    // vf6-vf9
            store(got + 112, m.r1);
            store(got + 128, m.r2);
            store(got + 144, m.r3);
            store(got + 160, row.acc);
            const uint32_t statusOut = v.status;
            std::memcpy(got + 176, &v.q, 4);
            std::memcpy(got + 180, &statusOut, 4);
            ++rotCount;
            if (std::memcmp(got, out, sizeof(out)) != 0 && ++rotBad <= 5)
            {
                for (size_t i = 0; i < sizeof(out); ++i)
                    if (got[i] != out[i])
                    {
                        std::printf("rot vector %d (axis %u) differs at byte %zu\n", rotCount, axis, i);
                        break;
                    }
            }
        }
        std::fclose(r);
    }
    bad += rotBad;

    // Rung 3's batch 1 (<vectors>.ops, written by shadow_test from the recompiled functions): OuterProduct, Normalize,
    // capVu0Length, InversMatrix, ScaleVector, MulVector, TransMatrix; records the natives would decline are skipped.
    int opsCount = 0, opsBad = 0;
    if (std::FILE *o = std::fopen((std::string(argv[1]) + ".ops").c_str(), "rb"))
    {
        unsigned char rec[256];
        const Vec4 vf0 = bitsOf(0u, 0u, 0u, 0x3F800000u);
        while (std::fread(rec, 1, sizeof(rec), o) == sizeof(rec))
        {
            uint32_t kind;
            uint16_t status;
            float s;
            std::memcpy(&kind, rec, 4), std::memcpy(&s, rec + 132, 4), std::memcpy(&status, rec + 136, 2);
            const Vec4 a = load(rec + 4), b = load(rec + 68), vf5 = load(rec + 84), vf6 = load(rec + 100), acc = load(rec + 116);
            unsigned char got[64], want[64];
            size_t size = 0;
            auto put = [&](const void *mine, const void *theirs, size_t n) {
                std::memcpy(got + size, mine, n), std::memcpy(want + size, theirs, n), size += n;
            };
            unsigned char t[16], u[16];
            if (kind == 1)
            {
                if (!libvu0::finiteXyz(a) || !libvu0::finiteXyz(b) || libvu0::exponent255(laneBits<3>(vf6)))
                    continue;
                const libvu0::OuterResult r = libvu0::outerProduct(a, b);
                store(t, blend<8>(r.r, sub(vf6, vf6))), put(t, rec + 140, 16);
                store(u, blend<7>(acc, r.acc)), put(u, rec + 220, 16);
            }
            else if (kind == 2)
            {
                if (!libvu0::finiteXyz(a) || !libvu0::ordinaryLength(libvu0::squaresSummed(a, a)))
                    continue;
                const libvu0::NormalizeResult n = libvu0::normalize(vf0, a, vf5, status);
                store(t, n.vf6), put(t, rec + 140, 16);
                store(u, n.vf5), put(u, rec + 242, 14);
                put(&n.q, rec + 236, 4), put(&n.status, rec + 240, 2);
            }
            else if (kind == 3)
            {
                if (!libvu0::finiteXyz(a))
                    continue;
                const libvu0::LengthResult l = libvu0::length(vf0, a, status);
                store(t, l.vf4), put(t, rec + 204, 16);
                put(&l.q, rec + 236, 4), put(&l.status, rec + 240, 2);
            }
            else if (kind == 4)
            {
                libvu0::Words r[4];
                bool ok = true;
                for (int k = 0; k < 4; ++k)
                    std::memcpy(r[k].w, rec + 4 + 16 * k, 16), ok = ok && libvu0::finiteXyz(load(r[k].w));
                if (!ok)
                    continue;
                const Vec4 v5 = load(r[3].w), v9 = blend<7>(v5, sub(v5, v5));
                store(r[3].w, v9);
                const libvu0::Words t4 = libvu0::pextlw(r[1], r[0]), t5 = libvu0::pextuw(r[1], r[0]);
                const libvu0::Words t6 = libvu0::pextlw(r[3], r[2]), t7 = libvu0::pextuw(r[3], r[2]);
                const libvu0::Words c0 = libvu0::pcpyld(t6, t4), c1 = libvu0::pcpyud(t4, t6), c2 = libvu0::pcpyld(t7, t5);
                const libvu0::InversResult x = libvu0::inversRows(load(c0.w), load(c1.w), load(c2.w), v5, v9, acc);
                unsigned char m[64];
                std::memcpy(m, c0.w, 16), std::memcpy(m + 16, c1.w, 16), std::memcpy(m + 32, c2.w, 16), store(m + 48, x.vf4);
                put(m, rec + 140, 48);
                store(t, x.vf4), put(t, rec + 188, 16);
            }
            else if (kind == 5)
            {
                if (!libvu0::finiteInputs(a) || (floatBits(s) & 0x7FFFFFFFu) >= 0x5E800000u)
                    continue;
                store(t, mul(a, splat(s))), put(t, rec + 140, 16);
            }
            else if (kind == 6)
            {
                if (!libvu0::finiteInputs(a) || !libvu0::finiteInputs(b))
                    continue;
                store(t, mul(a, b)), put(t, rec + 140, 16);
            }
            else
            {
                const Vec4 row3 = load(rec + 52);
                if (!libvu0::finiteXyz(row3) || !libvu0::finiteXyz(b))
                    continue;
                put(rec + 4, rec + 140, 48);
                store(t, blend<7>(row3, add(row3, b))), put(t, rec + 188, 16);
            }
            ++opsCount;
            if (std::memcmp(got, want, size) != 0 && ++opsBad <= 5)
                std::printf("ops vector %d (kind %u) differs\n", opsCount, kind);
        }
        std::fclose(o);
    }
    bad += opsBad;
#if defined(KAMITE_VU_SSE)
    const char *path = "SSE";
#elif defined(KAMITE_VU_NEON)
    const char *path = "NEON";
#else
    const char *path = "plain C++";
#endif
    std::printf("%s: %d vectors, %d differ; RotMatrix %d vectors, %d differ; batch 1 maths %d vectors, %d differ\n", path,
                count, bad - rotBad - opsBad, rotCount, rotBad, opsCount, opsBad);
    return bad ? 1 : 0;
}
