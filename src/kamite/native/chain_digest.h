#pragma once

// Rung 3's end-to-end check (claude/native-next.md; dev builds only, nothing in KAMITE_RELEASE): during a test-case
// replay, every DMA chain the runtime's walker builds (runtime/ps2_dma_chain.h: what the game sends to VU1 and the GS)
// is hashed into <case>.<30|60>.chain.bin. Replay-Cases.cmd fingerprints the file, so two builds (or a build with a
// native packet builder on and off) sent byte-identical chains when the lines match. tools/dma/chain_compare.py finds
// the first chain that differs between two such files.
//
// Only chains started during the replay's own frames count (test_mode::replayFrame; not the boot or the state load).
// File: "GHRCHN1\0", then one 16-byte record per chain: u32 test frame, u32 channel << 28 (0x8 VIF0, 0x9 VIF1, 0xA GIF)
// | bytes, u64 XXH3 of the bytes. No build stamp in it: identical chains give an identical file.

namespace kamite::native::chain_digest
{
    // During a replay (KAMITE_REPLAY_CASE): opens nothing yet, registers the walker's observer; the file is created at
    // the first chain. path = the case + ".30" / ".60" (the shadow harness's output base).
    void begin(const char *outputBase);
    // At exit (native::flush).
    void end();
}
