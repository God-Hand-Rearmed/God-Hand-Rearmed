#include "kamite/mot_sample.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace kamite::mot_sample
{
    namespace
    {
        // PlatinumGames' 16-bit float: sign 1, exponent 6 (bias 47), mantissa 9.
        float half(uint16_t v)
        {
            const uint32_t sign = (v >> 15) & 1u, expo = (v >> 9) & 0x3Fu, mant = v & 0x1FFu;
            if (expo == 0u && mant == 0u)
                return 0.0f;
            const uint32_t bits = (sign << 31) | ((expo - 47u + 127u) << 23) | (mant << 14);
            float f = 0.0f;
            std::memcpy(&f, &bits, 4);
            return f;
        }
        uint16_t rd16(const uint8_t *p)
        {
            uint16_t v = 0;
            std::memcpy(&v, p, 2);
            return v;
        }
        uint32_t rd32(const uint8_t *p)
        {
            uint32_t v = 0;
            std::memcpy(&v, p, 4);
            return v;
        }
        float windNear(float a, float near)
        {
            constexpr float kTurn = 6.2831853f;
            return a + kTurn * std::round((near - a) / kTurn);
        }
    }

    bool valid(const uint8_t *mot, size_t size)
    {
        return mot && size >= 8u && std::memcmp(mot, "mtb3", 4) == 0 && 8u + 12u * size_t(mot[6]) <= size;
    }

    uint32_t frames(const uint8_t *mot) { return rd16(mot + 4u); }

    // mtb3: u16 frames, u8 records, u8 loop; 12-byte records: u8 bone, u8 channel (high nibble 0x00 constant f32
    // payload / 0x10 Hermite curve at motion + payload; low nibble 0..5 = t xyz, r xyz; 0xFF ends the table), u16 key
    // count, u32 flags, u32 payload; curve: six half floats (value, value step, in tangent, its step, out tangent, its
    // step), then 4-byte keys (frames since the last, value step, in step, out step); between two keys a cubic
    // Hermite with the first's out and the second's in tangent.
    bool sample(const uint8_t *mot, size_t size, uint32_t bone, uint32_t component, float t, float &out)
    {
        if (!valid(mot, size))
            return false;
        bool found = false;
        const uint32_t records = mot[6];
        for (uint32_t i = 0; i < records; ++i)
        {
            const uint8_t *r = mot + 8u + 12u * i;
            const uint32_t channel = r[1];
            if (channel == 0xFFu)
                break;
            if (r[0] != bone || (channel & 0x0Fu) != component)
                continue;
            const uint32_t keys = rd16(r + 2u), payload = rd32(r + 8u);
            if ((channel & 0xF0u) == 0x00u)
            {
                std::memcpy(&out, &payload, 4);
                found = true;
                continue;
            }
            if ((channel & 0xF0u) != 0x10u || keys == 0u || size_t(payload) + 12u + 4u * size_t(keys) > size)
                continue; // an encoding not known here: as if unkeyed
            const uint8_t *c = mot + payload;
            const float p = half(rd16(c)), dp = half(rd16(c + 2u)), m0 = half(rd16(c + 4u)), dm0 = half(rd16(c + 6u)),
                        m1 = half(rd16(c + 8u)), dm1 = half(rd16(c + 10u));
            float f0 = 0.0f, v0 = 0.0f, out0 = 0.0f, value = 0.0f;
            uint32_t frame = 0u;
            for (uint32_t k = 0; k < keys; ++k)
            {
                const uint8_t *key = c + 12u + 4u * k;
                frame += key[0];
                const float f = static_cast<float>(frame), v = p + dp * key[1], in = m0 + dm0 * key[2], o = m1 + dm1 * key[3];
                if (k == 0u)
                {
                    value = v;
                    if (t < f)
                        break; // before the first key: its value
                }
                else if (f > t && f > f0)
                {
                    const float u = (t - f0) / (f - f0), u2 = u * u, u3 = u2 * u;
                    value = (2 * u3 - 3 * u2 + 1) * v0 + (u3 - 2 * u2 + u) * out0 + (-2 * u3 + 3 * u2) * v + (u3 - u2) * in;
                    break;
                }
                else
                    value = v; // at or past this key (the last key holds; of several on one frame the last counts)
                f0 = f, v0 = v, out0 = o;
            }
            out = value;
            found = true;
        }
        return found;
    }

    void eulerMatrix(const float e[3], float r[3][3])
    {
        const float cx = std::cos(e[0]), sx = std::sin(e[0]), cy = std::cos(e[1]), sy = std::sin(e[1]), cz = std::cos(e[2]),
                    sz = std::sin(e[2]);
        r[0][0] = cy * cz, r[0][1] = sx * sy * cz - cx * sz, r[0][2] = cx * sy * cz + sx * sz;
        r[1][0] = cy * sz, r[1][1] = sx * sy * sz + cx * cz, r[1][2] = cx * sy * sz - sx * cz;
        r[2][0] = -sy, r[2][1] = sx * cy, r[2][2] = cx * cy;
    }

    Quat matrixQuat(const float r[3][3])
    {
        Quat q;
        q.w = std::sqrt(std::max(0.0f, 1.0f + r[0][0] + r[1][1] + r[2][2])) * 0.5f;
        q.x = std::copysign(std::sqrt(std::max(0.0f, 1.0f + r[0][0] - r[1][1] - r[2][2])) * 0.5f, r[2][1] - r[1][2]);
        q.y = std::copysign(std::sqrt(std::max(0.0f, 1.0f - r[0][0] + r[1][1] - r[2][2])) * 0.5f, r[0][2] - r[2][0]);
        q.z = std::copysign(std::sqrt(std::max(0.0f, 1.0f - r[0][0] - r[1][1] + r[2][2])) * 0.5f, r[1][0] - r[0][1]);
        const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        return n > 0.0f ? Quat{q.w / n, q.x / n, q.y / n, q.z / n} : Quat{};
    }

    void quatMatrix(const Quat &q, float r[3][3])
    {
        const float w = q.w, x = q.x, y = q.y, z = q.z;
        r[0][0] = 1 - 2 * (y * y + z * z), r[0][1] = 2 * (x * y - w * z), r[0][2] = 2 * (x * z + w * y);
        r[1][0] = 2 * (x * y + w * z), r[1][1] = 1 - 2 * (x * x + z * z), r[1][2] = 2 * (y * z - w * x);
        r[2][0] = 2 * (x * z - w * y), r[2][1] = 2 * (y * z + w * x), r[2][2] = 1 - 2 * (x * x + y * y);
    }

    Quat eulerQuat(const float e[3])
    {
        // qz qy qx (R = Rz Ry Rx), straight from the half angles
        const float cx = std::cos(e[0] * 0.5f), sx = std::sin(e[0] * 0.5f), cy = std::cos(e[1] * 0.5f),
                    sy = std::sin(e[1] * 0.5f), cz = std::cos(e[2] * 0.5f), sz = std::sin(e[2] * 0.5f);
        return Quat{cz * cy * cx + sz * sy * sx, cz * cy * sx - sz * sy * cx, cz * sy * cx + sz * cy * sx,
                    sz * cy * cx - cz * sy * sx};
    }

    Quat mul(const Quat &a, const Quat &b)
    {
        return {a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z, a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
                a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x, a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w};
    }

    Quat slerp(const Quat &a, Quat b, float u)
    {
        float d = a.w * b.w + a.x * b.x + a.y * b.y + a.z * b.z;
        if (d < 0.0f)
            b = {-b.w, -b.x, -b.y, -b.z}, d = -d;
        float ka = 1.0f - u, kb = u;
        if (d < 0.9995f)
        {
            const float th = std::acos(d), s = std::sin(th);
            ka = std::sin((1.0f - u) * th) / s, kb = std::sin(u * th) / s;
        }
        Quat q{ka * a.w + kb * b.w, ka * a.x + kb * b.x, ka * a.y + kb * b.y, ka * a.z + kb * b.z};
        const float n = std::sqrt(q.w * q.w + q.x * q.x + q.y * q.y + q.z * q.z);
        return Quat{q.w / n, q.x / n, q.y / n, q.z / n};
    }

    bool eulerNear(const float r[3][3], const float near[3], float out[3])
    {
        const float x0 = std::atan2(r[2][1], r[2][2]);
        const float xs[3] = {x0, x0 + 3.14159265f, near[0]}; // the two branches; at y = +-90 deg any x
        float best = 1e30f;
        for (const float x : xs)
        {
            const float c = std::cos(x), s = std::sin(x);
            const float p01 = r[0][1] * c - r[0][2] * s, p11 = r[1][1] * c - r[1][2] * s;
            const float p20 = r[2][0], p22 = r[2][1] * s + r[2][2] * c;
            const float e[3] = {windNear(x, near[0]), windNear(std::atan2(-p20, p22), near[1]), windNear(std::atan2(-p01, p11), near[2])};
            float check[3][3];
            eulerMatrix(e, check);
            float err = 0.0f;
            for (int i = 0; i < 3; ++i)
                for (int j = 0; j < 3; ++j)
                    err = std::max(err, std::fabs(check[i][j] - r[i][j]));
            const float distance = std::fabs(e[0] - near[0]) + std::fabs(e[1] - near[1]) + std::fabs(e[2] - near[2]);
            if (err < 1e-3f && distance < best)
            {
                best = distance;
                std::copy(e, e + 3, out);
            }
        }
        return best < 1e30f;
    }
}
