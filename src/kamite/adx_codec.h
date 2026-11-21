#pragma once

#include <cstddef>
#include <cstdint>
#include <vector>

// CRI ADX, the game's audio format (every entry of bgm/bgm00.afs, se/*.afs, se/ev/*.afs): the encoder the mods' audio
// is converted with (mod_audio.h). What the game's library reads (ELF, Nov 20, claude/mod-support.md "Step 2"):
//   header 0x00 u16 0x8000, 0x02 u16 copyright offset (data at +4, "(c)CRI" 2 before it), 0x04 type 3, 0x05 frame
//   bytes 18, 0x06 4 bits, 0x07 channels, 0x08 u32 rate, 0x0C u32 samples, 0x10 u16 high-pass Hz (500), 0x12 version 4,
//   0x13 flags (the disc's are 8 = encrypted; 0 = plain: func_0033D090 takes no key below 8). Loop (version 4, copyright
//   offset >= 0x38; func_0033EBD0): 0x20 u16 padding samples, 0x22 u16 1, 0x24 u16 0, 0x26 u16 1, 0x28 u32 loop start
//   sample, 0x2C u32 its byte offset, 0x30 u32 loop end sample, 0x34 u32 its byte offset. The disc's looped files pad
//   the start so the loop starts on a frame and place the data so its byte offset is on a 2048-byte sector; so do we.
//   Data: frames of 32 samples per channel, interleaved by channel; a frame = u16 scale + 16 bytes of 4-bit samples
//   (high nibble first). ADX_DecodeSte4AsSte: s = nibble * ((scale & 0x1FFF) + 1) + ((c1 * s1 + c2 * s2) >> 12),
//   clamped; c1 / c2 from the high-pass frequency (func_0033E6B8). End: a frame with scale 0x8001.
// Plain std: the boot and the host tests.
namespace kamite::adx
{
    struct Info
    {
        bool valid = false;
        int channels = 0;
        uint32_t rate = 0, samples = 0;
        uint8_t version = 0, flags = 0;
        uint32_t dataStart = 0;
        bool loops = false;
        uint32_t loopStart = 0, loopEnd = 0; // samples (end: where it jumps back)
    };
    // The header of an ADX file (its first 0x40 bytes or more).
    Info readInfo(const uint8_t *data, size_t size);

    struct Loop
    {
        bool on = false;
        uint32_t start = 0, end = 0; // samples; end where it jumps back (0 or past the last sample: the last sample)
    };
    // 16-bit PCM (frames x channels, interleaved) -> an ADX file: version 4, type 3, 500 Hz high-pass, not encrypted.
    std::vector<uint8_t> encode(const int16_t *pcm, size_t frames, int channels, uint32_t rate, const Loop &loop);

    // The game's decoder on a plain (unencrypted) file, for checks: PCM interleaved, false when it can't.
    bool decode(const std::vector<uint8_t> &adx, std::vector<int16_t> &pcm);

    // The library's prediction coefficients for a rate and high-pass frequency.
    void coefficients(uint32_t rate, int highPass, int &c1, int &c2);
}
