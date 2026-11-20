#pragma once

// A built-in playable character's player files, made from the player's own disc and the character's assets
// (resources/characters/<id>/, copied from assets/characters/<id>/): the C++ port of tools/models/build_characters.py
// merge_file / overlay_hd, gamemodel.py merge_model (trim_mdb, restrip_mdb, empty_mdb), strip.py and paint.py. Pure
// file layout, no runtime (builtin_characters.cpp reads the disc, calls these at boot and serves the results).
//
// The output equals the Python's byte for byte (checked offline against a disc: the C++ compiles standalone with g++):
// - Doubles where the Python computes with floats, float32 where it uses numpy float32 (the overlay), no FMA
//   contraction; Python's round() is round-half-even (std::nearbyint); sum() of floats is Neumaier's (Python 3.12+).
// - strip.py's 40 seeded tries use Python's random.Random (MT19937, init_by_array, _randbelow) and its first try takes
//   the first least-connected triangle in a CPython set's iteration order: both are emulated here.
// - The overlay's layer is scaled with Pillow's Lanczos on float images (Resample.c), the 16 colours come from
//   paint.quantize16's integer median cut (written the same in both).
// The full-resolution images (hdImage) are not compared byte for byte: their names come from the 16-colour textures.

#include <cstdint>
#include <map>
#include <string>
#include <string_view>
#include <vector>

namespace kamite::character_build
{
    using Bytes = std::vector<uint8_t>;

    // An image, row-major, `channels` bytes a pixel (4: RGBA, 3: RGB).
    struct Image
    {
        int width = 0, height = 0, channels = 4;
        std::vector<uint8_t> px;
    };

    // character.json, as build_characters.py reads it.
    struct Trim
    {
        double axis[3] = {}, minimum = 0.0;
        bool tint = false;
        double tintStart = 0.0, tintLength = 0.0, tintRgb[3] = {};
        bool inset = false;
        double insetStart = 0.0, insetFactor = 1.0, insetCentre[2] = {};
    };
    struct FileSpec
    {
        std::string path;                  // the game file ("pl/pl00.dat")
        bool empty = false;                // every model node drawn as nothing
        std::string nodes, textures;       // the character's .scr / .tm3 (relative to its folder)
        std::vector<std::string> keep;     // the game's nodes kept (default gamemodel.KEEP: the gauntlet arm, the hands)
        bool appendTextures = true;
        std::vector<std::pair<std::string, Trim>> trim;
        std::vector<std::pair<std::string, std::string>> overlay; // game texture -> RGBA PNG (relative)
    };
    bool readSpec(std::string_view json, std::vector<FileSpec> &out, std::string &error);

    // One painted texture of a merged file: the game's TIM3, the new one (in the merged file) and its layer.
    struct Overlay
    {
        std::string name;
        Bytes gameTim3, newTim3;
        const Image *layer = nullptr;
    };

    // build_characters.merge_file: the game's file merged with the character's nodes (.scr) and textures (.tm3) as the
    // spec says; layers: the overlay PNGs decoded (RGBA), by game texture name. False (error set) when a file is not
    // what the merge expects.
    bool mergeFile(const FileSpec &spec, const Bytes &game, const Bytes &nodes, const Bytes &textures,
                   const std::map<std::string, Image> &layers, Bytes &out, std::vector<Overlay> &overlays, std::string &error);

    // build_characters.overlay_hd: the full-resolution image of an overlaid texture (RGB): the pack's image of the
    // game's texture (packBase, RGBA; nullptr: none) or the game's texture scaled up 4 times, with the layer laid on.
    bool hdImage(const Bytes &gameTim3, const Image *packBase, const Image &layer, Image &out);

    // An uncompressed 32-bit .dds (as texture packs for the game ship): RGBA. False for anything else.
    bool readDds(const Bytes &dds, Image &out);

    // The game file's texture of that name (the last of that name in its TM3s, as build_characters.py finds it).
    bool findTexture(const Bytes &dat, const std::string &name, Bytes &tim3);

    // A 4-bit TIM3's payload and its 16-entry CLUT, as hd_textures::name4 takes them (128x128 only). False otherwise.
    bool tim3Payload128(const Bytes &tim3, const uint8_t *&payload, const uint8_t *&clut16);
}
