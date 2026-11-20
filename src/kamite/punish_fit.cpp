// Buck Breaker's punish fits (K, Nov 8): a punish made for one body leaves Gene too far from (or into) another, e.g.
// Stinger, made for the fat men, leaves a gap on a thug whose bent-over head is also 15-20 cm lower. Each body is
// fitted offline to the original's touch points (tools/models/punish_fit, claude/punish-fit-offline.md): per punish
// and body, curves over the punish's motions of Gene's placement (forward, side, turn), Gene's height and the
// victim's height (the hips up or down; the game's leg IK bends the knees, the feet stay put) and waist bend.
//
// Placement. Each paired punish places Gene with func_001299F0(a0 = Gene, a1 = the enemy, a2 = &vec{x, 0, z, 1} on the
// caller's stack, a3 = mode, f12 = yaw): mode 0 turns Gene to face the enemy, the enemy's heading (+0x104) = Gene's +
// pi, then Gene's += f12; mode 1 only sets the enemy's heading = Gene's + pi. Then Gene = enemy.pos + RotY(enemy
// heading) . (x * enemy +0x110, 0, z * enemy +0x118). After that each of the two moves by its own motion's root
// travel, never by the other's position, so Gene moved in the middle of the punish stays moved. The wrapper
// (FunctionHooks::replace on 0x1299F0, only the four punishes' calls, by return address) puts the curve's first
// values in: Turn in f12 (mode 0), or for mode 1 off Gene's heading for the call and back after; then Gene is moved
// by RotY(his facing without Turn) . (Side * enemy +0x110, 0, Forward * enemy +0x118); RotY as the game's
// (sceVu0RotMatrixY): x' = x cos + z sin, z' = -x sin + z cos; +z forward, +x the left. Each vblank while the punish
// plays, Gene's motion (+0x428) and frame (+0x430, half steps at 60 FPS) give the curve's values, and Gene moves by
// the difference from what was applied.
//
// Heights and bend. func_002A8578 (actor, motion, events, ...) starts every motion. When Gene starts one of the
// punish's motions (pointers from the caught enemy's table, +0x694 -> +0x304) or the caught enemy one of its own,
// the motion is copied into kRewriteArea with bone 0's height (Gene / victim) and bone 1's x rotation (the victim's
// waist) changed by the curve (mot_edit addCurve, npc_poses' way), and started from there. A victim's track may also
// carry the hips back, the pelvis tilt and the knees together (K, Nov 12: Stinger on women; tools stinger_women.py), and the
// feet back / the legs' rotation (Spanking at 1.30; tools spank130.py). Copies are kept per
// (body, punish, motion) and reused; the area starts over when full, the list after a save state load (saveState).
//
// Gene's pose (K, Nov 8: his arms through the victim where the original keeps clear; Cobra Twist's arms round the
// neck on women and fat men; Gorilla Suplex's arms folded too far in on the fat men): Gene's tracks also carry his torso
// (bone 1 turned in bone 0's space: x, z) and each arm (the upper arm, 13 / 7, turned in the clavicle's space: raise
// about z, swing about y; the forearm, 14 / 8, about y in the upper arm's), put in front of the motion's own rotation
// every frame (mot_edit preRotate); the shoulder helpers 12 / 6, which follow the upper arms at about half in the game's
// motions, get half. Offline the same pre-rotations are measured (fit5.py gene_xform).
//
// The women's chest grab Suplex (K, Nov 15; claude/boobplex.md): a fit's pose lines give whole pre-rotations for chosen
// bones over chosen frames (Gene's arms solved offline onto the bust at a few Money values, blended at the motion's start
// for the current Money; her hanging arm raised out of his way), its bust line holds her bust jiggle still while his
// hands are on (bustHold, for bone_tweaks' Money), and "frontcam" with its cam lines replaces the punish camera (two parts
// of Gene's model keyed by his punish motion: setCamera), one of a few ways at random.
//
// Size (K, Nov 8). Enemies of one model come in many sizes (stage 1: 1.03, 1.05, 1.07 ... 1.10), so a pair is not
// fitted per size: it is scaled. Every actor's model, root travel (cObjBase_addNullSpeed: the motion's delta x +0x110 /
// +0x114 / +0x118) and the pair's placement (x enemy +0x110 / +0x118) scale with it, so Gene and the enemy scaled by
// one factor are the fitted pair, uniformly bigger or smaller. Each fit is for an enemy size (scale=: 1.0, 1.3); a
// body whose fits are all larger also has the game's own pairing at 1.0. At the catch the fit whose size needs the
// least change is taken (sizes marked off are not), and Gene and the enemy meet halfway: both by k = sqrt(fit size /
// the enemy's size), Gene up, the enemy down (or the reverse), easing in over kRampVblanks and back after the punish.
// Each changes by at most 12% (K, Nov 8: 6% at first; 12% lets Gene spank the stage 1 demon, size 1.20): past that a
// Buck Breaker pick doesn't fit (the enemy keeps its own punish), and the enemy's own punish plays with k held there. The game's placement calls run with the enemy at the fit's size x k.
// size= is the fit's own victim size change (0.95..1.05), on top. Bodies with no fits (other skeletons) are untouched.
//
// The body: the enemy's skeleton (its cModel's parts, list at +0x144, count at +0x2B4; each part's offset +0xE0 and
// parent +0x148), hashed from the parts after the root: recolours share it. Hashed at every use (an enemy number does
// not fix its model). Values in the file are at size 1; placement is scaled by the enemy's size, heights are the
// model's own (the actor's scale applies).

#include "kamite/punish_fit.h"
#include "kamite/paths.h"

#include "kamite/body_detail.h"
#include "kamite/bone_tweaks.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/mot_edit.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"
#include "runtime/ps2_pnach.h"
#include "runtime/ps2_save_state.h"

#include <array>
#include <atomic>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <random>
#include <set>
#include <sstream>
#include <string>
#include <iterator>
#include <tuple>
#include <vector>

namespace fs = std::filesystem;

namespace kamite::punish_fit
{
    namespace
    {
        namespace work = game::GameWork;

        constexpr uint32_t kAlign = 0x001299F0u, kStartMotion = 0x002A8578u;
        constexpr float kPi = 3.14159265358979f, kDegrees = kPi / 180.0f;
        constexpr uint32_t kEm00Entries = 3998u;

        // The punishes' motions (em00 entries; Gene's come from the caught enemy's table, cEm00_GetPlMotion).
        struct Motions
        {
            uint16_t gene[4];
            uint16_t victim[4];
        };
        constexpr Motions kMotions[kPunishCount] = {
            {{333, 0, 0, 0}, {329, 331, 0, 0}},
            {{811, 813, 815, 0}, {805, 807, 809, 0}},
            {{1551, 1553, 1557, 1555}, {1543, 1545, 1549, 1547}},
            {{1337, 1339, 1341, 0}, {1327, 1329, 1331, 0}},
            {{3840, 0, 0, 0}, {3838, 0, 0, 0}},
        };
        // func_001299F0's calls by the four punishes, by return address, and the Gene motion each one starts.
        struct Call
        {
            uint32_t ra;
            Punish punish;
            uint16_t geneMotion;
        };
        constexpr Call kCalls[] = {{0x0011770Cu, Suplex, 333},     {0x00117AC8u, Stinger, 811}, {0x00117B90u, Stinger, 813},
                                   {0x00117E40u, Stinger, 815},    {0x0011825Cu, CobraTwist, 1551}, {0x00118790u, Spanking, 1337},
                                   {0x0011806Cu, GorillaSuplex, 3840}};
        constexpr uint8_t kGenePhase[kPunishCount] = {57u, 59u, 61u, 62u, 60u};
        const char *const kNames[kPunishCount] = {"Suplex", "Stinger", "Cobra Twist", "Spanking", "Gorilla Suplex"};
        const char *const kFileNames[kPunishCount] = {"Suplex", "Stinger", "CobraTwist", "Spanking", "GorillaSuplex"};

        constexpr uint32_t kNumber = 0x564u, kScaleX = 0x110u, kScaleY = 0x114u, kScaleZ = 0x118u, kCaught = 0x694u;
        constexpr int kRampVblanks = 12; // the size changes ease in (and out) over this many vblanks (K: quick)
        constexpr float kMaxStep = 1.12f; // each of the two changes size by at most this factor (K: 12%, for the stage 1 demon)
        constexpr int kValues = 14;
        constexpr float kFeetIn = 0.6f; // a victim's legs value of 1 moves the ankles this much of the way to the middle
        constexpr uint32_t kParts = 0x144u, kPartCount = 0x2B4u, kPartOffset = 0xE0u, kPartParent = 0x148u;
        constexpr uint32_t kPartRotation = 0x100u; // the part's rotation x, y, z (radians; the motion samples into it)
        constexpr uint32_t kPartAnchor = 0xD0u;    // -> the local matrix's position (the motion samples translations into it)
        constexpr uint32_t kMotionSample = 0x00158328u, kCalcParts = 0x00149EB8u;
        constexpr uint32_t kMotionPlaying = 0x428u, kMotionFrame = 0x430u;
        // A fit variant for models drawn with the detailed bust (Money above 100%, K Nov 15: the chest grab has a fit for
        // each bust): kept with the node= variants under this name (no mesh node name starts with '+').
        const std::string kDetailBust = "+bust";
        // The model's mesh nodes (body_detail.cpp): a list from +0x25C, next at +0x404, its info at +0x414, the name at info +8.
        constexpr uint32_t kMeshHead = 0x25Cu, kNodeNext = 0x404u, kNodeInfo = 0x414u, kInfoName = 0x8u;

        // A curve: keys (frame, values). Gene: forward (m), side (m), turn (degrees), height (m), torso x, torso z,
        // left raise, left swing, left elbow, right raise, right swing, right elbow, left forearm twist (degrees), lift (m:
        // his whole body, the hips and both leg IK targets, so the feet leave the ground; K: Cobra Twist on the big bodies);
        // victim: height (m), bend (degrees), hips back (m), pelvis tilt (degrees, + = the seat up), legs (0..1: the knees
        // together; K, Nov 12: Stinger on women), feet back (m: the ankles' IK targets), thigh pitch and knee bend (degrees,
        // about x, for motions with the legs by rotation; K, Nov 12: Spanking at 1.30, the victim lowered onto Gene's thigh).
        struct Key
        {
            float frame = 0.0f;
            float v[kValues] = {};
        };
        struct Track
        {
            uint16_t motion = 0;
            bool gene = false;
            std::vector<Key> keys;
            void at(float frame, float out[kValues]) const
            {
                for (int i = 0; i < kValues; ++i)
                    out[i] = 0.0f;
                if (keys.empty())
                    return;
                if (frame <= keys.front().frame || keys.size() == 1)
                {
                    std::memcpy(out, keys.front().v, sizeof(Key::v));
                    return;
                }
                if (frame >= keys.back().frame)
                {
                    std::memcpy(out, keys.back().v, sizeof(Key::v));
                    return;
                }
                size_t i = 1;
                while (keys[i].frame < frame)
                    ++i;
                const Key &a = keys[i - 1], &b = keys[i];
                const float t = (frame - a.frame) / (b.frame - a.frame);
                for (int c = 0; c < kValues; ++c)
                    out[c] = a.v[c] + (b.v[c] - a.v[c]) * t;
            }
        };
        // A bone's whole pre-rotation per frame (rotation vectors, degrees), for one Money value (0: any): the women's chest
        // grab Suplex (K, Nov 15; tools chest_suplex.py), Gene's arms solved onto the bust at a few Money values and blended
        // at the motion's start. Outside its frames the bone keeps the fit's own pose (or none).
        struct Pose
        {
            uint16_t motion = 0;
            bool gene = false;
            float money = 0.0f;
            int bone = 0;
            std::vector<std::pair<float, std::array<float, 3>>> keys;
            std::array<float, 3> at(float frame) const
            {
                if (frame <= keys.front().first)
                    return keys.front().second;
                if (frame >= keys.back().first)
                    return keys.back().second;
                size_t i = 1;
                while (keys[i].first < frame)
                    ++i;
                const auto &[f0, a] = keys[i - 1];
                const auto &[f1, b] = keys[i];
                const float t = (frame - f0) / (f1 - f0);
                return {a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t};
            }
        };
        struct Fit
        {
            bool off = false;
            bool gorilla = false;  // a Suplex fit: the Suplex pick plays the Gorilla Suplex on this body
            bool own = false;      // for the enemy's own punish (its prompt unchanged), e.g. Spanking on women (K, Nov 12)
            bool frontCam = false; // the punish camera swung round to the pair's front (K, Nov 15: the women's chest grab)
            std::string node;      // node=: only for models with a mesh node whose name starts with this (K, Nov 15: mohawk women);
                                   // kDetailBust (bust=detail): only for models drawn with the detailed bust (body_detail)
            float scale = 1.0f;    // the enemy size it was fitted at
            float size = 1.0f;     // the victim's size change for the pairing
            std::vector<Track> tracks;
            std::vector<Pose> poses;
            uint16_t bustMotion = 0;                       // Gene's motion the bust hold follows
            std::vector<std::pair<float, float>> bust;     // Gene's frame -> how much the victim's bust jiggle is held (0..1)
            struct CamPath
            {
                uint16_t motion = 0;
                int variant = 0;
                std::vector<std::pair<float, std::array<float, 6>>> keys; // frame -> eye, look-at (Gene's model space)
            };
            std::vector<CamPath> cams; // frontcam: the punish camera's ways (one picked at random per punish)
            const CamPath *cam(uint16_t motion, int variant) const
            {
                for (const CamPath &c : cams)
                    if (c.motion == motion && c.variant == variant)
                        return &c;
                return nullptr;
            }
            const Track *find(uint16_t motion, bool gene) const
            {
                for (const Track &t : tracks)
                    if (t.motion == motion && t.gene == gene)
                        return &t;
                return nullptr;
            }
            bool hasPoses(uint16_t motion, bool gene) const
            {
                for (const Pose &p : poses)
                    if (p.motion == motion && p.gene == gene)
                        return true;
                return false;
            }
        };

        using Fits = std::vector<std::shared_ptr<Fit>>; // one body's fits for one punish, by enemy size

        // The fit a pair plays at enemy size s (see the top): fit nullptr = the game's own pairing at 1.0.
        struct Anchor
        {
            bool any = false;    // a fit (or the game's own) to scale to
            bool within = false; // reached with each of the two changing by at most kMaxStep
            const Fit *fit = nullptr;
            float scale = 1.0f; // the fit's enemy size
            float k = 1.0f;     // Gene's factor; the enemy's: scale x k / s
        };
        Anchor anchorFor(const Fits &fits, float s)
        {
            Anchor best;
            float bestD = 1e9f;
            const auto consider = [&](const Fit *f, float a) {
                const float d = std::fabs(std::log(s / a));
                if (d < bestD)
                {
                    bestD = d;
                    best.any = true;
                    best.fit = f;
                    best.scale = a;
                }
            };
            if (fits.front()->scale > 1.0f)
                consider(nullptr, 1.0f);
            for (const auto &f : fits)
                if (!f->off)
                    consider(f.get(), f->scale);
            if (best.any)
            {
                best.within = bestD <= 2.0f * std::log(kMaxStep) + 1e-4f;
                best.k = std::clamp(std::sqrt(s / best.scale), 1.0f / kMaxStep, kMaxStep);
            }
            return best;
        }

        // The pair playing (EE thread only).
        struct Pair
        {
            int punish = -1;
            uint32_t gene = 0, enemy = 0, body = 0;
            const Fit *fit = nullptr; // the fit played (nullptr: the game's own pairing, scaled, or no fit)
            int sizeKey = 100;        // the fit's size x 100 (the copies are per fit)
            float baseScale[3] = {1.0f, 1.0f, 1.0f};  // the enemy's own
            float placeScale[3] = {1.0f, 1.0f, 1.0f}; // the enemy at the fit's size x k: what placement scales by
            bool live = false;
            uint32_t geneMotionPtr = 0; // the pointer Gene's current punish motion was started with (ours or the game's)
            const Track *geneTrack = nullptr;
            float applied[3] = {}; // forward, side, turn (radians) put in so far
            int camVariant = 0;    // the fit's camera way for this punish (frontcam)
        };

        // An actor's size change (Gene's, the victim's), easing in while the pair plays and out after it (EE thread only).
        struct Resize
        {
            uint32_t actor = 0, number = 0;
            float base[3] = {1.0f, 1.0f, 1.0f};
            float now = 1.0f, target = 1.0f, written = 0.0f;
            bool active = false;
        };

        uint8_t *g_ram = nullptr;
        std::atomic<bool> g_on{false}; // [Game] BuckBreaker, switched live (setEnabled)
        bool g_ready = false;          // the placement is hooked (install)
        bool g_pairOn = false;         // the setting the pair in play began with (EE thread)
        PS2Runtime::RecompiledFunction g_align = nullptr;
        fs::path g_file, g_shippedFile;
        std::map<uint64_t, Fits> g_fits; // punish << 32 | body, by size; read at start, then only read
        std::map<uint64_t, Fits> g_ownFits; // the same for the enemy's own punish (fits marked own)
        // node= fits: (punish, body) -> (node prefix, the variant's key); the variant's fits are in g_fits under its key, so
        // everything keyed by body (pair, save states, motion copies) keeps them apart from the body's other fits.
        std::map<uint64_t, std::vector<std::pair<std::string, uint32_t>>> g_variants;
        std::map<uint32_t, std::pair<uint32_t, std::string>> g_variantOf; // the variant's key -> (body, node), for the log
        std::map<uint32_t, std::string> g_bodyNames; // body -> its type ("men", ...), punish_fit.txt's body lines
        Pair g_pair;

        // A punish keeps the setting it began with (K, Nov 13: what is under way plays out as it started).
        bool pairOn(uint32_t enemy, int punish, bool starts)
        {
            return !starts && g_pair.enemy == enemy && g_pair.punish == punish ? g_pairOn : g_on.load(std::memory_order_relaxed);
        }
        Resize g_resize[2]; // the victim, Gene
        std::map<std::tuple<uint32_t, int, uint16_t, bool, int, int, int>, uint32_t> g_copies; // (body, punish, motion, gene, size, Money, camera) -> copy

        // The posed bones in float (K, Nov 15: the chest grab's hands trembled on her). A motion stores each rotation as
        // 8-bit steps of its whole range (0.4-0.7 deg on the arms, whose ranges span the throw), so a pose keyed every
        // frame lands up to half a step off at every frame, a different way each frame: on the arms' four bones up to
        // ~0.8 cm at the palm, a tremble where the hands are held on her. The copy keeps the 8-bit version (the game's own
        // logic plays it); the pose lines' bones are also kept here as whole rotations per frame (pre . the original's
        // own), and while a copy plays they are written over the sampled rotations (part +0x100) as the parts are built
        // (cModel_calcParts), between frames slerped at the time the motion was sampled (func_00158328 reads +0x430
        // before it advances). Copy address -> its posed bones; cleared with g_copies.
        struct FloatPose
        {
            int bone = 0;
            std::vector<std::pair<float, mot::Mat>> keys; // motion time -> whole local rotation (every key of its lines)
        };
        // Every other edited channel of a posed copy in float too (K, Nov 15: the hands still trembled on her): the
        // hold hangs off Gene's torso (bone 1, the fit's own pose) and hips (bone 0 y) and her hips (0 y) and spine bend
        // (1 x), all keyed every frame in 8 bits as well (offline, his hands in her chest frame: up to 1 cm at frames,
        // 2 cm at half frames; 0 with all four in float). A rotation component (+0x100) or the hips' height / depth
        // (bone 0 y / z: the sampler writes translations through the part's anchor +0xD0), every half frame from 0.
        struct FloatChannel
        {
            int bone = 0, component = 0;
            std::vector<float> values; // at motion time i / 2
        };
        struct FloatSet
        {
            std::vector<FloatPose> poses; // the edited bones' whole rotations first, the pose lines after (they win)
            std::vector<FloatChannel> channels;
        };
        std::map<uint32_t, FloatSet> g_floatPoses;
        std::map<uint32_t, float> g_sampledAt; // model -> the motion time its parts were last sampled at (while posed)
        uint32_t g_areaAt = kRewriteArea;
        std::set<uint64_t> g_logged;

        uint64_t fitKey(int punish, uint32_t body) { return (static_cast<uint64_t>(punish) << 32) | body; }

        bool inRam(uint32_t p, uint32_t size)
        {
            p &= 0x1FFFFFFFu;
            return p >= 0x100000u && p < PS2_RDRAM_SIZE && size <= PS2_RDRAM_SIZE - p;
        }
        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), sizeof(v));
            return v;
        }
        float rdF(const uint8_t *rdram, uint32_t a)
        {
            float v;
            std::memcpy(&v, rdram + (a & PS2_RAM_MASK), sizeof(v));
            return v;
        }
        void wrF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & PS2_RAM_MASK), &v, sizeof(v)); }

        float wrap(float a)
        {
            while (a > kPi)
                a -= 2.0f * kPi;
            while (a < -kPi)
                a += 2.0f * kPi;
            return a;
        }

        // The enemy's model file: +0x2FE's low byte (func_002CFE68 builds "em%02x" from it; the kind +0x564 does not fix
        // the model).
        constexpr uint32_t kModelId = 0x2FEu;
        std::string enemyName(const uint8_t *rdram, uint32_t enemy)
        {
            uint16_t id;
            std::memcpy(&id, rdram + ((enemy + kModelId) & PS2_RAM_MASK), sizeof(id));
            char text[8];
            std::snprintf(text, sizeof(text), "em%02x", id & 0xFFu);
            return text;
        }

        // The enemy's skeleton hashed (FNV-1a): part count, then each part after the root: its parent's index and its
        // offset's bits (tools/models/punish_fit/common.py runtime_key computes the same from the file). 0 when it
        // cannot be read.
        // The enemy's parts (root first); their count, 0 if they can't be read.
        int readParts(const uint8_t *rdram, uint32_t enemy, uint32_t parts[64])
        {
            const int n = rdram[(enemy + kPartCount) & PS2_RAM_MASK];
            if (n < 2 || n > 64)
                return 0;
            uint32_t p = rd32(rdram, enemy + kParts);
            for (int i = 0; i < n; ++i)
            {
                if (!inRam(p, 0x150u))
                    return 0;
                parts[i] = p & PS2_RAM_MASK;
                p = rd32(rdram, p + kParts);
            }
            return n;
        }

        // The hash of the first n parts (common.runtime_key offline).
        uint32_t hashParts(const uint8_t *rdram, const uint32_t *parts, int n)
        {
            uint32_t h = 2166136261u;
            const auto mix = [&h](const void *data, size_t size) {
                const auto *b = static_cast<const uint8_t *>(data);
                for (size_t i = 0; i < size; ++i)
                    h = (h ^ b[i]) * 16777619u;
            };
            mix(&n, sizeof(n));
            for (int i = 1; i < n; ++i)
            {
                int8_t index = -1;
                const uint32_t parent = rd32(rdram, parts[i] + kPartParent) & PS2_RAM_MASK;
                for (int j = 0; j < n; ++j)
                    if (parts[j] == parent && j != i)
                        index = static_cast<int8_t>(j);
                mix(&index, 1);
                mix(rdram + parts[i] + kPartOffset, 12);
            }
            return h ? h : 1u;
        }

        // The body. A skeleton in no body line whose body bones (parts 0-25) match a known body's plays that body's
        // fits: the head's extra bones (26..) vary between identical bodies (punish_fit.txt's core lines). A body
        // still unknown is logged once with its parts (parent: offset), so it can be added offline.
        constexpr int kCoreParts = 26;
        std::map<uint32_t, uint32_t> g_core; // hash of parts 0-25 -> a body with fits
        std::set<uint32_t> g_seenBodies;
        uint32_t bodyOf(const uint8_t *rdram, uint32_t enemy)
        {
            uint32_t parts[64];
            const int n = readParts(rdram, enemy, parts);
            if (!n)
                return 0x80000000u | (rd32(rdram, enemy + kNumber) & 0xFFFFu);
            const uint32_t body = hashParts(rdram, parts, n);
            if (g_bodyNames.empty() || g_bodyNames.count(body))
                return body;
            const uint32_t core = n >= kCoreParts ? hashParts(rdram, parts, kCoreParts) : 0u;
            const auto it = g_core.find(core);
            const uint32_t alias = it != g_core.end() ? it->second : 0u;
            if (g_seenBodies.insert(body).second)
            {
                std::ostringstream text;
                text << "punish fit: body " << std::hex << body << " (" << enemyName(rdram, enemy) << ", " << std::dec << n
                     << " parts, core " << std::hex << core << std::dec << ") ";
                if (alias)
                    text << "plays body " << std::hex << alias << std::dec << "'s fits";
                else
                {
                    text << "unknown:";
                    for (int i = 1; i < n; ++i)
                    {
                        const uint32_t parent = rd32(rdram, parts[i] + kPartParent) & PS2_RAM_MASK;
                        int index = -1;
                        for (int j = 0; j < n; ++j)
                            if (parts[j] == parent && j != i)
                                index = j;
                        float t[3];
                        std::memcpy(t, rdram + parts[i] + kPartOffset, sizeof(t));
                        char item[64];
                        std::snprintf(item, sizeof(item), " %d:%d:%.4f,%.4f,%.4f", i, index, t[0], t[1], t[2]);
                        text << item;
                    }
                }
                KM_INFO(Game, text.str());
            }
            return alias ? alias : body;
        }

        const Fits *fitsFor(int punish, uint32_t body)
        {
            const auto it = g_fits.find(fitKey(punish, body));
            return it == g_fits.end() || it->second.empty() ? nullptr : &it->second;
        }

        // Enemies whose prompt offers a punish Buck Breaker changed (setChanged; the last few enemies seen).
        struct Changed
        {
            uint32_t enemy = 0u;
            bool changed = false;
        };
        Changed g_changed[16];
        size_t g_changedNext = 0u;
        bool changedFor(uint32_t enemy)
        {
            enemy &= PS2_RAM_MASK;
            for (const Changed &c : g_changed)
                if (c.enemy == enemy)
                    return c.changed;
            return false;
        }
        // The fits for a pair Buck Breaker made; nullptr for one the game already had (it plays as the game made it).
        const Fits *ownFitsFor(int punish, uint32_t body)
        {
            const auto it = g_ownFits.find(fitKey(punish, body));
            return it == g_ownFits.end() || it->second.empty() ? nullptr : &it->second;
        }
        // A changed punish plays the body's fits; the enemy's own punish only its own fits (none: the game's, untouched).
        const Fits *fitsForPair(const uint8_t *, uint32_t enemy, int punish, uint32_t body)
        {
            return changedFor(enemy) ? fitsFor(punish, body) : ownFitsFor(punish, body);
        }

        // Whether the enemy's model has a mesh node whose name starts with `prefix` (names are up to 8 characters).
        bool hasNode(const uint8_t *rdram, uint32_t enemy, const std::string &prefix)
        {
            if (prefix.empty() || prefix.size() > 8u)
                return false;
            uint32_t node = rd32(rdram, enemy + kMeshHead);
            for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
            {
                const uint32_t info = rd32(rdram, node + kNodeInfo);
                if (inRam(info, 16u) && std::memcmp(rdram + ((info + kInfoName) & PS2_RAM_MASK), prefix.data(), prefix.size()) == 0)
                    return true;
                node = rd32(rdram, node + kNodeNext);
            }
            return false;
        }

        // The body's fit variant for this model (node= / bust=detail fits), else the body.
        uint32_t variantOf(const uint8_t *rdram, uint32_t enemy, int punish, uint32_t body)
        {
            const auto it = g_variants.find(fitKey(punish, body));
            if (it != g_variants.end())
                for (const auto &[prefix, key] : it->second)
                    if (prefix == kDetailBust ? body_detail::bustDetailed(rdram, enemy) : hasNode(rdram, enemy, prefix))
                        return key;
            return body;
        }

        // The body a pair's fits are keyed by: a Buck Breaker pick takes the model's variant, the enemy's own punish its body.
        uint32_t pairBody(const uint8_t *rdram, uint32_t enemy, int punish)
        {
            const uint32_t body = bodyOf(rdram, enemy);
            return changedFor(enemy) ? variantOf(rdram, enemy, punish, body) : body;
        }
        const Fit *fitFor(int punish, uint32_t body) // its flags (off, gorilla)
        {
            const Fits *fits = fitsFor(punish, body);
            return fits ? fits->front().get() : nullptr;
        }

        // The motion `index` of an enemy's em00-style table (pack + its offset, u32 wrap like the game), 0 if none.
        uint32_t entry(const uint8_t *rdram, uint32_t enemy, uint16_t index)
        {
            const uint32_t pack = rd32(rdram, enemy + game::GameObj::kMotionData) & PS2_RAM_MASK;
            if (!inRam(pack, 4u + 4u * kEm00Entries) || rd32(rdram, pack) != kEm00Entries)
                return 0u;
            const uint32_t offset = rd32(rdram, pack + 4u + 4u * index);
            return offset ? (pack + offset) & PS2_RAM_MASK : 0u;
        }

        uint32_t player(const uint8_t *rdram) { return rd32(rdram, work::address(work::kPlayerObj)) & PS2_RAM_MASK; }

        void logPair(int punish, uint32_t body, uint32_t enemy, const char *what, float size)
        {
            const int sizeKey = static_cast<int>(std::lround(size * 100.0f));
            if (!g_logged.insert(fitKey(punish, body) ^ (static_cast<uint64_t>(sizeKey & 0xFFFF) << 40)).second)
                return;
            const auto variant = g_variantOf.find(body);
            KM_INFO(Game, "punish fit: " << kNames[punish] << " on body " << std::hex
                                         << (variant != g_variantOf.end() ? variant->second.first : body) << std::dec
                                         << (variant == g_variantOf.end() ? std::string()
                                             : variant->second.second == kDetailBust ? std::string(" (detailed bust)")
                                                                                      : " node " + variant->second.second)
                                         << " (" << enemyName(g_ram, enemy) << ", size " << size << "): " << what);
        }

        // Moves Gene by (side, forward) in his facing without Turn (scaled by the enemy's own size) and turns him.
        void moveGene(uint8_t *rdram, uint32_t gene, const float scale[3], float side, float forward, float turn, float facing)
        {
            const uint32_t pos = rd32(rdram, gene + game::GameObj::kPos);
            if (!inRam(pos, 12u))
                return;
            const float x = side * scale[0], z = forward * scale[2];
            const float c = std::cos(facing), s = std::sin(facing);
            if (x != 0.0f || z != 0.0f)
            {
                wrF(rdram, pos, rdF(rdram, pos) + x * c + z * s);
                wrF(rdram, pos + 8u, rdF(rdram, pos + 8u) - x * s + z * c);
            }
            if (turn != 0.0f)
                wrF(rdram, gene + 0x104u, wrap(rdF(rdram, gene + 0x104u) + turn));
        }

        void writeScale(uint8_t *rdram, uint32_t actor, const float base[3], float f)
        {
            for (uint32_t i = 0; i < 3; ++i)
                wrF(rdram, actor + kScaleX + 4u * i, base[i] * f);
        }
        // The size change's actor still there, its scale still what we last wrote.
        bool resizeOurs(const uint8_t *rdram, const Resize &r)
        {
            return inRam(r.actor, 0x700u) && rd32(rdram, r.actor + kNumber) == r.number && rdF(rdram, r.actor + kScaleY) == r.written;
        }
        void finishResizes(uint8_t *rdram)
        {
            for (Resize &r : g_resize)
            {
                if (r.active && resizeOurs(rdram, r))
                    writeScale(rdram, r.actor, r.base, 1.0f);
                r.active = false;
            }
        }
        void startResize(uint8_t *rdram, Resize &r, uint32_t actor, const float base[3], float target)
        {
            r = Resize{};
            if (target == 1.0f)
                return;
            r.actor = actor;
            r.number = rd32(rdram, actor + kNumber);
            std::memcpy(r.base, base, sizeof(r.base));
            r.target = target;
            r.written = rdF(rdram, actor + kScaleY);
            r.active = true;
        }
        void stepResizes()
        {
            for (Resize &r : g_resize)
            {
                if (!r.active)
                    continue;
                if (!resizeOurs(g_ram, r))
                {
                    r.active = false;
                    continue;
                }
                const float target = g_pair.live && (r.actor == g_pair.enemy || r.actor == g_pair.gene) ? r.target : 1.0f;
                const float step = std::fabs(r.target - 1.0f) / kRampVblanks;
                r.now = r.now < target ? std::min(target, r.now + step) : std::max(target, r.now - step);
                writeScale(g_ram, r.actor, r.base, r.now);
                r.written = rdF(g_ram, r.actor + kScaleY);
                if (target == 1.0f && r.now == 1.0f)
                    r.active = false;
            }
        }
        // The actor's own size: its scale, or what it was before our size change.
        void ownScale(const uint8_t *rdram, uint32_t actor, float out[3])
        {
            const Resize *mine = nullptr;
            for (const Resize &r : g_resize)
                if (r.active && r.actor == actor)
                    mine = &r;
            for (uint32_t i = 0; i < 3; ++i)
                out[i] = mine ? mine->base[i] : rdF(rdram, actor + kScaleX + 4u * i);
        }
        int sizeKeyOf(float s) { return static_cast<int>(std::lround(s * 100.0f)); }
        int sizeKeyOf(const Fit *f, float s) { return sizeKeyOf(s) + (f && f->own ? 100000 : 0); } // own fits: copies of their own

        // The fit for the enemy's punish at its size: the pair's, or its anchor's for a motion started before the pair's
        // placement call; nullptr when there is none (or it doesn't fit).
        const Fit *fitNow(const uint8_t *rdram, uint32_t enemy, int punish, int &sizeKey)
        {
            if (g_pair.enemy == enemy && g_pair.punish == punish && g_pair.live)
            {
                sizeKey = g_pair.sizeKey;
                return g_pair.fit;
            }
            const Fits *fits = fitsForPair(rdram, enemy, punish, pairBody(rdram, enemy, punish));
            if (!fits)
                return nullptr;
            float scale[3];
            ownScale(rdram, enemy, scale);
            const Anchor an = anchorFor(*fits, scale[1]);
            if (!an.any)
                return nullptr;
            sizeKey = sizeKeyOf(an.fit, an.scale);
            return an.fit;
        }

        // func_001299F0, wrapped.
        // One of the fit's camera ways at random (K, Nov 15), never the one picked last. The host's generator: the game's
        // own random rolls (the AI's, recorded by test cases) stay untouched.
        int pickCamera(const Fit &fit, uint16_t motion)
        {
            static std::mt19937 rng{std::random_device{}()};
            static int last = -1;
            std::vector<int> ways;
            for (const Fit::CamPath &c : fit.cams)
                if (c.motion == motion && c.variant != last)
                    ways.push_back(c.variant);
            if (ways.empty())
                return last < 0 ? 0 : last;
            last = ways[std::uniform_int_distribution<size_t>(0, ways.size() - 1)(rng)];
            return last;
        }

        void alignHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const Call *call = nullptr;
            for (const Call &c : kCalls)
                if (c.ra == ra)
                    call = &c;
            const uint32_t gene = GPR_U32(ctx, 4) & PS2_RAM_MASK, enemy = GPR_U32(ctx, 5) & PS2_RAM_MASK;
            if (!call || !inRam(gene, 0x700u) || !inRam(enemy, 0x700u))
            {
                g_align(rdram, ctx, runtime);
                return;
            }
            const uint32_t body = pairBody(rdram, enemy, call->punish);
            const bool first = call->geneMotion == kMotions[call->punish].gene[0];
            const bool newPair = first || g_pair.enemy != enemy || g_pair.punish != call->punish || g_pair.gene != gene;
            if (newPair)
                g_pairOn = g_on.load(std::memory_order_relaxed);
            if (!g_pairOn)
            {
                // Buck Breaker off when this punish began: the game's own placement, exactly.
                if (newPair)
                {
                    finishResizes(rdram);
                    g_pair = Pair{};
                    g_pair.punish = call->punish;
                    g_pair.gene = gene;
                    g_pair.enemy = enemy;
                }
                g_align(rdram, ctx, runtime);
                return;
            }
            if (newPair)
            {
                // A new pair: the fit whose size needs the least change, Gene and the enemy scaled to meet it (any size
                // change still easing out ends first).
                finishResizes(rdram);
                ownScale(rdram, enemy, g_pair.baseScale);
                std::memcpy(g_pair.placeScale, g_pair.baseScale, sizeof(g_pair.placeScale));
                const Fits *fits = fitsForPair(rdram, enemy, call->punish, body);
                const Anchor an = fits ? anchorFor(*fits, g_pair.baseScale[1]) : Anchor{};
                g_pair.fit = an.fit;
                g_pair.camVariant = an.fit ? pickCamera(*an.fit, call->geneMotion) : 0;
                g_pair.sizeKey = sizeKeyOf(an.fit, an.scale);
                g_pair.live = an.any;
                if (an.any)
                {
                    const float e = an.scale * an.k / g_pair.baseScale[1]; // the enemy's factor (before the fit's size=)
                    for (int i = 0; i < 3; ++i)
                        g_pair.placeScale[i] = g_pair.baseScale[i] * e;
                    startResize(rdram, g_resize[0], enemy, g_pair.baseScale, e * (an.fit ? an.fit->size : 1.0f));
                    float geneBase[3];
                    ownScale(rdram, gene, geneBase);
                    startResize(rdram, g_resize[1], gene, geneBase, an.k);
                }
                if (first)
                {
                    char what[96];
                    if (!an.any)
                        std::snprintf(what, sizeof(what), "%s", fits ? "doesn't fit" : "no fit (the game's own placement)");
                    else
                        std::snprintf(what, sizeof(what), "%s at %.2f%s, Gene x%.3f, enemy x%.3f",
                                      an.fit ? "fitted" : "the game's own pairing", an.scale,
                                      an.within ? "" : " (past the cap: held there)", an.k,
                                      an.scale * an.k / g_pair.baseScale[1] * (an.fit ? an.fit->size : 1.0f));
                    logPair(call->punish, body, enemy, what, g_pair.baseScale[1]);
                }
            }
            const Fit *fit = g_pair.fit;
            const Track *track = fit ? fit->find(call->geneMotion, true) : nullptr;
            float v[kValues] = {};
            if (track)
                track->at(0.0f, v);
            const uint32_t mode = GPR_U32(ctx, 7);
            const float turn = v[2] * kDegrees;
            if (mode == 1u)
            {
                if (turn != 0.0f)
                    wrF(rdram, gene + 0x104u, wrap(rdF(rdram, gene + 0x104u) - turn));
            }
            else
                ctx->f[12] += turn;
            // The game's placement with the enemy at the fit's size x k (not its own, nor mid-ease).
            float held[3];
            for (uint32_t i = 0; i < 3; ++i)
                held[i] = rdF(rdram, enemy + kScaleX + 4u * i);
            const float one = 1.0f;
            if (g_pair.live)
                writeScale(rdram, enemy, g_pair.placeScale, one);
            g_align(rdram, ctx, runtime);
            if (g_pair.live)
                writeScale(rdram, enemy, held, one);
            const float facing = wrap(rdF(rdram, gene + 0x104u) - (mode == 1u ? 0.0f : turn));
            moveGene(rdram, gene, g_pair.placeScale, v[1], v[0], mode == 1u ? turn : 0.0f, facing);

            g_pair.punish = call->punish;
            g_pair.gene = gene;
            g_pair.enemy = enemy;
            g_pair.body = body;
            g_pair.geneTrack = track;
            g_pair.geneMotionPtr = 0u;
            g_pair.applied[0] = v[0];
            g_pair.applied[1] = v[1];
            g_pair.applied[2] = turn;
        }

        // A rotation vector (degrees) as a matrix (Rodrigues; the offline tools' scipy as_rotvec).
        mot::Mat fromRotvec(const std::array<float, 3> &v)
        {
            const float a = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
            if (a < 1e-6f)
                return {1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f};
            const float x = v[0] / a, y = v[1] / a, z = v[2] / a;
            const float c = std::cos(a * kDegrees), s = std::sin(a * kDegrees), k = 1.0f - c;
            return {c + x * x * k,     x * y * k - z * s, x * z * k + y * s,
                    y * x * k + z * s, c + y * y * k,     y * z * k - x * s,
                    z * x * k - y * s, z * y * k + x * s, c + z * z * k};
        }

        // The fit's pose lines for one motion: per bone, its whole pre-rotation at each frame its lines cover (`first` ..
        // `last`), blended between the two Money values nearest `money` (K, Nov 15: Gene's hands follow Money's bust).
        struct PoseOverride
        {
            int bone = 0, first = 0, last = -1;
            std::vector<mot::Mat> pre; // one a frame; only first .. last are meant
        };
        std::vector<PoseOverride> poseOverrides(const Fit &fit, uint16_t motion, bool gene, int frames, float money)
        {
            std::map<int, std::vector<const Pose *>> byBone;
            for (const Pose &p : fit.poses)
                if (p.motion == motion && p.gene == gene && !p.keys.empty())
                    byBone[p.bone].push_back(&p);
            std::vector<PoseOverride> out;
            for (auto &[bone, list] : byBone)
            {
                std::sort(list.begin(), list.end(), [](const Pose *a, const Pose *b) { return a->money < b->money; });
                const Pose *lo = list.front(), *hi = list.back();
                for (const Pose *p : list)
                    if (p->money <= money)
                        lo = p;
                for (auto it = list.rbegin(); it != list.rend(); ++it)
                    if ((*it)->money >= money)
                        hi = *it;
                if (money <= list.front()->money)
                    lo = hi = list.front();
                if (money >= list.back()->money)
                    lo = hi = list.back();
                const float t = hi->money > lo->money ? (money - lo->money) / (hi->money - lo->money) : 0.0f;
                PoseOverride o;
                o.bone = bone;
                o.first = std::max(0, static_cast<int>(std::ceil(lo->keys.front().first)));
                o.last = std::min(frames - 1, static_cast<int>(lo->keys.back().first));
                o.pre.resize(frames, fromRotvec({0.0f, 0.0f, 0.0f}));
                for (int f = o.first; f <= o.last; ++f)
                {
                    const auto a = lo->at(static_cast<float>(f)), b = hi->at(static_cast<float>(f));
                    o.pre[f] = fromRotvec({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t, a[2] + (b[2] - a[2]) * t});
                }
                out.push_back(std::move(o));
            }
            return out;
        }

        // The pose lines' bones as whole rotations at each of their keys (whole frames, and half frames where the lines
        // have them, for 60 FPS): the pre-rotation (Money blended as poseOverrides) on the original motion's own at that
        // time (m: the copy before any edit).
        std::vector<FloatPose> floatPoses(const Fit &fit, uint16_t motion, bool gene, const std::vector<uint8_t> &m, float money)
        {
            std::vector<FloatPose> out;
            std::vector<mot::Rec> own;
            if (!mot::parse(m, own))
                return out;
            std::map<int, std::vector<const Pose *>> byBone;
            for (const Pose &p : fit.poses)
                if (p.motion == motion && p.gene == gene && !p.keys.empty())
                    byBone[p.bone].push_back(&p);
            for (auto &[bone, list] : byBone)
            {
                std::sort(list.begin(), list.end(), [](const Pose *a, const Pose *b) { return a->money < b->money; });
                const Pose *lo = list.front(), *hi = list.back();
                for (const Pose *p : list)
                    if (p->money <= money)
                        lo = p;
                for (auto it = list.rbegin(); it != list.rend(); ++it)
                    if ((*it)->money >= money)
                        hi = *it;
                if (money <= list.front()->money)
                    lo = hi = list.front();
                if (money >= list.back()->money)
                    lo = hi = list.back();
                const float t = hi->money > lo->money ? (money - lo->money) / (hi->money - lo->money) : 0.0f;
                const mot::Rec *channel[3] = {};
                for (const mot::Rec &r : own)
                    if (r.bone == bone && (r.channel & 0x0F) >= 3 && (r.channel & 0x0F) <= 5)
                        channel[(r.channel & 0x0F) - 3] = &r;
                FloatPose fp;
                fp.bone = bone;
                for (const auto &key : lo->keys)
                {
                    const float time = key.first;
                    if (time < 0.0f)
                        continue;
                    const auto a = lo->at(time), b = hi->at(time);
                    float e[3];
                    for (int c = 0; c < 3; ++c)
                        e[c] = channel[c] ? mot::sample(*channel[c], time) : 0.0f;
                    fp.keys.emplace_back(time, mot::mul(fromRotvec({a[0] + (b[0] - a[0]) * t, a[1] + (b[1] - a[1]) * t,
                                                                    a[2] + (b[2] - a[2]) * t}),
                                                        mot::euler(e[0], e[1], e[2])));
                }
                if (!fp.keys.empty())
                    out.push_back(std::move(fp));
            }
            return out;
        }

        // The punish camera (K, Nov 15). Gene's punish motions carry the camera as two root-level parts of his model, 27 =
        // the eye, 28 = what it looks at (pl00 func_00124EC0: cCamManager_setPartsCamera, then cPartsCamera_SetParts with
        // parts 27 and 28; the parts camera puts them through Gene's matrix), keyed as translations in his model space. A
        // fit's cam line gives both per frame (to its last key; the game's own after): the women's chest grab is shot close
        // and from the front for the grab, then goes round to the game's own camera by frame 60 (four ways round, K: one
        // at random). The game's own translations are replaced (addCurve by the difference).
        void setCamera(std::vector<uint8_t> &m, uint16_t frames, const Fit::CamPath &path)
        {
            const auto &keys = path.keys;
            const auto value = [&](float f, int i) {
                if (f <= keys.front().first)
                    return keys.front().second[i];
                if (f >= keys.back().first)
                    return keys.back().second[i];
                size_t k = 1;
                while (keys[k].first < f)
                    ++k;
                const float t = (f - keys[k - 1].first) / (keys[k].first - keys[k - 1].first);
                return keys[k - 1].second[i] + (keys[k].second[i] - keys[k - 1].second[i]) * t;
            };
            for (int part = 0; part < 2; ++part)
                for (int c = 0; c < 3; ++c)
                {
                    std::vector<mot::Rec> recs;
                    if (!mot::parse(m, recs))
                        return;
                    const mot::Rec *own = nullptr;
                    for (const mot::Rec &r : recs)
                        if (r.bone == 27 + part && (r.channel & 0x0F) == c)
                            own = &r;
                    if (!own)
                        continue;
                    std::vector<float> delta(frames, 0.0f); // past the path's last key: the game's own camera
                    for (int f = 0; f < frames && static_cast<float>(f) <= keys.back().first; ++f)
                        delta[f] = value(static_cast<float>(f), part * 3 + c) - mot::sample(*own, static_cast<float>(f));
                    mot::addCurve(m, 27 + part, c, delta, 1);
                }
        }

        // A copy of the motion at `motion` with the track's heights (and the victim's bend), in kRewriteArea; 0 if
        // nothing changes or it does not fit.
        mot::Mat slerp(const mot::Mat &a, const mot::Mat &b, float u);

        // The edits rewrite() made to a posed copy, played in float (FloatChannel above): whole rotations pre . own . post
        // for the pre-rotated bones (own with any curve added to its rotation), values for the other curves; every half
        // frame, from the original's own curves (sampled between frames as the game does) and the edits between frames.
        struct CurveEdit
        {
            int bone = 0, component = 0;
            std::vector<float> delta; // a frame
        };
        struct PreEdit
        {
            int bone = 0;
            std::vector<mot::Mat> pre, post; // a frame (the last repeats; empty: none)
        };
        FloatSet editedFloats(const std::vector<mot::Rec> &own, int frames, const std::vector<CurveEdit> &curves,
                              const std::vector<PreEdit> &pres)
        {
            FloatSet out;
            const int steps = 2 * (frames - 1);
            if (steps < 0)
                return out;
            auto ownRec = [&](int bone, int component) -> const mot::Rec * {
                for (const mot::Rec &r : own)
                    if (r.bone == bone && (r.channel & 0x0F) == component)
                        return &r;
                return nullptr;
            };
            auto deltaAt = [&](int bone, int component, float t) {
                float d = 0.0f;
                for (const CurveEdit &c : curves)
                    if (c.bone == bone && c.component == component && !c.delta.empty())
                    {
                        const int n = static_cast<int>(c.delta.size());
                        const int f = std::min(static_cast<int>(t), n - 1), g = std::min(f + 1, n - 1);
                        const float u = t - static_cast<float>(static_cast<int>(t));
                        d += c.delta[f] + (c.delta[g] - c.delta[f]) * u;
                    }
                return d;
            };
            auto matAt = [](const std::vector<mot::Mat> &v, float t) {
                if (v.empty())
                    return mot::euler(0.0f, 0.0f, 0.0f);
                const int n = static_cast<int>(v.size());
                const int f = std::min(static_cast<int>(t), n - 1), g = std::min(f + 1, n - 1);
                const float u = t - static_cast<float>(static_cast<int>(t));
                return u > 0.0f && f != g ? slerp(v[f], v[g], u) : v[f];
            };
            std::set<int> preBones;
            for (const PreEdit &p : pres)
            {
                preBones.insert(p.bone);
                FloatPose fp;
                fp.bone = p.bone;
                const mot::Rec *rec[3] = {ownRec(p.bone, 3), ownRec(p.bone, 4), ownRec(p.bone, 5)};
                for (int i = 0; i <= steps; ++i)
                {
                    const float t = 0.5f * static_cast<float>(i);
                    float e[3];
                    for (int c = 0; c < 3; ++c)
                        e[c] = (rec[c] ? mot::sample(*rec[c], t) : 0.0f) + deltaAt(p.bone, 3 + c, t);
                    mot::Mat r = mot::mul(matAt(p.pre, t), mot::euler(e[0], e[1], e[2]));
                    if (!p.post.empty())
                        r = mot::mul(r, matAt(p.post, t));
                    fp.keys.emplace_back(t, r);
                }
                out.poses.push_back(std::move(fp));
            }
            for (const CurveEdit &c : curves)
            {
                // Rotations of bones not pre-rotated; translations only the hips' height and depth (an x the sampler may
                // mirror, the legs' IK targets are not a part's own position).
                const bool rotation = c.component >= 3 && c.component <= 5;
                if ((rotation && preBones.count(c.bone)) || (!rotation && !(c.bone == 0 && (c.component == 1 || c.component == 2))))
                    continue;
                if (std::any_of(out.channels.begin(), out.channels.end(),
                                [&](const FloatChannel &f) { return f.bone == c.bone && f.component == c.component; }))
                    continue;
                const mot::Rec *rec = ownRec(c.bone, c.component);
                if (!rotation && !rec)
                    continue;
                FloatChannel fc;
                fc.bone = c.bone;
                fc.component = c.component;
                for (int i = 0; i <= steps; ++i)
                {
                    const float t = 0.5f * static_cast<float>(i);
                    fc.values.push_back((rec ? mot::sample(*rec, t) : 0.0f) + deltaAt(c.bone, c.component, t));
                }
                out.channels.push_back(std::move(fc));
            }
            return out;
        }

        uint32_t rewrite(uint8_t *rdram, uint32_t motion, const Track &track, const Fit &fit, uint32_t body, int punish, int sizeKey)
        {
            const bool posed = fit.hasPoses(track.motion, track.gene);
            const float money = bone_tweaks::money();
            const int camVariant = track.gene && fit.frontCam && fit.cam(track.motion, g_pair.camVariant) ? g_pair.camVariant : -1;
            const auto key = std::make_tuple(body, punish, track.motion, track.gene, sizeKey,
                                             posed ? static_cast<int>(std::lround(money * 100.0f)) : 0, camVariant);
            if (const auto it = g_copies.find(key); it != g_copies.end())
                return it->second;
            const uint32_t length = mot::motionLength(rdram + motion, PS2_RDRAM_SIZE - motion);
            if (length == 0u || length > 0x10000u)
                return 0u;
            std::vector<uint8_t> m(rdram + motion, rdram + motion + length);
            uint16_t frames;
            std::memcpy(&frames, m.data() + 4, 2);
            // A posed copy plays every edit in float (FloatSet): the original's curves and each edit as made.
            std::vector<mot::Rec> ownRecs;
            if (posed)
                mot::parse(m, ownRecs);
            std::vector<CurveEdit> curveEdits;
            std::vector<PreEdit> preEdits;
            auto addCurve = [&](int bone, int component, const std::vector<float> &delta) {
                mot::addCurve(m, bone, component, delta, 1);
                if (posed)
                    curveEdits.push_back({bone, component, delta});
            };
            auto preRotate = [&](int bone, const std::vector<mot::Mat> &pre, const std::vector<mot::Mat> &post = {}) {
                mot::preRotate(m, bone, pre, 1, post);
                if (posed)
                    preEdits.push_back({bone, pre, post});
            };
            std::vector<float> height(frames), bend(frames), lift(frames), back(frames), tilt(frames), legs(frames);
            std::vector<float> feet(frames), thighs(frames), knees(frames);
            // Gene's pose: the torso (bone 1), upper arms (13 left, 7 right), their helpers (12, 6: half), forearms (14, 8).
            constexpr int kPoseBones[7] = {1, 13, 12, 7, 6, 14, 8};
            std::vector<mot::Mat> pose[7];
            std::vector<mot::Mat> twist; // the left forearm about its length: half on bone 15, half on the hand (16)
            bool anyHeight = false, anyBend = false, anyPose = false, anyTwist = false, anyLift = false, anyLegs = false;
            bool anyFeet = false, anyFold = false;
            for (int f = 0; f < frames; ++f)
            {
                float v[kValues];
                track.at(static_cast<float>(f), v);
                height[f] = track.gene ? v[3] + v[13] : v[0];
                lift[f] = track.gene ? v[13] : 0.0f;
                anyLift = anyLift || lift[f] != 0.0f;
                bend[f] = track.gene ? 0.0f : v[1] * kDegrees;
                anyHeight = anyHeight || height[f] != 0.0f;
                anyBend = anyBend || bend[f] != 0.0f;
                if (!track.gene)
                {
                    back[f] = -v[2];
                    tilt[f] = v[3] * kDegrees;
                    legs[f] = std::clamp(v[4], 0.0f, 1.0f);
                    anyLegs = anyLegs || back[f] != 0.0f || tilt[f] != 0.0f || legs[f] != 0.0f;
                    feet[f] = -v[5];
                    thighs[f] = v[6] * kDegrees;
                    knees[f] = v[7] * kDegrees;
                    anyFeet = anyFeet || feet[f] != 0.0f;
                    anyFold = anyFold || thighs[f] != 0.0f || knees[f] != 0.0f;
                    continue;
                }
                float a[8];
                for (int i = 0; i < 8; ++i)
                {
                    a[i] = v[4 + i] * kDegrees;
                    anyPose = anyPose || a[i] != 0.0f;
                }
                pose[0].push_back(mot::mul(mot::rotZ(a[1]), mot::rotX(a[0])));
                pose[1].push_back(mot::mul(mot::rotZ(a[2]), mot::rotY(a[3])));
                pose[2].push_back(mot::mul(mot::rotZ(a[2] * 0.5f), mot::rotY(a[3] * 0.5f)));
                pose[3].push_back(mot::mul(mot::rotZ(-a[5]), mot::rotY(-a[6])));
                pose[4].push_back(mot::mul(mot::rotZ(-a[5] * 0.5f), mot::rotY(-a[6] * 0.5f)));
                pose[5].push_back(mot::rotY(a[4]));
                pose[6].push_back(mot::rotY(-a[7]));
                twist.push_back(mot::rotX(v[12] * kDegrees * 0.5f));
                anyTwist = anyTwist || v[12] != 0.0f;
            }
            // Whole pre-rotations from the fit's pose lines (the women's chest grab): Gene's arm bones in the pose list
            // replace the fit's own pose over their frames; other bones (the clavicles, the hands, the victim's arm) get
            // their own pre-rotation, none outside their frames.
            std::vector<std::pair<int, std::vector<mot::Mat>>> extra;
            std::vector<FloatPose> floats;
            if (posed)
                floats = floatPoses(fit, track.motion, track.gene, m, money);
            if (posed)
                for (PoseOverride &o : poseOverrides(fit, track.motion, track.gene, frames, money))
                {
                    int slot = -1;
                    for (int i = 0; i < 7 && track.gene; ++i)
                        if (kPoseBones[i] == o.bone)
                            slot = i;
                    if (slot < 0)
                    {
                        extra.emplace_back(o.bone, std::move(o.pre));
                        continue;
                    }
                    for (int f = o.first; f <= o.last; ++f)
                        pose[slot][f] = o.pre[f];
                    anyPose = true;
                }
            const Fit::CamPath *camera = track.gene && fit.frontCam ? fit.cam(track.motion, g_pair.camVariant) : nullptr;
            if (!anyHeight && !anyBend && !anyPose && !anyTwist && !anyLegs && !anyFeet && !anyFold && extra.empty() && !camera)
            {
                g_copies[key] = 0u;
                return 0u;
            }
            if (anyHeight)
                addCurve(0, 1, height);
            if (anyLift)
                for (int bone : {20, 24}) // the leg IK targets (model space) go up with the hips: the legs keep their shape
                    addCurve(bone, 1, lift);
            if (anyBend)
                addCurve(1, 3, bend);
            if (anyLegs)
            {
                // The victim's hips back (bone 0 z), the pelvis tipped (17 about x) and the knees together: the thighs'
                // outward turn (18 / 22 about y) taken away and the ankles' IK targets (20 / 24 x) moved toward the middle.
                std::vector<mot::Rec> recs;
                mot::parse(m, recs);
                auto scaled = [&](int bone, int component, float k) {
                    std::vector<float> d(frames, 0.0f);
                    for (const mot::Rec &r : recs)
                        if (r.bone == bone && (r.channel & 0x0F) == component)
                            for (int f = 0; f < frames; ++f)
                                d[f] = -k * legs[f] * mot::sample(r, static_cast<float>(f));
                    return d;
                };
                std::vector<float> thighs[2] = {scaled(18, 4, 1.0f), scaled(22, 4, 1.0f)};
                std::vector<float> ankles[2] = {scaled(20, 0, kFeetIn), scaled(24, 0, kFeetIn)};
                addCurve(0, 2, back);
                addCurve(17, 3, tilt);
                addCurve(18, 4, thighs[0]);
                addCurve(20, 0, ankles[0]);
                addCurve(22, 4, thighs[1]);
                addCurve(24, 0, ankles[1]);
            }
            if (anyFeet)
            {
                // The feet back: the ankles' IK targets (20 / 24 z, model space). Only on motions that move them (the legs
                // by IK); a curve added to a motion without one would be a new channel.
                std::vector<mot::Rec> recs;
                mot::parse(m, recs);
                for (int bone : {20, 24})
                    if (std::any_of(recs.begin(), recs.end(),
                                    [&](const mot::Rec &r) { return r.bone == bone && (r.channel & 0x0F) == 2; }))
                        addCurve(bone, 2, feet);
            }
            if (anyFold)
            {
                // Motions with the legs by rotation (no IK): the thighs (18 / 22) and knees (19 / 23) about x.
                for (int bone : {18, 22})
                    addCurve(bone, 3, thighs);
                for (int bone : {19, 23})
                    addCurve(bone, 3, knees);
            }
            if (anyPose)
                for (int i = 0; i < 7; ++i)
                    preRotate(kPoseBones[i], pose[i]);
            if (anyTwist)
                for (int bone : {15, 16})
                    preRotate(bone, {}, twist);
            for (const auto &[bone, pre] : extra)
                preRotate(bone, pre);
            if (camera)
                setCamera(m, frames, *camera);
            const uint32_t size = (static_cast<uint32_t>(m.size()) + 0x3Fu) & ~0x3Fu;
            if (size > kRewriteAreaEnd - kRewriteArea)
                return 0u;
            if (g_areaAt + size > kRewriteAreaEnd)
            {
                g_copies.clear(); // start over (the motions of an earlier pair are not playing any more)
                g_floatPoses.clear();
                g_sampledAt.clear();
                g_areaAt = kRewriteArea;
            }
            const uint32_t at = g_areaAt;
            std::memcpy(rdram + at, m.data(), m.size());
            g_areaAt += size;
            g_copies[key] = at;
            if (posed)
            {
                FloatSet set = editedFloats(ownRecs, frames, curveEdits, preEdits);
                for (FloatPose &f : floats)
                    set.poses.push_back(std::move(f));
                if (!set.poses.empty() || !set.channels.empty())
                    g_floatPoses[at] = std::move(set);
            }
            return at;
        }

        // A rotation as a unit quaternion (w, x, y, z) and back (row-major, column vectors).
        std::array<float, 4> toQuat(const mot::Mat &r)
        {
            std::array<float, 4> q{};
            const float t = r[0] + r[4] + r[8];
            if (t > 0.0f)
            {
                const float s = std::sqrt(t + 1.0f) * 2.0f;
                q = {0.25f * s, (r[7] - r[5]) / s, (r[2] - r[6]) / s, (r[3] - r[1]) / s};
            }
            else if (r[0] > r[4] && r[0] > r[8])
            {
                const float s = std::sqrt(1.0f + r[0] - r[4] - r[8]) * 2.0f;
                q = {(r[7] - r[5]) / s, 0.25f * s, (r[1] + r[3]) / s, (r[2] + r[6]) / s};
            }
            else if (r[4] > r[8])
            {
                const float s = std::sqrt(1.0f + r[4] - r[0] - r[8]) * 2.0f;
                q = {(r[2] - r[6]) / s, (r[1] + r[3]) / s, 0.25f * s, (r[5] + r[7]) / s};
            }
            else
            {
                const float s = std::sqrt(1.0f + r[8] - r[0] - r[4]) * 2.0f;
                q = {(r[3] - r[1]) / s, (r[2] + r[6]) / s, (r[5] + r[7]) / s, 0.25f * s};
            }
            return q;
        }
        mot::Mat fromQuat(const std::array<float, 4> &q)
        {
            const float w = q[0], x = q[1], y = q[2], z = q[3];
            return {1 - 2 * (y * y + z * z), 2 * (x * y - w * z),     2 * (x * z + w * y),
                    2 * (x * y + w * z),     1 - 2 * (x * x + z * z), 2 * (y * z - w * x),
                    2 * (x * z - w * y),     2 * (y * z + w * x),     1 - 2 * (x * x + y * y)};
        }
        mot::Mat slerp(const mot::Mat &a, const mot::Mat &b, float u)
        {
            const auto qa = toQuat(a);
            auto qb = toQuat(b);
            float d = qa[0] * qb[0] + qa[1] * qb[1] + qa[2] * qb[2] + qa[3] * qb[3];
            if (d < 0.0f)
            {
                for (float &c : qb)
                    c = -c;
                d = -d;
            }
            float ka = 1.0f - u, kb = u;
            if (d < 0.9995f)
            {
                const float th = std::acos(std::min(d, 1.0f)), sn = std::sin(th);
                ka = std::sin((1.0f - u) * th) / sn;
                kb = std::sin(u * th) / sn;
            }
            std::array<float, 4> q{};
            float n = 0.0f;
            for (int i = 0; i < 4; ++i)
            {
                q[i] = qa[i] * ka + qb[i] * kb;
                n += q[i] * q[i];
            }
            n = std::sqrt(n);
            for (float &c : q)
                c /= n;
            return fromQuat(q);
        }

        // func_00158328 (a0 = the actor): it samples the actor's motion at +0x430 into its parts, then moveMotion advances
        // +0x430. Kept only for actors playing a copy with posed bones.
        void onMotionSample(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (g_floatPoses.empty())
                return;
            const uint32_t actor = GPR_U32(ctx, 4) & PS2_RAM_MASK;
            if (!inRam(actor, 0x700u) || !g_floatPoses.count(rd32(rdram, actor + kMotionPlaying) & PS2_RAM_MASK))
                return;
            g_sampledAt[actor] = rdF(rdram, actor + kMotionFrame);
        }

        // cModel_calcParts (a0 = the model; it builds each part's matrices from +0x100): the posed bones' sampled
        // rotations replaced by the float ones at the sampled time.
        void onCalcParts(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (g_floatPoses.empty())
                return;
            const uint32_t model = GPR_U32(ctx, 4) & PS2_RAM_MASK;
            if (!inRam(model, 0x700u))
                return;
            const auto it = g_floatPoses.find(rd32(rdram, model + kMotionPlaying) & PS2_RAM_MASK);
            const auto at = g_sampledAt.find(model);
            if (it == g_floatPoses.end() || at == g_sampledAt.end())
                return;
            uint32_t parts[64];
            const int n = readParts(rdram, model, parts);
            const float t = at->second;
            for (const FloatChannel &c : it->second.channels)
            {
                if (c.bone >= n || c.values.empty() || !(t >= 0.0f))
                    continue;
                const float x = 2.0f * t;
                const int last = static_cast<int>(c.values.size()) - 1;
                const int i = std::min(static_cast<int>(x), last), j = std::min(i + 1, last);
                const float v = c.values[i] + (c.values[j] - c.values[i]) * (x - static_cast<float>(static_cast<int>(x)));
                if (c.component >= 3)
                {
                    const uint32_t a = parts[c.bone] + kPartRotation + 4u * static_cast<uint32_t>(c.component - 3);
                    wrF(rdram, a, mot::nearAngle(v, rdF(rdram, a)));
                }
                else if (const uint32_t anchor = rd32(rdram, parts[c.bone] + kPartAnchor) & PS2_RAM_MASK; inRam(anchor, 16u))
                    wrF(rdram, anchor + 4u * static_cast<uint32_t>(c.component), v);
            }
            for (const FloatPose &p : it->second.poses)
            {
                const auto &k = p.keys;
                if (p.bone >= n || !(t >= k.front().first) || t > k.back().first)
                    continue;
                size_t i = 0;
                while (i + 1 < k.size() && k[i + 1].first <= t)
                    ++i;
                const float span = i + 1 < k.size() ? k[i + 1].first - k[i].first : 0.0f;
                const float u = span > 0.0f ? (t - k[i].first) / span : 0.0f;
                const mot::Mat r = u > 0.0f ? slerp(k[i].second, k[i + 1].second, u) : k[i].second;
                float e[3];
                mot::toEuler(r, e[0], e[1], e[2]);
                const uint32_t rotAt = parts[p.bone] + kPartRotation;
                for (int c = 0; c < 3; ++c)
                    wrF(rdram, rotAt + 4u * static_cast<uint32_t>(c), mot::nearAngle(e[c], rdF(rdram, rotAt + 4u * static_cast<uint32_t>(c))));
            }
        }

        // func_002A8578 (a0 = the actor, a1 = its motion, a2 = the motion's events): the pair's punish motions start
        // from their rewritten copies.
        void onStartMotion(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            const uint32_t actor = GPR_U32(ctx, 4) & PS2_RAM_MASK, motion = GPR_U32(ctx, 5) & PS2_RAM_MASK;
            if (!inRam(actor, 0x700u) || !inRam(motion, 8u))
                return;
            const uint32_t gene = player(rdram);
            if (!inRam(gene, 0x700u))
                return;
            const bool isGene = actor == gene;
            const uint32_t enemy = isGene ? rd32(rdram, gene + kCaught) & PS2_RAM_MASK : actor;
            if (!inRam(enemy, 0x700u) || (!isGene && (rd32(rdram, gene + kCaught) & PS2_RAM_MASK) != actor))
                return;
            for (int p = 0; p < kPunishCount; ++p)
            {
                const uint16_t *list = isGene ? kMotions[p].gene : kMotions[p].victim;
                for (int i = 0; i < 4 && list[i]; ++i)
                {
                    if (entry(rdram, enemy, list[i]) != motion)
                        continue;
                    if (!pairOn(enemy, p, i == 0))
                        return; // Buck Breaker off for this punish: the game's motion as it is
                    const uint32_t body = pairBody(rdram, enemy, p);
                    int sizeKey = 100;
                    const Fit *fit = fitNow(rdram, enemy, p, sizeKey);
                    const Track *track = fit ? fit->find(list[i], isGene) : nullptr;
                    uint32_t started = motion;
                    if (track)
                        if (const uint32_t copy = rewrite(rdram, motion, *track, *fit, body, p, sizeKey))
                        {
                            started = copy;
                            SET_GPR_U32(ctx, 5, copy);
                        }
                    if (isGene && g_pair.live && g_pair.enemy == enemy && g_pair.punish == p)
                    {
                        g_pair.geneTrack = track;
                        g_pair.geneMotionPtr = started;
                    }
                    return;
                }
            }
        }

        // Save states (K, Nov 8: a state loaded after playing other punishes played wrong motions in wrong places). The
        // copies live in guest RAM, which a load puts back as it was when the state was saved, so the copy list made
        // since pointed at whatever the state holds there. A state keeps the area's fill point and the pair in play (a
        // state saved mid-punish goes on with it); after every load the list starts empty and each copy is made again
        // past the state's own. A state without them (older builds): the area from its start, no pair.
        struct SavedPair
        {
            int32_t punish = -1, fitIndex = -1, sizeKey = 100;
            uint32_t gene = 0, enemy = 0, body = 0, geneMotionPtr = 0;
            float baseScale[3] = {}, placeScale[3] = {}, applied[3] = {};
            uint16_t trackMotion = 0;
            uint8_t live = 0, hasTrack = 0;
        };
        bool g_stateLoaded = false; // the state carried ours (until the load's end)

        void saveState(ps2x::state::Writer &out)
        {
            uint32_t version = 1u, areaAt = g_areaAt;
            SavedPair s;
            s.punish = g_pair.punish;
            s.gene = g_pair.gene;
            s.enemy = g_pair.enemy;
            s.body = g_pair.body;
            s.sizeKey = g_pair.sizeKey;
            s.geneMotionPtr = g_pair.geneMotionPtr;
            std::memcpy(s.baseScale, g_pair.baseScale, sizeof(s.baseScale));
            std::memcpy(s.placeScale, g_pair.placeScale, sizeof(s.placeScale));
            std::memcpy(s.applied, g_pair.applied, sizeof(s.applied));
            s.live = g_pair.live ? 1u : 0u;
            if (const Fits *fits = g_pair.punish >= 0 ? fitsFor(g_pair.punish, g_pair.body) : nullptr)
                for (size_t i = 0; i < fits->size(); ++i)
                    if ((*fits)[i].get() == g_pair.fit)
                        s.fitIndex = static_cast<int32_t>(i);
            if (const Fits *fits = g_pair.punish >= 0 ? ownFitsFor(g_pair.punish, g_pair.body) : nullptr)
                for (size_t i = 0; i < fits->size(); ++i)
                    if ((*fits)[i].get() == g_pair.fit)
                        s.fitIndex = 1000 + static_cast<int32_t>(i); // an own fit
            if (g_pair.geneTrack)
            {
                s.hasTrack = 1u;
                s.trackMotion = g_pair.geneTrack->motion;
            }
            ps2x::state::fields(out, version, areaAt, s, g_resize[0], g_resize[1]);
        }

        void loadState(ps2x::state::Reader &in)
        {
            uint32_t version = 0u, areaAt = kRewriteArea;
            SavedPair s;
            Resize resize[2];
            ps2x::state::fields(in, version, areaAt, s, resize[0], resize[1]);
            g_copies.clear();
            g_floatPoses.clear();
            g_sampledAt.clear();
            g_areaAt = areaAt >= kRewriteArea && areaAt <= kRewriteAreaEnd ? areaAt : kRewriteArea;
            g_pair = Pair{};
            g_pair.punish = s.punish;
            g_pair.gene = s.gene;
            g_pair.enemy = s.enemy;
            g_pair.body = s.body;
            g_pair.sizeKey = s.sizeKey;
            g_pair.geneMotionPtr = s.geneMotionPtr;
            std::memcpy(g_pair.baseScale, s.baseScale, sizeof(s.baseScale));
            std::memcpy(g_pair.placeScale, s.placeScale, sizeof(s.placeScale));
            std::memcpy(g_pair.applied, s.applied, sizeof(s.applied));
            const bool ownFit = s.fitIndex >= 1000;
            const int index = ownFit ? s.fitIndex - 1000 : s.fitIndex;
            const Fits *fits = s.punish >= 0 && s.punish < kPunishCount
                                   ? (ownFit ? ownFitsFor(s.punish, s.body) : fitsFor(s.punish, s.body))
                                   : nullptr;
            const bool fitOk = index < 0 || (fits && static_cast<size_t>(index) < fits->size());
            if (fits && index >= 0 && fitOk)
                g_pair.fit = (*fits)[static_cast<size_t>(index)].get();
            if (g_pair.fit && s.hasTrack)
                g_pair.geneTrack = g_pair.fit->find(s.trackMotion, true);
            g_pair.live = s.live && fitOk && s.punish >= 0 && s.punish < kPunishCount;
            g_resize[0] = resize[0];
            g_resize[1] = resize[1];
            g_stateLoaded = true;
        }

        void afterLoad()
        {
            g_pairOn = g_on.load(std::memory_order_relaxed);
            if (!g_stateLoaded)
            {
                g_copies.clear();
                g_floatPoses.clear();
                g_sampledAt.clear();
                g_areaAt = kRewriteArea;
                g_pair = Pair{};
                g_resize[0] = Resize{};
                g_resize[1] = Resize{};
            }
            g_stateLoaded = false;
        }

        // EE thread, each vblank: Gene follows his placement curve while the punish plays.
        void onVblank()
        {
            if (!g_ram)
                return;
            stepResizes();
            if (!g_pair.live)
                return;
            const uint32_t gene = player(g_ram);
            if (gene != g_pair.gene || !inRam(gene, 0x700u) || g_ram[gene + game::GameObj::kMode] != 0u ||
                g_ram[gene + game::GameObj::kMode + 1u] != kGenePhase[g_pair.punish] ||
                (rd32(g_ram, gene + kCaught) & PS2_RAM_MASK) != g_pair.enemy)
            {
                g_pair.live = false;
                return;
            }
            if (!g_pair.geneTrack || (rd32(g_ram, gene + kMotionPlaying) & PS2_RAM_MASK) != g_pair.geneMotionPtr)
                return;
            float v[kValues];
            g_pair.geneTrack->at(rdF(g_ram, gene + kMotionFrame), v);
            const float want[3] = {v[0], v[1], v[2] * kDegrees};
            if (want[0] == g_pair.applied[0] && want[1] == g_pair.applied[1] && want[2] == g_pair.applied[2])
                return;
            const float facing = wrap(rdF(g_ram, gene + 0x104u) - g_pair.applied[2]);
            moveGene(g_ram, gene, g_pair.placeScale, want[1] - g_pair.applied[1], want[0] - g_pair.applied[0],
                     want[2] - g_pair.applied[2], facing);
            std::memcpy(g_pair.applied, want, sizeof(want));
        }

        // punish_fit.txt:
        //   fit <Suplex|Stinger|CobraTwist|Spanking|GorillaSuplex> <body hex>[,<body hex>...] [off] [gorilla] [own] [frontcam]
        //       [node=<name>] [bust=detail] [scale=<s>] [size=<s>]   (the lines after it, until the next fit, are its;
        //       gorilla on a Suplex fit: that body plays the Gorilla Suplex for a Suplex pick; frontcam: the punish camera
        //       from the fit's cam lines; node: these fits replace the body's for a Buck Breaker pick on a model with a
        //       mesh node whose name starts with it, e.g. MOHI = the mohawk women; bust=detail: the same for a model drawn
        //       with the detailed bust; scale: the enemy size it is for, 1 if not given)
        //   gene <motion> <frame>:<forward>,<side>,<turn>,<height>[,<torso x>,<torso z>,<left raise>,<left swing>,
        //       <left elbow>,<right raise>,<right swing>,<right elbow>[,<left forearm twist>[,<lift>]]] ...
        //   victim <motion> <frame>:<height>,<bend>[,<hips back>,<pelvis tilt>,<legs>[,<feet back>,<thigh pitch>,<knee bend>]] ...
        //   pose <gene|victim> <motion> <money> <bone> <frame>:<x>,<y>,<z> ...   (the bone's whole pre-rotation over those
        //       frames, a rotation vector in degrees; money 0 = any, else the Money value it was solved at)
        //   bust <gene motion> <frame>:<hold> ...   (the caught victim's bust jiggle held, 0..1, by Gene's frame)
        //   cam <gene motion> <variant> <frame>:<eye x>,<y>,<z>,<look-at x>,<y>,<z> ...   (frontcam: the punish camera's parts
        //       27 / 28 in Gene's model space; one variant picked at random per punish, never the same twice running)
        //   body <body hex>[,<body hex>...] <type>   (the enemy labels' body type)
        void load()
        {
            std::error_code error;
            const fs::path &file = fs::exists(g_file, error) ? g_file : g_shippedFile;
            std::ifstream in(file);
            std::string line;
            Fit *fit = nullptr;
            size_t count = 0;
            while (std::getline(in, line))
            {
                if (!line.empty() && line.back() == '\r')
                    line.pop_back();
                if (line.empty() || line[0] == '#')
                    continue;
                std::istringstream fields(line);
                std::string kind;
                fields >> kind;
                if (kind == "fit")
                {
                    std::string name, bodies, token;
                    fields >> name >> bodies;
                    int punish = -1;
                    for (int i = 0; i < kPunishCount; ++i)
                        if (name == kFileNames[i])
                            punish = i;
                    auto made = std::make_shared<Fit>();
                    while (fields >> token && token[0] != '#')
                    {
                        if (token == "off")
                            made->off = true;
                        else if (token == "gorilla")
                            made->gorilla = true;
                        else if (token == "own")
                            made->own = true;
                        else if (token == "frontcam")
                            made->frontCam = true;
                        else if (token.rfind("node=", 0) == 0)
                            made->node = token.substr(5, 8);
                        else if (token == "bust=detail")
                            made->node = kDetailBust;
                        else if (token.rfind("scale=", 0) == 0)
                            made->scale = std::strtof(token.c_str() + 6, nullptr);
                        else if (token.rfind("size=", 0) == 0)
                            made->size = std::clamp(std::strtof(token.c_str() + 5, nullptr), 0.9f, 1.1f);
                    }
                    fit = nullptr;
                    std::istringstream keys(bodies);
                    std::string body;
                    while (punish >= 0 && std::getline(keys, body, ','))
                        if (uint32_t key = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 16)))
                        {
                            if (!made->node.empty())
                            {
                                // The variant's own key (FNV-1a of the body and the node name).
                                uint32_t h = 2166136261u;
                                for (int i = 0; i < 4; ++i)
                                    h = (h ^ ((key >> (8 * i)) & 0xFFu)) * 16777619u;
                                for (const char c : made->node)
                                    h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
                                h = h ? h : 1u;
                                auto &list = g_variants[fitKey(punish, key)];
                                if (std::none_of(list.begin(), list.end(), [&](const auto &v) { return v.second == h; }))
                                    list.emplace_back(made->node, h);
                                g_variantOf[h] = {key, made->node};
                                key = h;
                            }
                            (made->own ? g_ownFits : g_fits)[fitKey(punish, key)].push_back(made);
                            fit = made.get();
                        }
                    count += fit != nullptr;
                }
                else if (kind == "body")
                {
                    std::string bodies, name;
                    fields >> bodies >> name;
                    std::istringstream keys(bodies);
                    std::string body;
                    while (std::getline(keys, body, ','))
                        if (const uint32_t key = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 16)))
                            g_bodyNames[key] = name;
                }
                else if (kind == "core")
                {
                    std::string core, body;
                    fields >> core >> body;
                    const auto key = static_cast<uint32_t>(std::strtoul(core.c_str(), nullptr, 16));
                    const auto to = static_cast<uint32_t>(std::strtoul(body.c_str(), nullptr, 16));
                    if (key && to)
                        g_core[key] = to;
                }
                else if (fit && kind == "pose")
                {
                    // pose <gene|victim> <motion> <money> <bone> <frame>:<x>,<y>,<z> ... (rotation vector, degrees)
                    Pose p;
                    std::string who, item;
                    unsigned motion = 0;
                    fields >> who >> motion >> p.money >> p.bone;
                    p.gene = who == "gene";
                    p.motion = static_cast<uint16_t>(motion);
                    while (fields >> item)
                    {
                        std::array<float, 3> v{};
                        if (std::sscanf(item.c_str(), "%*f:%f,%f,%f", &v[0], &v[1], &v[2]) == 3)
                            p.keys.emplace_back(std::strtof(item.c_str(), nullptr), v);
                    }
                    if (!p.keys.empty() && p.bone > 0 && p.bone < 64)
                        fit->poses.push_back(std::move(p));
                }
                else if (fit && kind == "cam")
                {
                    // cam <gene motion> <variant> <frame>:<eye x>,<y>,<z>,<look-at x>,<y>,<z> ...   (frontcam: the camera)
                    Fit::CamPath path;
                    unsigned motion = 0;
                    std::string item;
                    fields >> motion >> path.variant;
                    path.motion = static_cast<uint16_t>(motion);
                    while (fields >> item)
                    {
                        std::array<float, 6> v{};
                        if (std::sscanf(item.c_str(), "%*f:%f,%f,%f,%f,%f,%f", &v[0], &v[1], &v[2], &v[3], &v[4], &v[5]) == 6)
                            path.keys.emplace_back(std::strtof(item.c_str(), nullptr), v);
                    }
                    if (!path.keys.empty())
                        fit->cams.push_back(std::move(path));
                }
                else if (fit && kind == "bust")
                {
                    // bust <gene motion> <frame>:<hold> ...
                    unsigned motion = 0;
                    std::string item;
                    fields >> motion;
                    fit->bustMotion = static_cast<uint16_t>(motion);
                    while (fields >> item)
                    {
                        const size_t colon = item.find(':');
                        if (colon != std::string::npos)
                            fit->bust.emplace_back(std::strtof(item.c_str(), nullptr), std::strtof(item.c_str() + colon + 1, nullptr));
                    }
                }
                else if (fit && (kind == "gene" || kind == "victim"))
                {
                    Track track;
                    track.gene = kind == "gene";
                    unsigned motion = 0;
                    fields >> motion;
                    track.motion = static_cast<uint16_t>(motion);
                    std::string item;
                    while (fields >> item)
                    {
                        Key k;
                        const size_t colon = item.find(':');
                        if (colon == std::string::npos)
                            continue;
                        k.frame = std::strtof(item.c_str(), nullptr);
                        const char *at = item.c_str() + colon + 1;
                        for (int c = 0; c < kValues && *at; ++c)
                        {
                            char *end = nullptr;
                            k.v[c] = std::strtof(at, &end);
                            at = *end == ',' ? end + 1 : end;
                        }
                        track.keys.push_back(k);
                    }
                    if (!track.keys.empty())
                        fit->tracks.push_back(std::move(track));
                }
            }
            for (auto *map : {&g_fits, &g_ownFits})
                for (auto &[key, fits] : *map)
                    std::stable_sort(fits.begin(), fits.end(), [](const auto &a, const auto &b) { return a->scale < b->scale; });
            KM_INFO(Game, "punish fit: " << count << " fits for " << g_fits.size() << " bodies and punishes from "
                                         << file.filename().string()
                                         << (file == g_shippedFile ? " (shipped)" : " (yours, next to the exe)"));
        }

        bool envOn(const char *name)
        {
            const char *v = std::getenv(name);
            return v && *v && *v != '0';
        }
    }

    void setChanged(uint32_t enemy, bool changed)
    {
        enemy &= PS2_RAM_MASK;
        for (Changed &c : g_changed)
            if (c.enemy == enemy)
            {
                c.changed = changed;
                return;
            }
        g_changed[g_changedNext] = Changed{enemy, changed};
        g_changedNext = (g_changedNext + 1u) % std::size(g_changed);
    }

    bool doesntFit(const uint8_t *rdram, uint32_t enemy, int punish)
    {
        if (!g_on || punish < 0 || punish >= kPunishCount || !inRam(enemy, 0x700u))
            return false;
        enemy &= PS2_RAM_MASK;
        const Fits *fits = fitsFor(punish, variantOf(rdram, enemy, punish, bodyOf(rdram, enemy)));
        if (!fits)
            return false;
        float scale[3];
        ownScale(rdram, enemy, scale);
        return !anchorFor(*fits, scale[1]).within;
    }

    bool suplexAsGorilla(const uint8_t *rdram, uint32_t enemy)
    {
        if (!g_on || !inRam(enemy, 0x700u))
            return false;
        enemy &= PS2_RAM_MASK;
        const Fit *fit = fitFor(Suplex, variantOf(rdram, enemy, Suplex, bodyOf(rdram, enemy)));
        return fit && fit->gorilla;
    }

    bool suplexIsChestGrab(const uint8_t *rdram, uint32_t enemy)
    {
        if (!g_on || !inRam(enemy, 0x700u))
            return false;
        enemy &= PS2_RAM_MASK;
        const Fit *fit = fitFor(Suplex, variantOf(rdram, enemy, Suplex, bodyOf(rdram, enemy)));
        return fit && fit->frontCam && !fit->off;
    }

    float bustHold(const uint8_t *rdram, uint32_t model)
    {
        const Fit *fit = g_pair.fit;
        if (!g_pair.live || !fit || fit->bust.empty() || (model & PS2_RAM_MASK) != g_pair.enemy || !g_pair.geneTrack ||
            g_pair.geneTrack->motion != fit->bustMotion || !inRam(g_pair.gene, 0x700u) ||
            (rd32(rdram, g_pair.gene + kMotionPlaying) & PS2_RAM_MASK) != g_pair.geneMotionPtr)
            return 0.0f;
        const float frame = rdF(rdram, g_pair.gene + kMotionFrame);
        const auto &b = fit->bust;
        if (frame <= b.front().first || frame >= b.back().first)
            return frame <= b.front().first ? b.front().second : b.back().second;
        size_t i = 1;
        while (b[i].first < frame)
            ++i;
        const float t = (frame - b[i - 1].first) / (b[i].first - b[i - 1].first);
        return std::clamp(b[i - 1].second + (b[i].second - b[i - 1].second) * t, 0.0f, 1.0f);
    }

    float seatSlap(const uint8_t *rdram, uint32_t model)
    {
        // Gene's right hand meets the seat at these frames of his Spanking motions (offline, tools/models/punish_fit
        // sim on em16: the hand's nearest pass to the pelvis): the mash loop 1339 at 4, the finish 1341 at 2 and the
        // launching slap at 21. Fitted pairs play rewritten copies with the same timing.
        struct Contact
        {
            uint16_t motion;
            float frame, strength;
        };
        constexpr Contact kContacts[] = {{1339, 4.0f, 1.0f}, {1341, 2.0f, 1.0f}, {1341, 21.0f, 1.5f}};
        static uint32_t s_ptr = 0u;
        static float s_frame = 0.0f;
        model &= PS2_RAM_MASK;
        const uint32_t gene = player(rdram);
        if (!inRam(gene, 0x700u) || (rd32(rdram, gene + kCaught) & PS2_RAM_MASK) != model || !inRam(model, 0x700u))
            return 0.0f;
        const uint32_t ptr = rd32(rdram, gene + kMotionPlaying) & PS2_RAM_MASK;
        uint16_t number = 0;
        if (ptr == entry(rdram, model, 1339))
            number = 1339;
        else if (ptr == entry(rdram, model, 1341))
            number = 1341;
        else if (g_pair.live && g_pair.enemy == model && g_pair.punish == Spanking && g_pair.geneTrack &&
                 ptr == g_pair.geneMotionPtr)
            number = g_pair.geneTrack->motion;
        if (number != 1339 && number != 1341)
        {
            s_ptr = 0u;
            return 0.0f;
        }
        const float frame = rdF(rdram, gene + kMotionFrame);
        // A motion seen for the first time counts from where it is (no slap for frames already past); a loop back to
        // its start counts from before 0.
        const float prev = ptr != s_ptr ? frame : (frame < s_frame ? -1.0f : s_frame);
        s_ptr = ptr;
        s_frame = frame;
        float hit = 0.0f;
        for (const Contact &c : kContacts)
            if (c.motion == number && prev < c.frame && frame >= c.frame)
                hit = std::max(hit, c.strength);
        return hit;
    }

    void describe(const uint8_t *rdram, uint32_t enemy, std::string &label, std::string &status)
    {
        label.clear();
        status.clear();
        enemy &= PS2_RAM_MASK;
        if (!inRam(enemy, 0x700u))
            return;
        float scale[3];
        ownScale(rdram, enemy, scale);
        const uint32_t body = bodyOf(rdram, enemy);
        const auto name = g_bodyNames.find(body);
        char text[64];
        std::snprintf(text, sizeof(text), "%s | %s | %.2f", enemyName(rdram, enemy).c_str(),
                      name != g_bodyNames.end() ? name->second.c_str() : "?", scale[1]);
        label = text;
        // In Gene's punish: which one, and what the fit does.
        const uint32_t gene = player(rdram);
        if (!inRam(gene, 0x700u) || (rd32(rdram, gene + kCaught) & PS2_RAM_MASK) != enemy || rdram[gene + game::GameObj::kMode] != 0u)
            return;
        const uint8_t phase = rdram[gene + game::GameObj::kMode + 1u];
        for (int p = 0; p < kPunishCount; ++p)
            if (phase == kGenePhase[p])
            {
                const Fits *fits = g_on ? fitsForPair(rdram, enemy, p, pairBody(rdram, enemy, p)) : nullptr;
                const Anchor an = fits ? anchorFor(*fits, scale[1]) : Anchor{};
                char what[96];
                if (!an.any)
                    std::snprintf(what, sizeof(what), "%s", fits ? "doesn't fit" : "no fit (the game's placement)");
                else
                    std::snprintf(what, sizeof(what), "%s %.2f, pair x%.3f%s", an.fit ? "fit" : "game's own at", an.scale,
                                  an.k, an.within ? "" : " (held at the cap)");
                status = std::string(kNames[p]) + ": " + what;
                return;
            }
    }

    void install(PS2Runtime &runtime, const fs::path &exeDir)
    {
        g_file = exeDir / "punish_fit.txt";
        g_shippedFile = kamite::paths::resources() / "punish_fit.txt";
        // Installed always and switched live with Buck Breaker (K, Nov 13); off, every punish is the game's own.
        g_ram = runtime.memory().getRDRAM();
        if (!g_ram || !runtime.hasFunction(kAlign) || !(g_align = runtime.lookupFunction(kAlign)) ||
            !FunctionHooks::instance().replace(kAlign, "punish placement (Buck Breaker's punish fit)", &alignHook,
                                               ReplacementStatus::Native, "[Game] BuckBreaker: per-body punish fits"))
        {
            KM_WARN(Game, "punish fit: func_001299F0 is not hookable; no punish fits");
            return;
        }
        g_ready = true;
        g_on = envOn("KAMITE_BUCK_BREAKER");
        if (!FunctionHooks::instance().observeEntry(kStartMotion, "punish fit: motion start", &onStartMotion))
            KM_WARN(Game, "punish fit: func_002A8578 is not hookable; placement only, no heights or bend");
        if (!FunctionHooks::instance().observeEntry(kMotionSample, "punish fit: motion sample time", &onMotionSample) ||
            !FunctionHooks::instance().observeEntry(kCalcParts, "punish fit: posed bones in float", &onCalcParts))
            KM_WARN(Game, "punish fit: func_00158328 / cModel_calcParts not hookable; posed bones play at the motion's 8-bit steps");
        load();
        ps2x::pnach::addFrameHook(&onVblank);
        ps2x::savestate::registerExtension("kamite.punish_fit", &saveState, &loadState);
        ps2x::savestate::onLoaded(&afterLoad);
    }

    void setEnabled(bool on)
    {
        g_on.store(on && g_ready, std::memory_order_relaxed);
    }
}
