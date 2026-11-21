#pragma once

// The DMA chain walker for the GIF / VIF1 / VIF0 channels' source-chain mode (PS2Memory::writeIORegister, CHCR with
// MOD = 1). Native road rung 3, step 0 (claude/native-next.md): the walk first lists the guest bytes the chain sends
// as pieces (adjacent ones merged), then the caller copies them in one go. It used to grow the buffer with a vector
// insert per tag (two with TTE) and translate every address through PS2Memory::translateAddress: ~5% of the game
// thread in crowded scenes (34,000 tags, 7.6 MB a frame on K's casino capture). The bytes sent, their order and the
// registers left behind are exactly the old walker's (tools/dma/chain_walk_check.cpp runs both on K's RAM captures).

#include "runtime/ps2_byte_buffer.h"
#include "runtime/ps2_memory.h"

#include <cstdint>
#include <cstring>
#include <vector>

namespace ps2x::dma
{
    struct ChainPiece
    {
        const uint8_t *src;
        uint32_t len;
    };

    // The channel's registers going in (TADR, ASR0 / ASR1, CHCR's ASP and tag bits) and coming out.
    struct ChainWalk
    {
        uint32_t tagAddr = 0u;
        uint32_t asr0 = 0u;
        uint32_t asr1 = 0u;
        uint32_t asp = 0u;
        uint32_t lastTagUpper = 0u;
        int tagsProcessed = 0;
        bool looped = false;
    };

    constexpr int kMaxChainTags = 1 << 20;

    // translate(address) is PS2Memory::translateAddress (it may throw); it is only called for addresses past the RAM
    // backing, where it returns the address itself for everything below (addresses under 0x20000000 that are not the
    // scratchpad's are taken as physical). tie = CHCR.TIE, tagData = TTE on a VIF channel (each tag's upper 64 bits go
    // in front of its payload). Returns false if the chain looped or had no end (the caller drops it).
    template <typename Translate>
    bool walkChain(ChainWalk &w, bool tie, bool tagData, const uint8_t *rdram, const uint8_t *scratchpad,
                   Translate &&translate, std::vector<ChainPiece> &pieces, size_t &bytesOut)
    {
        pieces.clear();
        size_t total = 0u;
        auto add = [&](const uint8_t *src, uint32_t len)
        {
            if (!pieces.empty() && pieces.back().src + pieces.back().len == src)
                pieces.back().len += len;
            else
                pieces.push_back(ChainPiece{src, len});
            total += len;
        };
        auto phys = [&](uint32_t address) -> uint32_t
        {
            return address < PS2_RDRAM_SIZE ? address : translate(address);
        };
        auto scratch = [](uint32_t address) { return address >= PS2_RDRAM_SIZE && ps2IsScratchpadAddress(address); };

        auto appendData = [&](uint32_t srcAddr, uint32_t qwCount)
        {
            const uint64_t bytes64 = static_cast<uint64_t>(qwCount) * 16ull;
            uint32_t bytes = (bytes64 > 0xFFFFFFFFull) ? 0xFFFFFFFFu : static_cast<uint32_t>(bytes64);
            const bool spr = scratch(srcAddr);
            uint32_t src = phys(srcAddr);
            const uint8_t *base = spr ? scratchpad : rdram;
            const uint32_t maxSz = spr ? PS2_SCRATCHPAD_SIZE : PS2_RDRAM_SIZE;
            while (bytes > 0)
            {
                if (src >= maxSz)
                    src = 0;
                uint32_t chunk = bytes;
                if (src + chunk > maxSz)
                    chunk = maxSz - src;
                if (chunk == 0)
                    break;
                add(base + src, chunk);
                bytes -= chunk;
                src += chunk;
            }
        };

        uint32_t tagAddr = w.tagAddr, asr0 = w.asr0, asr1 = w.asr1, asp = w.asp;
        uint32_t lastTagUpper = w.lastTagUpper;
        int tagsProcessed = 0;
        // A chain that comes back to a tag in the same walker state (address, call stack) loops forever: the PS2's
        // DMA would hang there. Brent's cycle check finds it exactly at no cost per tag.
        uint32_t loopTag = tagAddr, loopAsp = asp, loopAsr0 = asr0, loopAsr1 = asr1;
        int loopPower = 1, loopLength = 0;
        bool looped = false;

        while (tagsProcessed < kMaxChainTags)
        {
            const uint32_t currentTagAddr = tagAddr;
            const bool tagInSPR = scratch(tagAddr);
            uint32_t physTag = 0;
            if (tagAddr < PS2_RDRAM_SIZE)
                physTag = tagAddr;
            else
            {
                try
                {
                    physTag = translate(tagAddr);
                }
                catch (...)
                {
                    break;
                }
            }
            const uint8_t *tagBase = tagInSPR ? scratchpad : rdram;
            const uint32_t tagMax = tagInSPR ? PS2_SCRATCHPAD_SIZE : PS2_RDRAM_SIZE;
            if (physTag + 16 > tagMax)
                break;

            const uint8_t *tp = tagBase + physTag;
            uint64_t tag;
            std::memcpy(&tag, tp, sizeof(tag));
            const uint16_t tagQwc = static_cast<uint16_t>(tag & 0xFFFF);
            const uint32_t id = static_cast<uint32_t>((tag >> 28) & 0x7);
            const bool irq = ((tag >> 31) & 0x1ull) != 0ull;
            const uint32_t addr = static_cast<uint32_t>((tag >> 32) & 0x7FFFFFFF);
            lastTagUpper = static_cast<uint32_t>((tag >> 16) & 0xFFFFu);
            ++tagsProcessed;

            uint32_t dataAddr = 0;
            bool hasPayload = (tagQwc > 0);
            bool endChain = false;

            switch (id)
            {
            case 0: // refe
                dataAddr = addr;
                tagAddr = tagAddr + 16;
                endChain = true;
                break;
            case 1: // cnt
                dataAddr = tagAddr + 16;
                tagAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                break;
            case 2: // next
                dataAddr = tagAddr + 16;
                tagAddr = addr;
                break;
            case 3: // ref
            case 4: // refs
                dataAddr = addr;
                tagAddr = tagAddr + 16;
                break;
            case 5: // call
                dataAddr = tagAddr + 16;
                {
                    const uint32_t retAddr = dataAddr + static_cast<uint32_t>(tagQwc) * 16u;
                    if (asp == 0u)
                    {
                        asr0 = retAddr;
                        asp = 1u;
                    }
                    else if (asp == 1u)
                    {
                        asr1 = retAddr;
                        asp = 2u;
                    }
                }
                tagAddr = addr;
                break;
            case 6: // ret
                dataAddr = tagAddr + 16;
                if (asp == 2u)
                {
                    tagAddr = asr1;
                    asp = 1u;
                }
                else if (asp == 1u)
                {
                    tagAddr = asr0;
                    asp = 0u;
                }
                else
                {
                    endChain = true;
                }
                break;
            case 7: // end
                dataAddr = tagAddr + 16;
                endChain = true;
                break;
            default:
                hasPayload = false;
                endChain = true;
                break;
            }

            if (tagData)
            {
                // The tag's own upper 64 bits, from the tag as the old walker read them (translated again: the same
                // address, so the same place).
                const bool localScratch = scratch(currentTagAddr);
                const uint32_t localPhys = phys(currentTagAddr);
                const uint8_t *localBase = localScratch ? scratchpad : rdram;
                const uint32_t localMax = localScratch ? PS2_SCRATCHPAD_SIZE : PS2_RDRAM_SIZE;
                if (localPhys + 16u <= localMax)
                    add(localBase + localPhys + 8u, 8u);
            }

            if (hasPayload)
                appendData(dataAddr, tagQwc);
            if (irq && tie)
                endChain = true;
            if (endChain)
                break;
            if (tagAddr == loopTag && asp == loopAsp && asr0 == loopAsr0 && asr1 == loopAsr1)
            {
                looped = true;
                break;
            }
            if (++loopLength == loopPower)
            {
                loopTag = tagAddr;
                loopAsp = asp;
                loopAsr0 = asr0;
                loopAsr1 = asr1;
                loopPower <<= 1;
                loopLength = 0;
            }
        }

        w.tagAddr = tagAddr;
        w.asr0 = asr0;
        w.asr1 = asr1;
        w.asp = asp;
        w.lastTagUpper = lastTagUpper;
        w.tagsProcessed = tagsProcessed;
        w.looped = looped;
        bytesOut = total;
        return !(looped || tagsProcessed >= kMaxChainTags);
    }

    // Copies the pieces into out (resized to bytes, no zero fill first).
    inline void gatherChain(const std::vector<ChainPiece> &pieces, size_t bytes, ByteBuffer &out)
    {
        out.resize(bytes);
        uint8_t *dst = out.data();
        for (const ChainPiece &p : pieces)
        {
            std::memcpy(dst, p.src, p.len);
            dst += p.len;
        }
    }

    // Dev builds' check of rung 3 (claude/native-next.md): called with every chain the walker built, on the thread that
    // started the DMA, before it is queued. Unset (nullptr) in play and releases.
    using ChainObserver = void (*)(uint32_t channelBase, const uint8_t *data, size_t size);
    void setChainObserver(ChainObserver observer);
}
