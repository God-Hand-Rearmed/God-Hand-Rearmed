// Rung 3 step 0's proof (claude/native-next.md): the new DMA chain walker (runtime/ps2_dma_chain.h) against the old one
// (old_walker_body.inc: ps2_memory.cpp's walker before step 0, verbatim) on K's RAM captures and on random chains.
// Every chain's bytes, the registers left behind (TADR, ASR0/1, ASP, CHCR's tag bits) and a dropped chain must match.
//
//   g++ -std=c++20 -O2 -msse4.1 -I runtime/ps2xRuntime/include -I runtime/ps2xIOP/include tools/dma/chain_walk_check.cpp \
//       -o /tmp/chain_walk_check
//   /tmp/chain_walk_check [ram_<tick>.bin ...]      (F8 captures from K's play folder; the chain heads are at 0x754C80)
//   /tmp/chain_walk_check --bench ram_<tick>.bin    (time per walk, old vs new)

#include "runtime/ps2_dma_chain.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <random>
#include <stdexcept>
#include <string>
#include <vector>

namespace
{
    std::vector<uint8_t> g_rdram(PS2_RDRAM_SIZE), g_spr(PS2_SCRATCHPAD_SIZE);
    uint8_t *m_rdram = g_rdram.data();
    uint8_t *m_scratchpad = g_spr.data();

    bool isScratchpad(uint32_t a) { return ps2IsScratchpadAddress(a); }
    // PS2Memory::translateAddress for what a chain can name (no TLB here: KSEG2/3 throw, as an unmapped one does).
    uint32_t translateAddress(uint32_t a)
    {
        if (isScratchpad(a))
            return ps2ScratchpadOffset(a);
        if (a >= 0x20000000u && a < 0x40000000u)
            return a & (PS2_RDRAM_SIZE - 1u);
        if (a >= 0x80000000u && a < 0xC0000000u)
            return a & 0x1FFFFFFFu;
        if (a < 0x80000000u)
            return a;
        throw std::runtime_error("unmapped");
    }
    void inRange(uint32_t offset, size_t bytes, size_t regionSize, const char *op, uint32_t address)
    {
        if (uint64_t(offset) + bytes > regionSize)
            throw std::runtime_error(std::string(op) + std::to_string(address));
    }
    template <typename T>
    T loadScalar(const uint8_t *base, uint32_t offset, size_t regionSize, const char *op, uint32_t address)
    {
        inRange(offset, sizeof(T), regionSize, op, address);
        T v{};
        std::memcpy(&v, base + offset, sizeof(T));
        return v;
    }
    std::vector<uint8_t> takeChainBuffer() { return {}; }

    struct Result
    {
        std::vector<uint8_t> bytes;
        uint32_t regs[4] = {};
        bool threw = false;
        bool operator==(const Result &o) const
        {
            return threw == o.threw && bytes == o.bytes && std::memcmp(regs, o.regs, sizeof(regs)) == 0;
        }
    };

    // The verbatim body indexes m_ioRegisters[channelBase + ...]: a view over this walk's channel registers.
    struct Regs
    {
        uint32_t base[0x20] = {};
        uint32_t channel = 0;
        uint32_t &operator[](uint32_t i) { return base[(i - channel) >> 2]; }
    };

    Result oldWalk(uint32_t channelBase, uint32_t chcr, uint32_t tadr, uint32_t asr0In, uint32_t asr1In)
    {
        Result r;
        Regs m_ioRegisters;
        m_ioRegisters.channel = channelBase;
        m_ioRegisters[channelBase + 0x30] = tadr;
        m_ioRegisters[channelBase + 0x40] = asr0In;
        m_ioRegisters[channelBase + 0x50] = asr1In;
        try
        {
#include "old_walker_body.inc"
            r.bytes = std::move(chainBuf);
            r.regs[0] = m_ioRegisters[channelBase + 0x30];
            r.regs[1] = m_ioRegisters[channelBase + 0x40];
            r.regs[2] = m_ioRegisters[channelBase + 0x50];
            r.regs[3] = m_ioRegisters[channelBase + 0x00];
        }
        catch (...)
        {
            r = Result{};
            r.threw = true;
        }
        return r;
    }

    std::vector<ps2x::dma::ChainPiece> g_pieces;
    Result newWalk(uint32_t channelBase, uint32_t chcr, uint32_t tadr, uint32_t asr0In, uint32_t asr1In)
    {
        Result r;
        try
        {
            ps2x::dma::ChainWalk walk;
            walk.tagAddr = tadr;
            walk.asr0 = asr0In;
            walk.asr1 = asr1In;
            walk.asp = (chcr >> 4) & 0x3u;
            walk.lastTagUpper = (chcr >> 16) & 0xFFFFu;
            const bool tie = (chcr & (1u << 7)) != 0u;
            const bool vif = channelBase == 0x10009000u || channelBase == 0x10008000u;
            const bool tte = vif && (chcr & 0x40u) != 0u;
            size_t bytes = 0;
            const bool kept = ps2x::dma::walkChain(walk, tie, tte, m_rdram, m_scratchpad, translateAddress, g_pieces, bytes);
            ps2x::ByteBuffer out;
            if (kept && bytes)
                ps2x::dma::gatherChain(g_pieces, bytes, out);
            r.bytes.assign(out.begin(), out.end());
            r.regs[0] = walk.tagAddr;
            r.regs[1] = walk.asr0;
            r.regs[2] = walk.asr1;
            uint32_t c = chcr;
            c = (c & ~(0x3u << 4)) | ((walk.asp & 0x3u) << 4);
            c = (c & 0x0000FFFFu) | (walk.lastTagUpper << 16);
            r.regs[3] = c;
        }
        catch (...)
        {
            r = Result{};
            r.threw = true;
        }
        return r;
    }

    int g_fail = 0, g_checked = 0;
    void compare(const char *what, uint32_t ch, uint32_t chcr, uint32_t tadr, uint32_t a0 = 0, uint32_t a1 = 0)
    {
        const Result o = oldWalk(ch, chcr, tadr, a0, a1), n = newWalk(ch, chcr, tadr, a0, a1);
        ++g_checked;
        if (!(o == n))
        {
            if (++g_fail <= 10)
                std::printf("MISMATCH %s ch %08x chcr %08x tadr %08x: bytes %zu / %zu, threw %d / %d, regs %08x %08x %08x %08x / "
                            "%08x %08x %08x %08x\n",
                            what, ch, chcr, tadr, o.bytes.size(), n.bytes.size(), o.threw, n.threw, o.regs[0], o.regs[1],
                            o.regs[2], o.regs[3], n.regs[0], n.regs[1], n.regs[2], n.regs[3]);
        }
    }

    const uint32_t kChannels[3] = {0x10009000u, 0x1000A000u, 0x10008000u};

    void randomChains(uint64_t seed, int count)
    {
        std::mt19937_64 rng(seed);
        auto pick = [&](uint32_t n) { return uint32_t(rng() % n); };
        for (int c = 0; c < count; ++c)
        {
            // A small world: tags in a 64 KB window (sometimes near the RAM backing's end, sometimes the scratchpad).
            const uint32_t kind = pick(6);
            uint32_t window = kind == 0 ? PS2_RDRAM_SIZE - 0x8000u : kind == 1 ? 0x70000000u : 0x00100000u + pick(0x400) * 0x1000u;
            uint32_t windowPhys = translateAddress(window);
            uint8_t *base = (window == 0x70000000u) ? m_scratchpad : m_rdram;
            const uint32_t span = window == 0x70000000u ? 0x4000u : 0x8000u;
            for (uint32_t i = 0; i < span; i += 8)
            {
                uint64_t v = rng();
                std::memcpy(base + ((windowPhys + i) % (window == 0x70000000u ? PS2_SCRATCHPAD_SIZE : PS2_RDRAM_SIZE)), &v, 8);
            }
            const int ntags = 1 + pick(40);
            uint32_t at = window + (pick(span / 16 / 2) * 16);
            for (int t = 0; t < ntags; ++t)
            {
                const uint32_t id = pick(10) < 8 ? pick(8) : pick(3) + 1;
                const uint32_t qwc = pick(4) == 0 ? 0 : pick(pick(8) == 0 ? 0x10000 : 40);
                uint32_t addr;
                switch (pick(8))
                {
                case 0: addr = 0x70000000u + pick(0x400) * 16; break;
                case 1: addr = 0x20000000u | (window + pick(span / 16) * 16); break;
                case 2: addr = PS2_RDRAM_SIZE - pick(64) * 16; break;
                case 3: addr = 0x10000000u + pick(0x100) * 16; break;
                default: addr = window + pick(span / 16) * 16; break;
                }
                const uint64_t irq = pick(16) == 0 ? 1ull : 0ull;
                const uint64_t tag = uint64_t(qwc) | (uint64_t(pick(4)) << 26) | (uint64_t(id) << 28) | (irq << 31) |
                                     (uint64_t(addr & 0x7FFFFFFFu) << 32);
                uint32_t p;
                try { p = translateAddress(at); } catch (...) { break; }
                uint8_t *b = isScratchpad(at) ? m_scratchpad : m_rdram;
                const uint32_t lim = isScratchpad(at) ? PS2_SCRATCHPAD_SIZE : PS2_RDRAM_SIZE;
                if (p + 16 > lim)
                    break;
                std::memcpy(b + p, &tag, 8);
                at += 16 + (id == 1 ? qwc * 16 : (pick(3) == 0 ? pick(64) * 16 : 0));
            }
            const uint32_t ch = kChannels[pick(3)];
            uint32_t chcr = 0x105u | (pick(2) << 6) | (pick(2) << 7) | (pick(3) << 4) | (pick(0x10000) << 16);
            uint32_t tadr = window + (pick(span / 16 / 2) * 16);
            if (pick(10) == 0)
                tadr = 0x80000000u | tadr;
            if (pick(20) == 0)
                tadr = 0xC0000000u | pick(0x1000) * 16; // unmapped: the old walker stops at once
            compare("random", ch, chcr, tadr, window + pick(span / 16) * 16, window + pick(span / 16) * 16);
        }
    }
}

int main(int argc, char **argv)
{
    bool bench = false;
    for (int i = 1; i < argc; ++i)
    {
        if (std::string(argv[i]) == "--bench")
        {
            bench = true;
            continue;
        }
        FILE *f = std::fopen(argv[i], "rb");
        if (!f)
        {
            std::printf("cannot open %s\n", argv[i]);
            return 2;
        }
        std::fill(g_rdram.begin(), g_rdram.end(), 0);
        std::fread(m_rdram, 1, 32u << 20, f);
        std::fclose(f);
        for (uint32_t b = 0; b < 2; ++b)
        {
            uint32_t head;
            std::memcpy(&head, m_rdram + 0x754C80 + b * 128, 4);
            for (uint32_t ch : kChannels)
                for (uint32_t chcr : {0x145u, 0x105u, 0x1C5u})
                    compare(argv[i], ch, chcr, head);
            const Result r = newWalk(0x10009000u, 0x145u, head, 0, 0);
            std::printf("%s buffer %u: chain %zu bytes, %zu pieces\n", argv[i], b, r.bytes.size(), g_pieces.size());
            if (bench)
            {
                for (int which = 0; which < 2; ++which)
                {
                    const auto t0 = std::chrono::steady_clock::now();
                    size_t sink = 0;
                    static ps2x::ByteBuffer kept; // the game's buffers come back with their capacity
                    for (int k = 0; k < 50; ++k)
                    {
                        if (which)
                        {
                            ps2x::dma::ChainWalk walk;
                            walk.tagAddr = head;
                            size_t bytes = 0;
                            kept.clear();
                            if (ps2x::dma::walkChain(walk, false, true, m_rdram, m_scratchpad, translateAddress, g_pieces, bytes))
                                ps2x::dma::gatherChain(g_pieces, bytes, kept);
                            sink += kept.size();
                        }
                        else
                            sink += oldWalk(0x10009000u, 0x145u, head, 0, 0).bytes.size();
                    }
                    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / 50;
                    std::printf("  %s walker: %.3f ms per walk (%zu)\n", which ? "new" : "old", ms, sink / 50);
                }
            }
        }
    }
    std::fill(g_rdram.begin(), g_rdram.end(), 0);
    randomChains(1, 200000);
    randomChains(2, 200000);
    std::printf("%d walks compared, %d mismatches\n", g_checked, g_fail);
    return g_fail ? 1 : 0;
}
