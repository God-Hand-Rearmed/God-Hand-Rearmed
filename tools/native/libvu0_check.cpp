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
#if defined(KAMITE_VU_SSE)
    const char *path = "SSE";
#elif defined(KAMITE_VU_NEON)
    const char *path = "NEON";
#else
    const char *path = "plain C++";
#endif
    std::printf("%s: %d vectors, %d differ; RotMatrix %d vectors, %d differ\n", path, count, bad - rotBad, rotCount, rotBad);
    return bad ? 1 : 0;
}
