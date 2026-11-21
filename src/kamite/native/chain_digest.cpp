#include "kamite/native/chain_digest.h"

#if !defined(KAMITE_RELEASE)

#include "kamite/test_mode.h"
#include "runtime/ps2_dma_chain.h"

#define XXH_INLINE_ALL
#include "../../../thirdparty/pcsx2/3rdparty/include/xxhash.h"

#include <cstdint>
#include <cstdio>
#include <mutex>
#include <string>

namespace kamite::native::chain_digest
{
    namespace
    {
        std::mutex g_mutex;
        std::string g_path;
        std::FILE *g_file = nullptr;
        bool g_failed = false;

        void observe(uint32_t channelBase, const uint8_t *data, size_t size)
        {
            // Only the replay's own frames: before the state loads (boot, loading) the frame count follows the host.
            const uint32_t frame = test_mode::replayFrame();
            if (frame == test_mode::kReplayIdle)
                return;
            const uint64_t hash = XXH3_64bits(data, size);
            const uint32_t record[4] = {frame, (((channelBase >> 12) & 0xFu) << 28) | (static_cast<uint32_t>(size) & 0x0FFFFFFFu),
                                        static_cast<uint32_t>(hash), static_cast<uint32_t>(hash >> 32)};
            std::lock_guard<std::mutex> lock(g_mutex);
            if (!g_file)
            {
                if (g_failed)
                    return;
                g_file = std::fopen(g_path.c_str(), "wb");
                if (!g_file)
                {
                    g_failed = true;
                    std::fprintf(stderr, "[native] chain digest: cannot write %s\n", g_path.c_str());
                    return;
                }
                std::fwrite("GHRCHN1\0", 1, 8, g_file);
            }
            std::fwrite(record, sizeof(record), 1, g_file);
        }
    }

    void begin(const char *outputBase)
    {
        std::lock_guard<std::mutex> lock(g_mutex);
        g_path = std::string(outputBase) + ".chain.bin";
        std::remove(g_path.c_str()); // a previous run's (Replay-Cases.cmd deletes it too)
        ps2x::dma::setChainObserver(&observe);
        std::fprintf(stderr, "[native] chain digest: %s\n", g_path.c_str());
    }

    void end()
    {
        ps2x::dma::setChainObserver(nullptr);
        std::lock_guard<std::mutex> lock(g_mutex);
        if (g_file)
        {
            std::fclose(g_file);
            g_file = nullptr;
        }
    }
}

#else // KAMITE_RELEASE: no digest

namespace kamite::native::chain_digest
{
    void begin(const char *) {}
    void end() {}
}

#endif
