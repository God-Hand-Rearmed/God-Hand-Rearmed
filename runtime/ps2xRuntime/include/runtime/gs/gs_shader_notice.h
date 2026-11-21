#pragma once

// The "Preparing shaders" notice of the PCSX2 GS's first start (gs_pcsx2_backend.cpp): from the GS's open until its
// first frame is presented, which on a new shader cache is the time PCSX2 compiles its shaders for the GPU (README:
// "the screen may stay black"). Its arithmetic only: no window, no file, no clock (times are seconds passed in), so
// test/shader_notice_test.cpp checks it on its own.
//
// The count comes from outside PCSX2: its shader caches write one fixed-size entry to an index file per compiled
// shader and flush it at once (VKShaderCache::CompileAndAddShaderSPV, D3D11ShaderCache::CompileAndAddShaderBlob), so
// the index's growth since the open is the number of shaders compiled. kShaderIndexes has the files and their layout;
// gs_pcsx2_backend_test checks the Vulkan one against the real library.

#include <cstdint>
#include <cstdio>
#include <string>

namespace ps2x::gs
{
    // One shader cache index in PCSX2's cache folder: a header, then entryBytes per shader.
    struct ShaderIndexFormat
    {
        const char *name;
        uint32_t headerBytes;
        uint32_t entryBytes;
    };

    // Vulkan: u32 version + VK_PIPELINE_CACHE_HEADER (32 bytes), entries of 2 u64 + 4 u32. Direct3D 11: u32 version,
    // entries of 6 u64 + 4 u32, one file per shader model (D3D::ShaderModelToCacheString).
    inline constexpr ShaderIndexFormat kShaderIndexes[] = {
        {"vulkan_shaders.idx", 36u, 32u},
        {"d3d_shaders_sm40.idx", 4u, 64u},
        {"d3d_shaders_sm41.idx", 4u, 64u},
        {"d3d_shaders_sm50.idx", 4u, 64u},
    };
    inline constexpr size_t kShaderIndexCount = sizeof(kShaderIndexes) / sizeof(kShaderIndexes[0]);

    // One look at an index file: there or not, its size, and its first headerBytes (the header).
    struct ShaderIndexSample
    {
        bool exists = false;
        uint64_t size = 0u;
        std::string head;
    };

    // Shaders added to one index since begin(). PCSX2 starts the file again when it cannot use it (another GPU or
    // driver, a new cache version, a damaged file): it shrinks, or its header changes; the count then starts from 0.
    class ShaderIndexCounter
    {
    public:
        void begin(const ShaderIndexFormat &format, const ShaderIndexSample &baseline)
        {
            m_format = format;
            m_base = baseline.exists ? entries(baseline.size) : 0u;
            m_head = baseline.head;
            m_last = baseline.size;
            m_seen = baseline.exists;
            m_compiled = 0u;
        }

        void observe(const ShaderIndexSample &sample)
        {
            if (!sample.exists)
            {
                m_base = 0u; // deleted (PCSX2 removes a file it cannot use, then writes it anew)
                m_last = 0u;
                return;
            }
            const bool restarted = sample.size < m_last || (!m_head.empty() && sample.head.size() >= m_head.size() &&
                                                            sample.head.compare(0, m_head.size(), m_head) != 0);
            if (restarted)
            {
                m_base = 0u;
                m_head = sample.head;
            }
            m_seen = true;
            m_last = sample.size;
            const uint64_t now = entries(sample.size);
            if (now > m_base && now - m_base > m_compiled)
                m_compiled = now - m_base;
        }

        uint64_t compiled() const { return m_compiled; }
        // The file was there at some point (else nothing can be said about this API's shaders).
        bool seen() const { return m_seen; }

    private:
        uint64_t entries(uint64_t size) const
        {
            return (m_format.entryBytes && size > m_format.headerBytes) ? (size - m_format.headerBytes) / m_format.entryBytes : 0u;
        }

        ShaderIndexFormat m_format{"", 0u, 1u};
        uint64_t m_base = 0u;
        uint64_t m_last = 0u;
        uint64_t m_compiled = 0u;
        std::string m_head;
        bool m_seen = false;
    };

    // Every index in kShaderIndexes: shaders compiled since begin() over all of them, -1 when none was ever there (the
    // shader cache is off, or a PCSX2 that names its files otherwise).
    class ShaderIndexSet
    {
    public:
        void begin(const ShaderIndexSample (&baseline)[kShaderIndexCount])
        {
            for (size_t i = 0; i < kShaderIndexCount; ++i)
                m_counters[i].begin(kShaderIndexes[i], baseline[i]);
        }
        void observe(const ShaderIndexSample (&samples)[kShaderIndexCount])
        {
            for (size_t i = 0; i < kShaderIndexCount; ++i)
                m_counters[i].observe(samples[i]);
        }
        int64_t compiled() const
        {
            int64_t total = -1;
            for (const ShaderIndexCounter &counter : m_counters)
                if (counter.seen())
                    total = (total < 0 ? 0 : total) + static_cast<int64_t>(counter.compiled());
            return total;
        }

    private:
        ShaderIndexCounter m_counters[kShaderIndexCount];
    };

    class ShaderNotice
    {
    public:
        // A warm cache opens in well under this: the notice never flashes up for it.
        static constexpr double kShowAfterSeconds = 0.5;
        static constexpr double kSweepSeconds = 1.6; // one pass of the bar's moving block, there and back
        static constexpr float kBlockWidth = 0.28f;  // the block, as a fraction of the bar

        void start(double now)
        {
            m_start = now;
            m_started = true;
            m_finished = false;
        }
        void finish(double now)
        {
            if (!m_started || m_finished)
                return;
            m_end = now;
            m_finished = true;
        }
        void setShowAfter(double seconds) { m_showAfter = seconds; }
        // compiled < 0: unknown (no shader cache index was found).
        void setCompiled(int64_t compiled) { m_compiled = compiled; }

        bool started() const { return m_started; }
        bool finished() const { return m_finished; }
        int64_t compiled() const { return m_compiled; }
        double elapsed(double now) const
        {
            if (!m_started)
                return 0.0;
            const double end = m_finished ? m_end : now;
            return end > m_start ? end - m_start : 0.0;
        }
        bool visible(double now) const { return m_started && !m_finished && elapsed(now) >= m_showAfter; }

        static const char *title() { return "Preparing shaders for your GPU (first start can take a while)"; }

        // "12 s, 37 shaders compiled": the seconds from 1 s on ("0 s" reads as stuck), the count once there is one.
        std::string detail(double now) const
        {
            std::string text;
            const long long seconds = static_cast<long long>(elapsed(now));
            if (seconds >= 1)
                text = std::to_string(seconds) + " s";
            if (m_compiled > 0)
                text += (text.empty() ? "" : ", ") + std::to_string(m_compiled) +
                        (m_compiled == 1 ? " shader compiled" : " shaders compiled");
            return text;
        }

        // The bar's moving block at this time, as fractions of the bar (0 <= start < end <= 1): it slides from one
        // end to the other and back, so the bar moves while nothing says how far along the work is.
        static void block(double now, float &start, float &end)
        {
            double phase = now / kSweepSeconds;
            phase -= static_cast<double>(static_cast<long long>(phase));
            if (phase < 0.0)
                phase += 1.0;
            const double there = phase < 0.5 ? phase * 2.0 : (1.0 - phase) * 2.0; // 0 -> 1 -> 0
            start = static_cast<float>(there * (1.0 - kBlockWidth));
            end = start + kBlockWidth;
        }

        // The log line at the end: "[gs] PCSX2 GS: first frame 1234 ms after opening, 57 shaders compiled".
        std::string logLine() const
        {
            char text[160];
            const long long ms = static_cast<long long>(elapsed(m_end) * 1000.0 + 0.5);
            if (m_compiled >= 0)
                std::snprintf(text, sizeof(text), "[gs] PCSX2 GS: first frame %lld ms after opening, %lld shader%s compiled", ms,
                              static_cast<long long>(m_compiled), m_compiled == 1 ? "" : "s");
            else
                std::snprintf(text, sizeof(text), "[gs] PCSX2 GS: first frame %lld ms after opening, shader count unavailable", ms);
            return text;
        }

    private:
        double m_start = 0.0;
        double m_end = 0.0;
        double m_showAfter = kShowAfterSeconds;
        int64_t m_compiled = -1;
        bool m_started = false;
        bool m_finished = false;
    };

    // Draws the notice over the whole game window with raylib, between BeginDrawing and EndDrawing
    // (gs_shader_notice_draw.cpp).
    void drawShaderNotice(const char *title, const char *detail, float blockStart, float blockEnd);
}
