// The C++ character build (src/kamite/character_build.cpp) on one game file, for check_character_build.py, which
// compares it with the Python build byte for byte. Pure file layout: compiles on its own with any C++20 compiler.
//
//   check_character_build <character folder> <game file path, e.g. pl/pl00.dat> <the game's file> <out file>
//                         [<game texture> <layer .rgba> <width> <height>]...
//
// Writes the merged file to <out file> and, for each painted texture, <out file>.<texture>.rgb (the full-resolution
// image, RGB, from the game's texture scaled up) and <out file>.<texture>.tim3 (the new 16-colour texture).
#include "kamite/character_build.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <string>

using namespace kamite;

static std::vector<uint8_t> load(const std::string &path)
{
    std::ifstream in(path, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
}
static bool save(const std::string &path, const std::vector<uint8_t> &bytes)
{
    std::ofstream out(path, std::ios::binary);
    out.write(reinterpret_cast<const char *>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    return static_cast<bool>(out);
}

int main(int argc, char **argv)
{
    if (argc < 5 || (argc - 5) % 4)
        return std::fprintf(stderr, "usage: see the comment at the top\n"), 2;
    const std::string folder = argv[1], gamePath = argv[2], out = argv[4];
    const std::vector<uint8_t> json = load(folder + "/character.json");
    std::vector<character_build::FileSpec> specs;
    std::string error;
    if (!character_build::readSpec(std::string_view(reinterpret_cast<const char *>(json.data()), json.size()), specs, error))
        return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    const character_build::FileSpec *spec = nullptr;
    for (const auto &s : specs)
        if (s.path == gamePath)
            spec = &s;
    if (!spec)
        return std::fprintf(stderr, "%s is not in character.json\n", gamePath.c_str()), 1;
    std::map<std::string, character_build::Image> layers;
    for (int i = 5; i + 3 < argc; i += 4)
    {
        character_build::Image &layer = layers[argv[i]];
        layer.width = std::atoi(argv[i + 2]);
        layer.height = std::atoi(argv[i + 3]);
        layer.px = load(argv[i + 1]);
    }
    std::vector<uint8_t> merged;
    std::vector<character_build::Overlay> overlays;
    const auto start = std::chrono::steady_clock::now();
    if (!character_build::mergeFile(*spec, load(argv[3]), spec->empty ? std::vector<uint8_t>() : load(folder + "/" + spec->nodes),
                                    spec->empty ? std::vector<uint8_t>() : load(folder + "/" + spec->textures), layers, merged, overlays, error))
        return std::fprintf(stderr, "%s\n", error.c_str()), 1;
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    save(out, merged);
    for (const auto &o : overlays)
    {
        character_build::Image hd;
        if (!character_build::hdImage(o.gameTim3, nullptr, *o.layer, hd))
            return std::fprintf(stderr, "%s: no full-resolution image\n", o.name.c_str()), 1;
        save(out + "." + o.name + ".rgb", hd.px);
        save(out + "." + o.name + ".tim3", o.newTim3);
    }
    std::printf("%s: %zu bytes, %.0f ms\n", gamePath.c_str(), merged.size(), ms);
    return 0;
}
