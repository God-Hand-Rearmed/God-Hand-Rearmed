// The built-in playable characters (see builtin_characters.h).
#include "kamite/builtin_characters.h"
#include "kamite/character_build.h"
#include "kamite/character_mods.h"
#include "kamite/disc_files.h"
#include "kamite/file_layer.h"
#include "kamite/hd_textures.h"
#include "kamite/log.h"

// xxHash, header only: PCSX2's copy (hd_textures.cpp includes it the same way).
#define XXH_INLINE_ALL
#include "../../thirdparty/pcsx2/3rdparty/include/xxhash.h"

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <iterator>
#include <map>
#include <system_error>

namespace fs = std::filesystem;

namespace kamite::builtin_characters
{
    namespace
    {
        using Bytes = std::vector<uint8_t>;

        // Part of every cache key: raise it when character_build.cpp's output changes, so every player's cache is made
        // again from the new code.
        constexpr const char *kBuilder = "character_build 1";

        // Gene's body files (func_0030D8E0: 0x100 + his costume) and coat files (the table at 0x450590), as
        // olivia_player.cpp serves hers.
        constexpr const char *kBodies[] = {"pl/pl00.dat", "pl/pl02.dat", "pl/pl05.dat", "pl/pl07.dat", "pl/pl09.dat",
                                           "pl/pl0e.dat", "pl/pl0f.dat", "pl/pl10.dat", "pl/pl11.dat"};
        constexpr const char *kCoats[] = {"pl/pl01.dat", "pl/pl06.dat", "pl/pl08.dat", "pl/pl0a.dat", "pl/pl12.dat"};

        std::string utf8(const fs::path &p)
        {
            const std::u8string s = p.lexically_normal().make_preferred().u8string();
            return std::string(s.begin(), s.end());
        }
        std::string lower(std::string s)
        {
            for (char &c : s)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return s;
        }
        fs::path under(const fs::path &folder, const std::string &relative)
        {
            return folder / fs::path(reinterpret_cast<const char8_t *>(relative.c_str()));
        }
        bool readFile(const fs::path &path, Bytes &out)
        {
            std::ifstream in(path, std::ios::binary);
            if (!in)
                return false;
            out.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
            return true;
        }
        // Written beside, then moved over the old file: a crash mid-write never leaves half a file under the real name.
        bool writeFile(const fs::path &path, const void *data, size_t size)
        {
            std::error_code ec;
            fs::create_directories(path.parent_path(), ec);
            fs::path temp = path;
            temp += ".part";
            {
                std::ofstream out(temp, std::ios::binary | std::ios::trunc);
                out.write(static_cast<const char *>(data), static_cast<std::streamsize>(size));
                if (!out)
                    return false;
            }
            fs::rename(temp, path, ec);
            return !ec;
        }
        bool readDisc(const std::string &path, Bytes &out)
        {
            const auto file = disc_files::find(path);
            return file && disc_files::read(*file, out) && !out.empty();
        }
        void addTextureFolder(const fs::path &folder)
        {
            const char *extra = std::getenv("PS2X_PCSX2_TEXTURE_EXTRA");
            std::string folders = extra ? extra : "";
            folders += (folders.empty() ? "" : "|") + utf8(folder);
#if defined(_WIN32)
            _putenv_s("PS2X_PCSX2_TEXTURE_EXTRA", folders.c_str());
#else
            setenv("PS2X_PCSX2_TEXTURE_EXTRA", folders.c_str(), 1);
#endif
        }
        std::string hex(uint64_t v)
        {
            char text[17];
            std::snprintf(text, sizeof(text), "%016llx", static_cast<unsigned long long>(v));
            return text;
        }
        // The replacement name of a 128x128 4-bit TIM3 (empty for any other).
        std::string hdName(const Bytes &tim3)
        {
            const uint8_t *payload = nullptr, *clut = nullptr;
            return character_build::tim3Payload128(tim3, payload, clut) ? hd_textures::name4(payload, clut) : std::string();
        }
        // The pack's image of a game texture as the base of its painted version: an uncompressed .dds (as the Python
        // tools read) or a .png. False: the pack has none (or not one these read).
        bool packBase(const std::string &gameName, character_build::Image &out)
        {
            const fs::path file = gameName.empty() ? fs::path() : hd_textures::packFile(gameName);
            if (file.empty())
                return false;
            if (lower(utf8(file.extension())) == ".dds")
            {
                Bytes dds;
                return readFile(file, dds) && character_build::readDds(dds, out);
            }
            hd_textures::Image png;
            if (!hd_textures::loadPng(file, png))
                return false;
            out.width = png.width, out.height = png.height, out.channels = 4;
            out.px = std::move(png.rgba);
            return true;
        }

        struct Character
        {
            std::string id;
            fs::path folder, cache;
            std::vector<character_build::FileSpec> specs;
            std::vector<Bytes> games, nodes, textures;
            uint64_t key = 0;
        };

        // The character's inputs read and hashed (the cache key): the builder, character.json, every disc file it
        // changes, its own files, the pack images its painted textures start from (by name, size and date).
        bool gather(Character &c, std::string &error)
        {
            // The disc's path table and archive (nothing else has opened them this early: disc_files::find is empty
            // until open() has run; K, Nov 20: "pl/pl00.dat could not be read from the disc", Gene instead).
            if (!disc_files::open())
                return error = "the disc image has no file archive / path table", false;
            Bytes json;
            if (!readFile(c.folder / "character.json", json))
                return error = "no character.json", false;
            if (!character_build::readSpec(std::string_view(reinterpret_cast<const char *>(json.data()), json.size()), c.specs, error))
                return false;
            XXH3_state_t state;
            XXH3_64bits_reset(&state);
            auto hash = [&](const void *data, size_t size) {
                const uint64_t n = size;
                XXH3_64bits_update(&state, &n, sizeof(n));
                XXH3_64bits_update(&state, data, size);
            };
            auto hashText = [&](const std::string &text) { hash(text.data(), text.size()); };
            hashText(kBuilder);
            hash(json.data(), json.size());
            c.games.resize(c.specs.size());
            c.nodes.resize(c.specs.size());
            c.textures.resize(c.specs.size());
            for (size_t i = 0; i < c.specs.size(); ++i)
            {
                const character_build::FileSpec &spec = c.specs[i];
                if (!readDisc(spec.path, c.games[i]))
                    return error = spec.path + " could not be read from the disc", false;
                hashText(spec.path);
                hash(c.games[i].data(), c.games[i].size());
                if (spec.empty)
                    continue;
                if (!readFile(under(c.folder, spec.nodes), c.nodes[i]) || !readFile(under(c.folder, spec.textures), c.textures[i]))
                    return error = "missing " + spec.nodes + " or " + spec.textures, false;
                hash(c.nodes[i].data(), c.nodes[i].size());
                hash(c.textures[i].data(), c.textures[i].size());
                for (const auto &[texture, png] : spec.overlay)
                {
                    Bytes layer, tim3;
                    if (!readFile(under(c.folder, png), layer))
                        return error = "missing " + png, false;
                    hashText(texture);
                    hash(layer.data(), layer.size());
                    std::string pack = "no pack image";
                    if (character_build::findTexture(c.games[i], texture, tim3))
                        if (const std::string name = hdName(tim3); !name.empty())
                            if (const fs::path file = hd_textures::packFile(name); !file.empty())
                            {
                                std::error_code ec;
                                pack = utf8(file) + " " + std::to_string(fs::file_size(file, ec)) + " " +
                                       std::to_string(static_cast<long long>(fs::last_write_time(file, ec).time_since_epoch().count()));
                            }
                    hashText(pack);
                }
            }
            c.key = XXH3_64bits_digest(&state);
            return true;
        }

        // build.key: the key, then the painted textures' image names. True when it matches and every file is there.
        bool cached(const Character &c)
        {
            std::ifstream in(c.cache / "build.key");
            std::string line;
            if (!std::getline(in, line) || line != hex(c.key))
                return false;
            std::error_code ec;
            for (const auto &spec : c.specs)
                if (!fs::is_regular_file(under(c.cache, spec.path), ec))
                    return false;
            while (std::getline(in, line))
                if (!line.empty() && !fs::is_regular_file(c.cache / "textures" / (line + ".png"), ec))
                    return false;
            return true;
        }

        // The character built into its cache. built: the files (by disc path) in memory too, for a cache that can't be
        // written.
        bool build(const Character &c, std::map<std::string, Bytes> &built, int &images, std::string &error)
        {
            std::error_code ec;
            fs::remove(c.cache / "build.key", ec);
            const fs::path textures = c.cache / "textures";
            for (const auto &entry : fs::directory_iterator(textures, ec))
                if (entry.is_regular_file(ec))
                    fs::remove(entry.path(), ec);
            std::string names;
            bool written = true;
            for (size_t i = 0; i < c.specs.size(); ++i)
            {
                const character_build::FileSpec &spec = c.specs[i];
                std::map<std::string, character_build::Image> layers;
                for (const auto &[texture, png] : spec.overlay)
                {
                    hd_textures::Image image;
                    if (!hd_textures::loadPng(under(c.folder, png), image))
                        return error = png + " is not a readable PNG", false;
                    character_build::Image &layer = layers[texture];
                    layer.width = image.width, layer.height = image.height, layer.channels = 4;
                    layer.px = std::move(image.rgba);
                }
                Bytes out;
                std::vector<character_build::Overlay> overlays;
                if (!character_build::mergeFile(spec, c.games[i], c.nodes[i], c.textures[i], layers, out, overlays, error))
                    return false;
                written = writeFile(under(c.cache, spec.path), out.data(), out.size()) && written;
                for (const character_build::Overlay &o : overlays)
                {
                    const std::string name = hdName(o.newTim3);
                    character_build::Image base, hd;
                    const bool fromPack = packBase(hdName(o.gameTim3), base);
                    if (name.empty() || !character_build::hdImage(o.gameTim3, fromPack ? &base : nullptr, *o.layer, hd))
                    {
                        KM_WARN(Boot, "character '" << c.id << "': no full-resolution image for " << o.name);
                        continue;
                    }
                    fs::create_directories(textures, ec);
                    if (!hd_textures::saveRgbPng(textures / (name + ".png"), hd.width, hd.height, hd.px))
                    {
                        written = false;
                        continue;
                    }
                    names += name + "\n";
                    ++images;
                    KM_INFO(Boot, "character '" << c.id << "': " << o.name << " painted on " << (fromPack ? "the pack's image" : "the game's, scaled up")
                                                << " (" << hd.width << "x" << hd.height << ")");
                }
                built[lower(spec.path)] = std::move(out);
            }
            const std::string key = hex(c.key) + "\n" + names;
            if (!written || !writeFile(c.cache / "build.key", key.data(), key.size()))
                KM_WARN(Boot, "character '" << c.id << "': could not write its cache in " << utf8(c.cache) << "; built again next start");
            return true;
        }

        // One built-in character: its cache made current, its textures registered, its files served when chosen.
        void prepare(const fs::path &exeDir, const fs::path &folder, bool chosen)
        {
            const auto start = std::chrono::steady_clock::now();
            Character c;
            c.folder = folder;
            const std::u8string idText = folder.filename().u8string();
            c.id = lower(std::string(idText.begin(), idText.end()));
            c.cache = exeDir / "cache" / "characters" / fs::path(folder.filename());
            std::string error;
            if (!gather(c, error))
            {
                if (chosen)
                    KM_WARN(Boot, "character '" << c.id << "': " << error << "; playing as Gene");
                else
                    KM_WARN(Boot, "character '" << c.id << "': " << error << "; not built");
                return;
            }
            std::map<std::string, Bytes> built;
            int images = 0;
            const bool fresh = !cached(c);
            if (fresh && !build(c, built, images, error))
            {
                KM_WARN(Boot, "character '" << c.id << "': " << error << (chosen ? "; playing as Gene" : "; not built"));
                return;
            }
            std::error_code ec;
            if (fs::is_directory(c.cache / "textures", ec) && !fs::is_empty(c.cache / "textures", ec))
                addTextureFolder(c.cache / "textures");
            const auto ms = [&] { return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count(); };
            if (!chosen)
            {
                if (fresh)
                    KM_INFO(Boot, "character '" << c.id << "': built into " << utf8(c.cache) << " (" << images << " images; " << ms() << " ms)");
                return;
            }
            // Its files for every costume's (sorted, so the served files keep their places from run to run: a save state
            // stays valid).
            std::map<std::string, std::string> serve; // disc path -> the character's file
            for (const auto &spec : c.specs)
            {
                const std::string path = lower(spec.path);
                serve[path] = path;
                const auto alias = [&](const char *const *list, size_t count) {
                    for (size_t i = 0; i < count; ++i)
                        if (disc_files::find(list[i]))
                            serve.emplace(list[i], path);
                };
                if (path == kBodies[0])
                    alias(kBodies, std::size(kBodies));
                if (path == kCoats[0])
                    alias(kCoats, std::size(kCoats));
            }
            int replaced = 0;
            for (const auto &[path, from] : serve)
            {
                const auto memory = built.find(from);
                const fs::path file = under(c.cache, from);
                const bool ok = fs::is_regular_file(file, ec) ? file_layer::replace(path, file)
                                : memory != built.end()       ? file_layer::replace(path, memory->second)
                                                              : false;
                replaced += ok;
            }
            if (replaced == 0)
            {
                KM_WARN(Boot, "character '" << c.id << "': none of Gene's files could be replaced; playing as Gene");
                return;
            }
            characters::useHair(folder);
            KM_INFO(Boot, "playing as " << characters::describe(folder).name << " (built in; " << replaced << " files, "
                                        << (fresh ? "built from the disc" : "from the cache") << ", " << ms() << " ms)");
        }
    }

    std::vector<fs::path> folders(const fs::path &exeDir)
    {
        std::vector<fs::path> out;
        std::error_code ec;
        for (const auto &entry : fs::directory_iterator(exeDir / "resources" / "characters", ec))
            if (entry.is_directory(ec) && fs::is_regular_file(entry.path() / "character.json", ec))
                out.push_back(entry.path());
        std::sort(out.begin(), out.end());
        return out;
    }

    bool isBuiltIn(const fs::path &exeDir, const std::string &id)
    {
        for (const fs::path &folder : folders(exeDir))
        {
            const std::u8string name = folder.filename().u8string();
            if (lower(std::string(name.begin(), name.end())) == lower(id))
                return true;
        }
        return false;
    }

    void install(const fs::path &exeDir)
    {
        const char *chosen = std::getenv("KAMITE_CHARACTER");
        const std::string id = chosen ? lower(chosen) : std::string();
        for (const fs::path &folder : folders(exeDir))
        {
            const std::u8string name = folder.filename().u8string();
            prepare(exeDir, folder, lower(std::string(name.begin(), name.end())) == id);
        }
    }
}
