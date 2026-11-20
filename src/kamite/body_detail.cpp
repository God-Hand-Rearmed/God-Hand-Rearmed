// The women's detailed bust / seat at run time (see body_detail.h).
#include "kamite/body_detail.h"
#include "kamite/body_detail_build.h"
#include "kamite/bone_tweaks.h"
#include "kamite/disc_files.h"
#include "kamite/file_layer.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "kamite/mods.h"
#include "kamite/olivia_model.h"
#include "runtime/ps2_cd_image.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace kamite::body_detail
{
    namespace
    {
        struct Model
        {
            const char *file;
            uint32_t offset, size; // a dat entry: its offset in the file, the room up to the next entry
            uint64_t hash;
            bool bust, seat;
            bool grow = false; // no room in her own block: the model comes out bigger and her file is served whole
        };
        constexpr Model kModels[] = {
#include "kamite/body_detail_models.inc"
        };
        // Models kept as they are, with Cash's growth on their own seats (growOnly): file, block, size, FNV-1a 64.
        struct GrowModel
        {
            const char *file;
            uint32_t offset, size;
            uint64_t hash;
            GrowRules rules; // clothing layers (skirt, the body under it, a belt plate)
            int jiggle = 0;  // the jiggle bones (kJiggle*) on the nodes below, re-skinned in place (body_detail_build reskin)
            const char *reskin[8] = {};
        };
        constexpr GrowRules kOliviaSkirt{"skirt00", "dress", "z_blt00"}, kOliviaSkirt2{"skirt00", "dress2", "z_blt00"};
        constexpr GrowModel kGrowModels[] = {
            {"em/em52.dat", 0x31580u, 0x47640u, 0x97A8C864A29AC1F0ULL, {}, kJiggleSeat | kJiggleBust, {"body_u1", "BY_00"}}, // Shannon
            {"em/ema0.dat", 0x28060u, 0x47700u, 0xF37EDAD64FF2AE63ULL, kOliviaSkirt, kJiggleBust, {"dress", nullptr}}, // Olivia (skirt: no seat)
            {"el/el0a.dat", 0x577A0u, 0x21640u, 0x7E94290CBD92FD0FULL, kOliviaSkirt2}, // Olivia, cutscenes (dress)
            {"el/elf5.dat", 0x273E0u, 0x29D80u, 0xDBA0ADACA05FA7D8ULL, kOliviaSkirt2}, // Olivia, cutscenes (dress + extras)
            // Olivia's bunny suit (cutscenes, and playable Olivia's costume): the frills round the leg openings (z_a..z_f)
            // re-skinned with the body so they ride the seat with it (K, Nov 19).
            {"el/elf4.dat", 0x25A20u, 0x1E700u, 0x16E4FA36326B5C40ULL, {}, kJiggleSeat | kJiggleBust,
             {"body", "z_a", "z_b", "z_c", "z_d", "z_e", "z_f"}},
            {"el/el15.dat", 0x24EE0u, 0x1C840u, 0x2C2C47653973ABDAULL, {}},           // Shannon, cutscenes
            {"el/el1a.dat", 0xF460u, 0x46200u, 0x045B558E999D846BULL, {}},            // Shannon, cutscenes (whole)
            // not el09 (the bathtub Olivia: no hips)
        };

        constexpr uint32_t kGetMeshPtr = 0x0014B6F8u;    // cModel_getMeshPtr(model, index)
        constexpr uint32_t kTransCallSite = 0x0014A4C0u; // the return address of its first call in cModel_Trans
        constexpr uint32_t kMeshHead = 0x25Cu, kNodeNext = 0x404u, kNodeInfo = 0x414u, kInfoName = 0x8u, kNodeMdb = 0x388u;
        constexpr float kOn = 1.0005f; // Money / Cash above 100% (they are stored as scales)
        constexpr float kStrapLift = 0.004f; // Olivia's thigh straps: 4 mm out (they sat 0.4-1.7 mm off her thighs)

        // A rebuilt body, shared by every model whose block had the same bytes.
        struct Body
        {
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> id; // mdb bytes no state changes: what identifies it
            uint32_t size = 0;                                       // the mdb's size
            std::vector<Change> changes;
            int sentinel[3] = {-1, -1, -1};
            bool bust = false, seat = false;
            int jiggle = 0; // kJiggle*: the jiggle bones its records use (bone_tweaks places them every draw)
            JiggleBind bind;
        };
        // A rebuilt block for its file: where it goes, the room it had, its bytes (bigger than the room: grown).
        struct Placed
        {
            uint32_t offset = 0, room = 0;
            std::vector<uint8_t> scr;
        };
        std::vector<Body> g_bodies; // set at boot, read-only afterwards
        std::map<std::string, std::vector<uint8_t>> g_grownBlocks; // kGrowModels' files: the model block as the game reads it
        std::vector<std::string> g_nodeNames{"body"}; // the nodes check() looks at (the grown models' too)

        uint64_t fnv(const std::vector<uint8_t> &b)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const uint8_t x : b)
                h = (h ^ x) * 0x100000001B3ull;
            return h;
        }

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & (PS2_RAM_MASK & ~3u)), 4);
            return v;
        }
        bool inRam(uint32_t a, uint32_t size)
        {
            a &= PS2_RAM_MASK;
            return a >= 0x100000u && size <= PS2_RDRAM_SIZE - a;
        }

        // The bytes of a body's mdb that neither state touches: the header's first 12 (with the buffer and bone
        // counts), the buffer table, buffer 0's header and its first UVs.
        void identify(Body &body, const uint8_t *m, uint32_t size)
        {
            uint16_t nv;
            std::memcpy(&nv, m + 10, 2);
            uint32_t vb0, uv;
            uint16_t count;
            std::memcpy(&vb0, m + 32, 4);
            std::memcpy(&uv, m + vb0 + 8, 4);
            std::memcpy(&count, m + vb0 + 20, 2);
            auto take = [&](uint32_t at, uint32_t n) { body.id.emplace_back(at, std::vector<uint8_t>(m + at, m + at + n)); };
            take(0, 12);
            take(32, 4u * nv);
            take(vb0, 32);
            take(vb0 + uv, std::min<uint32_t>(64u, 4u * count));
            body.size = size;
        }

        // The rebuilt body an mdb is (its bytes no state changes), or nullptr.
        const Body *bodyOf(const uint8_t *rdram, uint32_t mdb)
        {
            for (const Body &b : g_bodies)
            {
                if (!inRam(mdb, b.size))
                    continue;
                const uint8_t *p = rdram + (mdb & PS2_RAM_MASK);
                bool same = true;
                for (const auto &[at, bytes] : b.id)
                    if (std::memcmp(p + at, bytes.data(), bytes.size()) != 0)
                    {
                        same = false;
                        break;
                    }
                if (same)
                    return &b;
            }
            return nullptr;
        }

        // The model's "body" node's mdb, or 0.
        uint32_t bodyMdb(const uint8_t *rdram, uint32_t model)
        {
            if (!inRam(model, 0x2E4u))
                return 0u;
            uint32_t node = rd32(rdram, model + kMeshHead);
            for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
            {
                const uint32_t info = rd32(rdram, node + kNodeInfo);
                if (inRam(info, 16u) && std::memcmp(rdram + ((info + kInfoName) & PS2_RAM_MASK), "body\0", 5) == 0)
                    return rd32(rdram, node + kNodeMdb);
                node = rd32(rdram, node + kNodeNext);
            }
            return 0u;
        }

        // A body node drawn: its bust / seat to the state Money / Cash ask for.
        void check(uint8_t *rdram, uint32_t mdb)
        {
            const Body *b = bodyOf(rdram, mdb);
            if (!b)
                return;
            uint8_t *p = rdram + (mdb & PS2_RAM_MASK);
            const float cash = bone_tweaks::cash(); // (the reads are relaxed atomics: cheap per node)
            const bool want[3] = {false, b->bust && bone_tweaks::money() > kOn, b->seat && cash > kOn};
            const float grow[3] = {0.0f, 0.0f, want[2] ? cashGrowthScale(cash) : 0.0f};
            for (int region = 1; region <= 2; ++region)
            {
                const int s = b->sentinel[region];
                if (s < 0)
                    continue;
                float at[3];
                wantedPosition(b->changes[static_cast<size_t>(s)], want[region], grow[region], at);
                if (std::memcmp(p + b->changes[static_cast<size_t>(s)].pos, at, 12) != 0)
                    apply(p, b->changes, region, want[region], grow[region]);
            }
        }

        void onGetMeshPtr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 31) != kTransCallSite)
                return;
            const uint32_t model = GPR_U32(ctx, 4);
            if (!inRam(model, 0x2E4u))
                return;
            uint32_t node = rd32(rdram, model + kMeshHead);
            for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
            {
                const uint32_t info = rd32(rdram, node + kNodeInfo);
                if (inRam(info, 16u))
                {
                    const char *name = reinterpret_cast<const char *>(rdram + ((info + kInfoName) & PS2_RAM_MASK));
                    for (const std::string &n : g_nodeNames)
                        if (std::strncmp(name, n.c_str(), 8) == 0)
                        {
                            check(rdram, rd32(rdram, node + kNodeMdb));
                            break;
                        }
                }
                node = rd32(rdram, node + kNodeNext);
            }
        }

        bool readDonor(const char *path, const char *node, Mesh &out)
        {
            const auto file = disc_files::find(path);
            std::vector<uint8_t> data;
            return file && disc_files::read(*file, data) && loadDonor(data.data(), data.size(), node, out);
        }
    }

    namespace
    {
        uint32_t le32(const std::vector<uint8_t> &b, size_t o)
        {
            uint32_t v = 0;
            std::memcpy(&v, b.data() + o, 4);
            return v;
        }

        // A file's rebuilt blocks reach the game: all the same size as their room -> byte patches over the disc
        // (PS2DiscOverlay); any bigger (the no-room women) -> the whole file laid out again and served by the file layer,
        // with its other rebuilt blocks written in too (the overlay never reaches a served file). A dat is (u32 count,
        // u32 offset[count], char type[4][count], entries); a bigger entry pushes every later one back by whole 2 KB
        // (their alignment kept), and their offsets with them.
        void place(const std::map<std::string, std::vector<Placed>> &placed)
        {
            std::vector<PS2DiscOverlay::Patch> patches;
            for (const auto &[path, blocks] : placed)
            {
                const auto file = disc_files::find(path);
                if (!file)
                    continue;
                // A mod's own file shows as it is (mod-support decision 3; rebuilding from it is step 3).
                if (const std::string *mod = mods::replacing(path))
                {
                    KM_INFO(Boot, "body detail: " << path << " comes from the mod " << *mod << "; left as the mod has it");
                    continue;
                }
                const bool whole = std::any_of(blocks.begin(), blocks.end(), [](const Placed &p) { return p.scr.size() > p.room; });
                if (!whole)
                {
                    for (const Placed &p : blocks)
                        patches.emplace_back(file->imageOffset + p.offset, p.scr);
                    continue;
                }
                std::vector<uint8_t> bytes;
                if (!disc_files::read(*file, bytes) || bytes.size() < 4u)
                    continue;
                const uint32_t count = le32(bytes, 0);
                if (count == 0u || count > 0x10000u || bytes.size() < 4u + 8u * count)
                {
                    KM_WARN(Boot, "body detail: " << path << " is not a dat; its bigger models are left out");
                    continue;
                }
                std::vector<Placed> order = blocks;
                std::sort(order.begin(), order.end(), [](const Placed &a, const Placed &b) { return a.offset > b.offset; });
                int bigger = 0;
                uint32_t added = 0;
                for (const Placed &p : order)
                {
                    if (p.scr.size() <= p.room)
                    {
                        std::memcpy(bytes.data() + p.offset, p.scr.data(), p.scr.size());
                        continue;
                    }
                    bool entry = false;
                    for (uint32_t i = 0; i < count; ++i)
                        entry = entry || le32(bytes, 4u + 4u * i) == p.offset;
                    if (!entry || p.offset + p.room > bytes.size())
                    {
                        KM_WARN(Boot, "body detail: " << path << " @0x" << std::hex << p.offset << std::dec << " is not an entry; left out");
                        continue;
                    }
                    const uint32_t grow = (static_cast<uint32_t>(p.scr.size()) - p.room + 0x7FFu) & ~0x7FFu;
                    bytes.insert(bytes.begin() + p.offset + p.room, grow, 0u);
                    std::memcpy(bytes.data() + p.offset, p.scr.data(), p.scr.size());
                    for (uint32_t i = 0; i < count; ++i)
                        if (const uint32_t o = le32(bytes, 4u + 4u * i); o > p.offset)
                        {
                            const uint32_t moved = o + grow;
                            std::memcpy(bytes.data() + 4u + 4u * i, &moved, 4);
                        }
                    ++bigger;
                    added += grow;
                }
                const size_t size = bytes.size();
                if (bigger != 0 && file_layer::replace(path, std::move(bytes)))
                    KM_INFO(Boot, "body detail: " << path << " served whole (" << bigger << " model(s) bigger, +" << added / 1024u << " KB, "
                                                  << size << " bytes)");
            }
            if (!patches.empty())
                PS2DiscOverlay::add(std::move(patches));
        }
    }

    namespace
    {
        // Gameplay Olivia from her cutscene parts (olivia_model.h; claude/limit-break-handoff.md decision 10), only with
        // KAMITE_OLIVIA_MODEL=1 (K, Nov 19: the cutscene head and the posed hands look worse in play than her own; off
        // until decided). Built only on the disc whose em/ema0.dat model kGrowModels lists.
        bool buildOlivia(const GrowModel &listed, olivia_model::Built &out)
        {
            if (const char *v = std::getenv("KAMITE_OLIVIA_MODEL"); !v || *v != '1')
                return false;
            std::vector<uint8_t> files[4];
            const char *paths[4] = {"em/ema0.dat", "el/el0a.dat", "el/el0b.dat", "el/el0c.dat"};
            for (int i = 0; i < 4; ++i)
            {
                const auto file = disc_files::find(paths[i]);
                if (!file || !disc_files::read(*file, files[i]))
                    return false;
            }
            if (files[0].size() < size_t{listed.offset} + listed.size ||
                fnv(std::vector<uint8_t>(files[0].begin() + listed.offset, files[0].begin() + listed.offset + listed.size)) != listed.hash)
                return false;
            if (!olivia_model::build(files[0], files[1], files[2], files[3], out))
            {
                KM_WARN(Boot, "body detail: gameplay Olivia keeps her model (" << out.error << ")");
                return false;
            }
            return true;
        }

        // PROBE (strip with K's OK, claude/strip-list.md): the game's texture cache in video memory. cVram (0x4664A0)
        // is emptied every frame (func_00150AA8, from the main loop) and refilled from GS block 0x2800 (textures; CLUTs
        // from 0x1C20) by each of its three lists (DEF / ALPHA / COKPIT), so a frame's textures per list must fit in
        // 0x1800 blocks (1.5 MB) below the top of the GS's 4 MB. Logs each list's highest use so far (in KB, at every new
        // high 64 KB above the last one logged), to see what the bigger Olivia costs.
        constexpr uint32_t kVramReset = 0x00150AA8u, kVram = 0x004664A0u;
        constexpr uint32_t kTexEnd[3] = {0x3DC60u, 0x3DC78u, 0x3DC6Cu}; // DEF, ALPHA, COKPIT: the next texture block
        uint32_t g_vramPeak[3] = {}, g_vramLogged[3] = {};
        void onVramReset(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 4) != kVram)
                return;
            static const char *const names[3] = {"DEF", "ALPHA", "COKPIT"};
            for (int i = 0; i < 3; ++i)
            {
                const uint32_t end = rd32(rdram, kVram + kTexEnd[i]);
                const uint32_t used = end > 0x2800u && end < 0x8000u ? end - 0x2800u : 0u; // 256-byte blocks
                if (used <= g_vramPeak[i])
                    continue;
                g_vramPeak[i] = used;
                if (used < g_vramLogged[i] + 256u)
                    continue;
                g_vramLogged[i] = used;
                KM_INFO(Game, "vram probe: " << names[i] << " textures peak " << used / 4u << " KB of 1536" << (used > 0x1800u ? " (OVER)" : ""));
            }
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kGetMeshPtr))
            return;
        if (!disc_files::open())
        {
            KM_WARN(Boot, "body detail: the disc image has no file archive; the women keep their models");
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        Mesh bunny, shannon;
        if (!readDonor("el/elf4.dat", "body", bunny) || !readDonor("el/el15.dat", "body_u1", shannon))
        {
            KM_WARN(Boot, "body detail: the donor models (el/elf4.dat, el/el15.dat) could not be read");
            return;
        }
        std::map<uint64_t, std::pair<int, std::vector<uint8_t>>> built; // block hash -> body (-1: failed), its scr
        std::map<std::string, std::vector<Placed>> placed;              // file -> its rebuilt blocks
        int models = 0, other = 0;
        for (const Model &m : kModels)
        {
            const auto file = disc_files::find(m.file);
            std::vector<uint8_t> block;
            if (!file || !disc_files::readRange(*file, m.offset, m.size, block) || block.size() != m.size || fnv(block) != m.hash)
            {
                ++other;
                continue;
            }
            auto it = built.find(m.hash);
            if (it == built.end())
            {
                Built b;
                if (!build(block.data(), block.size(), bunny, shannon, m.bust, m.seat, b, m.grow))
                {
                    KM_WARN(Boot, "body detail: " << m.file << " @0x" << std::hex << m.offset << std::dec << ": " << b.error);
                    built.emplace(m.hash, std::make_pair(-1, std::vector<uint8_t>()));
                    continue;
                }
                if (!b.error.empty())
                    KM_INFO(Boot, "body detail: " << m.file << " @0x" << std::hex << m.offset << std::dec << ": " << b.error);
                Body body;
                identify(body, b.scr.data() + b.bodyMdb, b.bodyMdbSize);
                body.changes = std::move(b.changes);
                std::memcpy(body.sentinel, b.sentinel, sizeof body.sentinel);
                body.bust = m.bust && b.sentinel[1] >= 0;
                body.seat = m.seat && b.sentinel[2] >= 0; // the detailed seat and Cash's growth on it
                body.jiggle = b.jiggle;
                body.bind = b.bind;
                g_bodies.push_back(std::move(body));
                it = built.emplace(m.hash, std::make_pair(static_cast<int>(g_bodies.size()) - 1, std::move(b.scr))).first;
            }
            if (it->second.first < 0)
                continue;
            placed[m.file].push_back({m.offset, m.size, it->second.second});
            ++models;
        }
        // Shannon and Olivia keep their models; Cash grows their own seats (and what hangs over them). Gameplay Olivia's
        // is first rebuilt from her cutscene parts (served whole below), and grown as that.
        int grown = 0;
        olivia_model::Built olivia;
        bool oliviaBuilt = false;
        for (const GrowModel &g : kGrowModels)
        {
            const bool isOlivia = std::strcmp(g.file, "em/ema0.dat") == 0 && (oliviaBuilt = buildOlivia(g, olivia));
            const auto file = disc_files::find(g.file);
            std::vector<uint8_t> block;
            std::vector<GrownNode> nodes;
            if (isOlivia)
                block.assign(olivia.dat.begin() + olivia.md, olivia.dat.begin() + olivia.md + olivia.mdSize);
            if (!isOlivia && (!file || !disc_files::readRange(*file, g.offset, g.size, block) || block.size() != g.size || fnv(block) != g.hash))
            {
                ++other;
                continue;
            }
            // Olivia's thigh straps off her thighs (they z-fought with them in play and in cutscenes; K, Nov 19).
            const bool straps = (std::strcmp(g.file, "em/ema0.dat") == 0 || std::strcmp(g.file, "el/el0a.dat") == 0 ||
                                 std::strcmp(g.file, "el/elf5.dat") == 0) &&
                                liftStraps(block.data(), block.size(), "boots", kStrapLift) > 0;
            if (!growOnly(block.data(), block.size(), g.rules, nodes))
            {
                ++other;
                continue;
            }
            // The jiggle bones on her own nodes: only when every node to re-skin is one the runtime knows (a grown
            // node), since a re-skinned node drawn without its bones placed would fall apart.
            int named = 0;
            while (named < 8 && g.reskin[named])
                ++named;
            const auto isReskinned = [&g](const char *name) {
                for (const char *r : g.reskin)
                    if (r && std::strncmp(name, r, 8) == 0)
                        return true;
                return false;
            };
            int known = 0;
            for (const GrownNode &n : nodes)
                known += isReskinned(n.name) ? 1 : 0;
            int jiggle = 0;
            if (g.jiggle && known == named)
                jiggle = reskin(block.data(), block.size(), g.reskin, named, g.jiggle);
            if (g.jiggle && jiggle == 0)
                KM_WARN(Boot, "body detail: " << g.file << ": no jiggle bones (" << known << " of " << named << " nodes known)");
            if ((jiggle != 0 || straps) && !isOlivia)
                placed[g.file].push_back({g.offset, g.size, block});
            for (GrownNode &n : nodes)
            {
                Body body;
                identify(body, block.data() + n.mdb, n.mdbSize);
                body.changes = std::move(n.changes);
                body.sentinel[2] = n.sentinel;
                body.seat = true;
                if (jiggle && isReskinned(n.name) && jiggleSpare(block.data(), block.size(), n.mdb, &body.bind))
                    body.jiggle = jiggle;
                g_bodies.push_back(std::move(body));
                if (std::find(g_nodeNames.begin(), g_nodeNames.end(), std::string(n.name)) == g_nodeNames.end())
                    g_nodeNames.emplace_back(n.name);
            }
            if (isOlivia)
                std::copy(block.begin(), block.end(), olivia.dat.begin() + olivia.md);
            g_grownBlocks[g.file] = block;
            ++grown;
        }
        if (oliviaBuilt && mods::replacing("em/ema0.dat"))
        {
            KM_INFO(Boot, "body detail: em/ema0.dat comes from the mod " << *mods::replacing("em/ema0.dat") << "; gameplay Olivia left as the mod has her");
            oliviaBuilt = false;
        }
        if (oliviaBuilt)
        {
            const size_t size = olivia.dat.size();
            if (file_layer::replace("em/ema0.dat", std::move(olivia.dat)))
            {
                KM_INFO(Boot, "body detail: gameplay Olivia from her cutscene models (" << olivia.nodes << " nodes, " << olivia.vertices
                                                                                      << " vertices; textures " << olivia.textureBytes / 1024u << " KB, the game's "
                                                                                      << olivia.gameTextureBytes / 1024u << " KB; " << size << " bytes)");
                FunctionHooks::instance().observeEntry(kVramReset, "vram_probe", &onVramReset);
            }
        }
        if (placed.empty() && grown == 0)
        {
            KM_WARN(Boot, "body detail: no model matched (" << other << " listed blocks differ on this disc)");
            return;
        }
        place(placed);
        FunctionHooks::instance().observeEntry(kGetMeshPtr, "body_detail", &onGetMeshPtr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        KM_INFO(Boot, "body detail: " << models << " women's models rebuilt, " << grown << " grown as they are (" << g_bodies.size() << " bodies, " << ms << " ms"
                                     << (other ? ", " + std::to_string(other) + " listed blocks differ" : std::string()) << ")");
    }

    Jiggle jiggleOf(const uint8_t *rdram, uint32_t model)
    {
        Jiggle out;
        if (!inRam(model, 0x2E4u))
            return out;
        uint32_t node = rd32(rdram, model + kMeshHead);
        for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
        {
            const uint32_t info = rd32(rdram, node + kNodeInfo);
            if (inRam(info, 16u))
            {
                const char *name = reinterpret_cast<const char *>(rdram + ((info + kInfoName) & PS2_RAM_MASK));
                for (const std::string &n : g_nodeNames)
                    if (std::strncmp(name, n.c_str(), 8) == 0)
                    {
                        if (const Body *b = bodyOf(rdram, rd32(rdram, node + kNodeMdb)); b && b->jiggle)
                        {
                            out.flags |= b->jiggle;
                            std::memcpy(out.pelvis, b->bind.pelvis, sizeof out.pelvis);
                            std::memcpy(out.bust, b->bind.bust, sizeof out.bust);
                        }
                        break;
                    }
            }
            node = rd32(rdram, node + kNodeNext);
        }
        return out;
    }

    std::vector<uint8_t> grownBlock(const char *file)
    {
        const auto it = g_grownBlocks.find(file);
        return it == g_grownBlocks.end() ? std::vector<uint8_t>() : it->second;
    }

    bool bustDetailed(const uint8_t *rdram, uint32_t model)
    {
        const uint32_t mdb = bodyMdb(rdram, model & PS2_RAM_MASK);
        const Body *b = mdb ? bodyOf(rdram, mdb) : nullptr;
        if (!b || !b->bust || b->sentinel[1] < 0)
            return false;
        const Change &c = b->changes[static_cast<size_t>(b->sentinel[1])];
        return std::memcmp(rdram + (mdb & PS2_RAM_MASK) + c.pos, c.detail, 12) == 0;
    }
}
