// CRI ADX encoder / decoder (see adx_codec.h).
#include "kamite/adx_codec.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>

namespace kamite::adx
{
    namespace
    {
        constexpr int kFrameSamples = 32, kFrameBytes = 18, kHighPass = 500;

        uint16_t be16(const uint8_t *p) { return static_cast<uint16_t>((p[0] << 8) | p[1]); }
        uint32_t be32(const uint8_t *p) { return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3]; }
        void put16(std::vector<uint8_t> &b, size_t at, uint32_t v)
        {
            b[at] = static_cast<uint8_t>(v >> 8);
            b[at + 1] = static_cast<uint8_t>(v);
        }
        void put32(std::vector<uint8_t> &b, size_t at, uint32_t v)
        {
            put16(b, at, v >> 16);
            put16(b, at + 2, v & 0xFFFFu);
        }

        int clamp16(int v) { return v < -32768 ? -32768 : v > 32767 ? 32767 : v; }

        struct History
        {
            int s1 = 0, s2 = 0;
        };

        // One frame of one channel at a step (the decoder's scale + 1): the nibbles and the squared error.
        double quantize(const int *x, int step, int c1, int c2, History &h, int8_t *nibbles)
        {
            double error = 0.0;
            for (int i = 0; i < kFrameSamples; ++i)
            {
                const int predicted = (c1 * h.s1 + c2 * h.s2) >> 12;
                const double r = static_cast<double>(x[i] - predicted) / step;
                int n = static_cast<int>(std::lround(r));
                n = n < -8 ? -8 : n > 7 ? 7 : n;
                const int s = clamp16(n * step + predicted);
                nibbles[i] = static_cast<int8_t>(n);
                const double d = static_cast<double>(s - x[i]);
                error += d * d;
                h.s2 = h.s1;
                h.s1 = s;
            }
            return error;
        }
    }

    void coefficients(uint32_t rate, int highPass, int &c1, int &c2)
    {
        // func_0033E6B8, in single precision as the library does it (cvt.w.s truncates).
        const float root2 = std::sqrt(2.0f);
        const float a = root2 - std::cos(6.2831850f * static_cast<float>(highPass) / static_cast<float>(rate));
        const float b = root2 - 1.0f;
        const float c = (a - std::sqrt((a + b) * (a - b))) / b;
        c1 = static_cast<int>((c + c) * 4096.0f);
        c2 = static_cast<int>(-c * c * 4096.0f);
    }

    Info readInfo(const uint8_t *d, size_t size)
    {
        Info info;
        if (!d || size < 0x14 || be16(d) != 0x8000u)
            return info;
        const uint32_t copyright = be16(d + 2);
        if (d[4] != 3 || d[5] != kFrameBytes || d[6] != 4 || d[7] == 0 || copyright < 0x10)
            return info;
        info.valid = true;
        info.channels = d[7];
        info.rate = be32(d + 8);
        info.samples = be32(d + 12);
        info.version = d[0x12];
        info.flags = d[0x13];
        info.dataStart = copyright + 4u;
        // func_0033EBD0: the loop fields of version 4 at 0x20 (copyright offset >= 0x38), of version 3 at 0x14 (>= 0x2C).
        const size_t at = info.version == 4 ? 0x20u : 0x14u, need = info.version == 4 ? 0x38u : 0x2Cu;
        if ((info.version == 3 || info.version == 4) && copyright >= need && size >= at + 0x18u && be16(d + at + 2) == 1u &&
            be16(d + at + 6) != 0u)
        {
            info.loops = true;
            info.loopStart = be32(d + at + 8);
            info.loopEnd = be32(d + at + 16);
        }
        return info;
    }

    std::vector<uint8_t> encode(const int16_t *pcm, size_t frames, int channels, uint32_t rate, const Loop &loopIn)
    {
        std::vector<uint8_t> out;
        if (!pcm || frames == 0 || channels < 1 || channels > 8 || rate == 0)
            return out;
        Loop loop = loopIn;
        if (loop.on && (loop.end == 0 || loop.end > frames))
            loop.end = static_cast<uint32_t>(frames);
        if (loop.on && loop.start >= loop.end)
            loop.on = false;
        // The loop starts on a frame: silence before the track moves it there (the header says how much).
        const uint32_t pad = loop.on ? (kFrameSamples - loop.start % kFrameSamples) % kFrameSamples : 0u;
        const uint64_t total = frames + pad;
        const uint64_t frameCount = (total + kFrameSamples - 1) / kFrameSamples;
        const uint32_t frameSet = static_cast<uint32_t>(kFrameBytes * channels); // one frame of every channel
        uint32_t dataStart = 0x28u;
        if (loop.on)
        {
            const uint32_t into = ((loop.start + pad) / kFrameSamples) * frameSet;
            dataStart = (0x800u - into % 0x800u) % 0x800u;
            while (dataStart < 0x3Cu)
                dataStart += 0x800u;
        }
        out.assign(dataStart + frameCount * frameSet + kFrameBytes, 0u);
        put16(out, 0, 0x8000u);
        put16(out, 2, dataStart - 4u);
        out[4] = 3;
        out[5] = kFrameBytes;
        out[6] = 4;
        out[7] = static_cast<uint8_t>(channels);
        put32(out, 8, rate);
        put32(out, 12, static_cast<uint32_t>(total));
        put16(out, 0x10, kHighPass);
        out[0x12] = 4;
        out[0x13] = 0;
        if (loop.on)
        {
            const uint32_t start = loop.start + pad, end = loop.end + pad;
            put16(out, 0x20, pad);
            put16(out, 0x22, 1u);
            put16(out, 0x24, 0u);
            put16(out, 0x26, 1u);
            put32(out, 0x28, start);
            put32(out, 0x2C, dataStart + (start / kFrameSamples) * frameSet);
            put32(out, 0x30, end);
            put32(out, 0x34, dataStart + ((end + kFrameSamples - 1) / kFrameSamples) * frameSet);
        }
        std::memcpy(out.data() + dataStart - 6, "(c)CRI", 6);

        int c1 = 0, c2 = 0;
        coefficients(rate, kHighPass, c1, c2);
        std::vector<History> history(static_cast<size_t>(channels));
        int x[kFrameSamples];
        int8_t nibbles[kFrameSamples], best[kFrameSamples];
        for (uint64_t f = 0; f < frameCount; ++f)
            for (int ch = 0; ch < channels; ++ch)
            {
                for (int i = 0; i < kFrameSamples; ++i)
                {
                    const uint64_t n = f * kFrameSamples + i; // with the padding
                    x[i] = n < pad || n >= total ? 0 : pcm[(n - pad) * channels + ch];
                }
                // The step from the frame's residuals as if each sample came out exact, then a few larger ones (the
                // decoded history drifts from the input): the one with the least error.
                History ideal = history[ch];
                int peak = 0;
                for (int i = 0; i < kFrameSamples; ++i)
                {
                    const int predicted = (c1 * ideal.s1 + c2 * ideal.s2) >> 12;
                    peak = std::max(peak, std::abs(x[i] - predicted));
                    ideal.s2 = ideal.s1;
                    ideal.s1 = x[i];
                }
                const int base = std::max(1, (peak + 6) / 7);
                double bestError = -1.0;
                int bestStep = base;
                History bestHistory;
                for (const int step : {base, base + base / 8 + 1, base + base / 4 + 1, base + base / 2 + 1})
                {
                    const int s = std::min(step, 0x2000);
                    History h = history[ch];
                    const double e = quantize(x, s, c1, c2, h, nibbles);
                    if (bestError < 0.0 || e < bestError)
                    {
                        bestError = e;
                        bestStep = s;
                        bestHistory = h;
                        std::memcpy(best, nibbles, sizeof(best));
                    }
                    if (e == 0.0)
                        break;
                }
                history[ch] = bestHistory;
                uint8_t *frame = out.data() + dataStart + f * frameSet + static_cast<uint64_t>(ch) * kFrameBytes;
                frame[0] = static_cast<uint8_t>((bestStep - 1) >> 8);
                frame[1] = static_cast<uint8_t>(bestStep - 1);
                for (int i = 0; i < kFrameSamples / 2; ++i)
                    frame[2 + i] = static_cast<uint8_t>(((best[2 * i] & 0xF) << 4) | (best[2 * i + 1] & 0xF));
            }
        // The end frame the disc's files have.
        uint8_t *end = out.data() + dataStart + frameCount * frameSet;
        end[0] = 0x80;
        end[1] = 0x01;
        end[3] = 0x0E;
        return out;
    }

    bool decode(const std::vector<uint8_t> &adx, std::vector<int16_t> &pcm)
    {
        const Info info = readInfo(adx.data(), adx.size());
        if (!info.valid || info.flags >= 8)
            return false;
        int c1 = 0, c2 = 0;
        coefficients(info.rate, be16(adx.data() + 0x10), c1, c2);
        const int channels = info.channels;
        pcm.assign(static_cast<size_t>(info.samples) * channels, 0);
        std::vector<History> history(static_cast<size_t>(channels));
        const uint64_t frameSet = static_cast<uint64_t>(kFrameBytes) * channels;
        for (uint64_t f = 0; f * kFrameSamples < info.samples; ++f)
            for (int ch = 0; ch < channels; ++ch)
            {
                const uint64_t at = info.dataStart + f * frameSet + static_cast<uint64_t>(ch) * kFrameBytes;
                if (at + kFrameBytes > adx.size())
                    return false;
                const uint8_t *frame = adx.data() + at;
                if (frame[0] & 0x80)
                    return false;
                const int step = (be16(frame) & 0x1FFF) + 1;
                History &h = history[ch];
                for (int i = 0; i < kFrameSamples; ++i)
                {
                    const uint8_t byte = frame[2 + i / 2];
                    int n = (i & 1) ? (byte & 0xF) : (byte >> 4);
                    n = n >= 8 ? n - 16 : n;
                    const int s = clamp16(n * step + ((c1 * h.s1 + c2 * h.s2) >> 12));
                    h.s2 = h.s1;
                    h.s1 = s;
                    const uint64_t sample = f * kFrameSamples + i;
                    if (sample < info.samples)
                        pcm[sample * channels + ch] = static_cast<int16_t>(s);
                }
            }
        return true;
    }
}
