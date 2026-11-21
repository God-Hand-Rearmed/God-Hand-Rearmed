#pragma once

#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

// A mod's audio for an archive entry (claude/mod-support.md "Step 2"): an .adx is used as it is; a .wav, .ogg or .mp3
// is converted to ADX (adx_codec.h) in the shape of the entry it replaces (its channels and rate) and cached under
// <exe>/cache/mod-audio, so only a new or changed file is converted at a start. Loops (K, Nov 20): the file's own loop
// points (an .ogg's LOOPSTART with LOOPLENGTH or LOOPEND tags, a .wav's smpl loop, an .adx's header), else the whole
// track loops where the entry it replaces looped. Decoding and resampling: raylib (LoadWaveFromMemory, WaveFormat).
namespace kamite::mod_audio
{
    struct Target
    {
        int channels = 2;
        uint32_t rate = 48000;
        bool loops = false; // the replaced entry loops
    };

    // .adx, .wav, .ogg or .mp3 (any case).
    bool isAudio(const std::filesystem::path &file);

    // The ADX to serve for source: source itself (an .adx) or its converted copy in cacheDir. Empty on failure, with
    // why in error; note says what was done (converted / from the cache, the loop), for the log.
    std::filesystem::path adxFor(const std::filesystem::path &source, const Target &target, const std::filesystem::path &cacheDir,
                                 std::string &note, std::string &error);

    // Loop points a file's bytes carry, in its own samples (end: where it jumps back). False: none.
    bool oggLoop(const std::vector<uint8_t> &ogg, uint64_t &start, uint64_t &end);
    bool wavLoop(const std::vector<uint8_t> &wav, uint64_t &start, uint64_t &end);
}
