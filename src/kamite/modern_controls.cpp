// Control Style: Classic (the PS2's) or Modern, which is Toggle or (the controller only) Hold.
//
// Modern: the right stick turns a free camera round Gene instead of dodging, and the left stick moves him relative to
// the camera (he faces where he walks). The right stick press (R3) switches between that and the original controls
// (tank movement, right stick = dodges, the camera behind Gene) (K, Nov 3: on the camera's own stick; was Select). The
// switch is on the release, and only when the stick stayed near the centre while pressed (a hard dodge flick that
// clicks the stick does not switch). In Modern the game never sees R3: Gene's shared cancel check func_00123938 sends
// an R3 press in a move's cancel window (seqFlags 0x10) to mode 0 state 4 (func_0010CB10, a short motion), which a
// switch would otherwise fire. The choice is kept across rooms, doors and loads; a launch starts in the free camera.
// With Auto, contact starts brawl mode: Gene's attack (or something he threw) hurting an enemy, or anything hurting
// Gene (the two damage steps, debug_tools' hooks call noteContact). Not the battle camera: it also comes on for
// crates and throws (K, Nov 3); not distance or enemy intent (K: free camera must still get you out of a fight).
// Nothing switches back: R3 returns to the free camera. L1 is always the game's own quick turn; in the free camera the camera swings back
// behind Gene with him. Whenever
// the game takes the camera (a close-up for a grab or a special move, a cutscene, a menu: no player camera update for
// a moment), everything is Classic for that moment and the choice comes back after it.
//
// Modern: Hold (K, Nov 19: the alpha's Hold to Brawl scheme back, [Game] ControlStyle=hold; the controller only, the
// keyboard has its Dodge Modifier): brawl mode while L1 is held; a second L1 press within 300 ms is the game's quick
// turn (L1 reaches the game only then, for 90 ms) and the camera stays behind Gene through it. R3 does nothing unless
// the Button Mapping page gave it the single-press turn ([PadN] R3QuickTurn, off by default: it travels with the PS2
// R3 input, so the turn is wherever R3's slot is mapped); not while the stick is pushed out (a flick that clicked).
// Auto starts brawl mode on contact as in Toggle, and with no switch to leave it, it also ends it: once the battle
// camera is off and no live enemy has been within 3 m for 1.5 s (the alpha's fight rule).
// Hold to Cam (K, Nov 20; [Game] HoldMode=cam, Hold's other mode is Hold to Brawl): the other way round, brawl mode
// unless L1 is held: held past kTapMs it is the free camera until let go; a shorter press is the game's quick turn,
// sent when L1 is let go. Auto means nothing there (brawl mode is already the rest state) and is not used.
//
// Input side (window thread, the pad filter ps2_host_input calls for player 1): L1 and the right stick are taken from
// what the game sees; the left stick is turned into the stick the original controls would need for the same
// direction (relative to where Gene faces). Camera side (game thread, camera.cpp's yaw hook): the camera's yaw round
// Gene and an extra pitch, eased back to the original camera when the original controls are in effect; while Gene
// stands or walks, his yaw (+0x104) is also turned toward the stick's direction directly (up to 900 degrees a
// second), so he does not have to wait for the original turning speed.
//
// Mouse ([Game] MouseCamera, on by default): the runtime's mouse camera captures the mouse on a click in the game
// window (that click is not passed on; Esc or losing the focus frees it, and so does the F2 menu); its movement turns
// the free camera at MouseSensitivity degrees per pixel. The mouse never reaches the game as a right stick, so it
// cannot dodge; while the original controls are in effect it does nothing.
//
// Per device (K, Nov 16): the controller and the keyboard & mouse each have their own Control Style and Auto ([Game]
// ControlStyle / ModernAuto and KbmControlStyle / KbmModernAuto); the one in effect is the device used last (the button
// prompts' detector: a bound key, mouse button, controller button or stick past half; never mouse movement), and each
// device comes back to the free camera or brawl mode it left. Switching to the keyboard says its style in a short
// notice when it differs from the controller's. The keyboard's Dodge Modifier (Shift + the move keys) reaches the game
// as the right stick even in the free camera (the mouse is that camera).
//
// Directions: the camera's yaw is in Gene's convention (the original camera's yaw is his + the offset at +0x414),
// "right" on the stick is the direction a camera turned right by the stick would face (yaw decreasing).
#include "kamite/modern_controls.h"

#include "kamite/hotkeys.h"
#include "kamite/button_prompts.h"
#include "kamite/jukebox.h"
#include "kamite/frame_patches.h"
#include "kamite/prompt_hd.h"
#include "kamite/camera.h"
#include "kamite/ini_file.h"
#include "kamite/log.h"
#include "kamite/menu_buttons.h"
#include "kamite/style_presets.h"
#include "kamite/test_mode.h"
#include "kamite/game_layout.h"
#include "ps2_host_input.h"
#include "runtime/ps2_game_menu.h"
#include "runtime/ps2_input_options.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <string>

#if __has_include("pcsx2gs_api.h")
#include "pcsx2gs_api.h"
#define KAMITE_HAS_GS_MENU 1
#endif

namespace kamite::modern
{
    namespace
    {
        constexpr float kPi = 3.14159265f;
        constexpr float kDegToRad = kPi / 180.0f;
        constexpr int64_t kNoCameraMs = 150;   // no player camera update for this long: the game has the camera
        constexpr float kStickDeadzone = 0.18f;
        constexpr float kSwitchMaxStick = 0.5f; // an R3 press with the right stick further out than this: a flick, no switch
        constexpr float kPitchMin = -1.2f, kPitchMax = 0.9f; // extra pitch, radians (the floor is camera.cpp's: above the ground)
        constexpr int64_t kQuickTurnFollowMs = 650;            // the camera stays behind Gene this long after one
        constexpr int64_t kDoubleTapMs = 300;                  // Hold: a second L1 press within this is the quick turn
        constexpr int64_t kQuickTurnMs = 90;                   // ... and L1 is held this long for the game
        constexpr int64_t kTapMs = 200;                        // Hold to Cam: a shorter L1 press is the quick turn
        constexpr float kFightRange = 3.0f;                    // Hold's Auto: still in the fight while an enemy is this close
        constexpr int64_t kFightLingerMs = 1500;               // ... or was, this long ago
        constexpr uint32_t kEnemyList = 0x5864F0u + 0x500u;    // cEmManage D_005864F0: the first used slot
        constexpr float kTurnRate = 900.0f * (3.14159265f / 180.0f); // Gene turned toward the stick, radians a second
        constexpr uint32_t kPlayerPointer = game::GameWork::address(game::GameWork::kPlayerObj); // GameWork.playerObj (Getplayer)

        // Settings (F2 menu on the GS thread, read by the window and game threads): the controller's, the keyboard's.
        std::atomic<bool> g_modern{false};
        std::atomic<bool> g_hold{false};        // the controller's Modern is Hold (with g_modern)
        std::atomic<bool> g_holdCam{false};     // ... Hold to Cam (else Hold to Brawl)
        std::atomic<bool> g_auto{true};
        std::atomic<bool> g_kbmModern{true};
        std::atomic<bool> g_kbmAuto{false};
        std::atomic<float> g_yawSpeed{200.0f};  // degrees a second at full stick
        std::atomic<float> g_pitchSpeed{80.0f};
        std::atomic<bool> g_invertX{false};
        std::atomic<bool> g_invertY{false};
        std::atomic<bool> g_mouseCamera{true};
        std::atomic<float> g_mouseSensitivity{0.1f}; // degrees per pixel

        // Camera side -> input side.
        std::atomic<bool> g_battleCamera{false};
        std::atomic<bool> g_gameCamera{false};
        std::atomic<int64_t> g_lastCameraMs{0};
        std::atomic<bool> g_free{false};        // the free camera is on (not easing back to the original)
        std::atomic<float> g_cameraYaw{0.0f};   // the free camera's yaw
        std::atomic<float> g_geneYaw{0.0f};

        // Input side -> camera side. Per device: [0] the controller, [1] the keyboard and mouse.
        std::atomic<bool> g_classic[2]{};       // the player's choice (R3, or Auto at a fight's start): original controls
        std::atomic<bool> g_classicByAuto[2]{}; // ... set by Auto (for the F2 status line; Hold's Auto ends it)
        std::atomic<bool> g_holdL1{false};      // Hold: brawl mode by L1 (Hold to Brawl: L1 held; Hold to Cam: L1 not held long)
        std::atomic<float> g_stickX{0.0f}, g_stickY{0.0f};
        std::atomic<bool> g_moving{false};      // the left stick asks for a direction (Modern movement)
        // The game has Gene's controls off (0x747A78: 0x02000000 KeyStop, 0x80000000 / 0x40000000 player off): menus
        // in the room (the fight ring's sign-up, T2's case Nov 4: KeyStop from the talk on), talks, events. The left
        // stick then goes to the game as it is: turned to the camera it read as other directions as Gene turned.
        // (A case's pad section is recorded after this filter: T2's shows the turned stick sweeping down -> left -> up
        // for one push up, so its replay keeps the old behaviour; only live play shows the fix.)
        std::atomic<bool> g_controlsOff{false};
        constexpr uint32_t kControlWord = 0x747A78u, kControlsOffBits = 0xC2000000u;
        std::atomic<float> g_moveYaw{0.0f};     // that direction (world yaw)
        std::atomic<uint32_t> g_recenter{0};    // bumped by each quick turn
        std::atomic<float> g_pitchFloor{-1.2f}; // the lowest extra pitch that keeps the camera above the ground

        // Input side only (window thread).
        bool g_l1Was = false;
        int64_t g_l1PressMs = -100000;   // Hold: the last L1 press that was not the second of a double tap (Hold to
                                         // Cam: the last press; -100000 = held since the game had the camera)
        int64_t g_quickTurnUntilMs = 0;  // Hold: L1 reaches the game until then (the quick turn)
        bool g_r3Was = false;
        bool g_r3Stray = false; // the right stick left the centre during this press: not a switch

        int64_t nowMs()
        {
            using namespace std::chrono;
            return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
        }

        // The device in effect: 1 = the keyboard and mouse were used last, 0 = a controller.
        int device()
        {
            return ps2_host_input::prompts::lastDevice() == ps2_host_input::prompts::Device::Keys ? 1 : 0;
        }
        bool modernNow() { return (device() ? g_kbmModern : g_modern).load(std::memory_order_relaxed); }
        // Modern: Hold is in effect (the controller's style; the keyboard has no Hold).
        bool holdNow()
        {
            return device() == 0 && g_modern.load(std::memory_order_relaxed) && g_hold.load(std::memory_order_relaxed);
        }
        // Hold to Cam is in effect (brawl mode unless L1 is held).
        bool holdCamNow() { return holdNow() && g_holdCam.load(std::memory_order_relaxed); }
        // Auto in effect: not under Hold to Cam (brawl mode is its rest state already).
        bool autoNow() { return device() ? g_kbmAuto.load(std::memory_order_relaxed) : g_auto.load(std::memory_order_relaxed) && !holdCamNow(); }

        bool gameHasCamera()
        {
            return g_gameCamera.load(std::memory_order_relaxed) ||
                   nowMs() - g_lastCameraMs.load(std::memory_order_relaxed) > kNoCameraMs;
        }

        // The original controls are in effect (besides the game having the camera).
        bool classicByChoice()
        {
            return g_classic[device()].load(std::memory_order_relaxed) ||
                   (holdNow() && g_holdL1.load(std::memory_order_relaxed));
        }

        float stickAxis(uint8_t value)
        {
            float axis = (static_cast<float>(value) - 128.0f) / 127.0f;
            axis = std::fmax(-1.0f, std::fmin(1.0f, axis));
            const float magnitude = std::fabs(axis);
            if (magnitude < kStickDeadzone)
                return 0.0f;
            return std::copysign((magnitude - kStickDeadzone) / (1.0f - kStickDeadzone), axis);
        }

        float wrapAngle(float angle)
        {
            while (angle > kPi)
                angle -= 2.0f * kPi;
            while (angle < -kPi)
                angle += 2.0f * kPi;
            return angle;
        }

        // Window thread, every sample: switching to the keyboard says its style when it differs from the controller's
        // (K, Nov 16: a line in the top-left corner for 3 s; switching to a controller is silent).
        void noteDevice()
        {
            static int last = -1;
            const int d = device();
            if (d == last)
                return;
            const bool first = last < 0;
            last = d;
            if (first || d != 1 || g_kbmModern.load(std::memory_order_relaxed) == g_modern.load(std::memory_order_relaxed))
                return;
            if (!g_kbmModern.load(std::memory_order_relaxed))
            {
                ps2x::menu::notify("KBM: Classic Controls", 3.0f);
                return;
            }
            // The key or button brawl mode is on (R3's first keyboard / mouse binding).
            const ps2_host_input::prompts::Binding r3 = ps2_host_input::prompts::snapshot().keys[ps2_host_input::prompts::R3];
            std::string key;
            if (r3.type == ps2_host_input::prompts::Type::Mouse && r3.code == 2 /* MOUSE_BUTTON_MIDDLE */)
                key = "Middle-click";
            else if (r3.type != ps2_host_input::prompts::Type::None)
                key = r3.label;
            ps2x::menu::notify(key.empty() ? std::string("KBM: Modern") : "KBM: [" + key + "] Brawl/Freecam", 3.0f);
        }

        // Modern: Hold's buttons (the pad filter, window thread, outside the game's camera moments): L1 held = brawl
        // mode, a second press within kDoubleTapMs = the game's quick turn; R3 the single-press turn when its slot has
        // it (not when the stick was out: a flick that clicked). L1 reaches the game only as the quick turn.
        // Hold to Cam: L1 held past kTapMs = the free camera, a shorter press = the quick turn on its release.
        void holdInput(ps2x::iop::PadInputState &state, bool l1, bool r3, bool stickOut)
        {
            // Toggle's R3 latch has no way out in Hold: only Auto's brawl mode stays (it ends by itself; none in Hold to
            // Cam).
            if (g_classic[0].load(std::memory_order_relaxed) &&
                (!g_classicByAuto[0].load(std::memory_order_relaxed) || holdCamNow()))
                g_classic[0] = false;
            const int64_t now = nowMs();
            if (holdCamNow())
            {
                if (l1 && !g_l1Was)
                    g_l1PressMs = now;
                else if (!l1 && g_l1Was && now - g_l1PressMs < kTapMs)
                {
                    g_quickTurnUntilMs = now + kQuickTurnMs;
                    g_recenter.fetch_add(1u, std::memory_order_relaxed);
                }
                g_holdL1 = !(l1 && now - g_l1PressMs >= kTapMs);
            }
            else if (l1 && !g_l1Was)
            {
                if (now - g_l1PressMs <= kDoubleTapMs)
                {
                    g_quickTurnUntilMs = now + kQuickTurnMs;
                    g_recenter.fetch_add(1u, std::memory_order_relaxed);
                    g_l1PressMs = -100000;
                }
                else
                    g_l1PressMs = now;
            }
            if (!holdCamNow())
                g_holdL1 = l1;
            g_l1Was = l1;
            if (r3 && !g_r3Was && !stickOut && ps2_host_input::extra::state().r3QuickTurn)
            {
                g_quickTurnUntilMs = now + kQuickTurnMs;
                g_recenter.fetch_add(1u, std::memory_order_relaxed);
            }
            g_r3Was = r3;
            g_r3Stray = false;
            constexpr size_t kL1Pressure = static_cast<size_t>(ps2x::iop::PadPressure::L1);
            const bool turn = now < g_quickTurnUntilMs;
            state.buttons = static_cast<uint16_t>((state.buttons & ~ps2x::iop::PadButton::L1) |
                                                  (turn ? ps2x::iop::PadButton::L1 : 0u));
            state.pressure[kL1Pressure] = turn ? 0xFFu : 0u;
        }

        // ps2_host_input's pad filter (window thread), player 1 only.
        void padFilter(uint32_t player, ps2x::iop::PadInputState &state)
        {
            style_presets::padFilter(player, state); // the d-pad style swaps (ps2_host_input has one filter)
            if (player != 0u)
                return;
            menu_buttons::noteRightStick(state.connected ? state.rightX : 0x80u); // the Techniques screen's Gene turn
            noteDevice();
            if (!modernNow())
                return;
            // The keyboard's Dodge Modifier: the right stick is a dodge for the game, never the camera's.
            const bool keyDodge = ps2_host_input::keyboardDodge();
            const bool r3 = (state.buttons & ps2x::iop::PadButton::R3) != 0u;
            state.buttons = static_cast<uint16_t>(state.buttons & ~ps2x::iop::PadButton::R3); // the switch's, never the game's
            const float rx = (static_cast<float>(state.rightX) - 128.0f) / 127.0f;
            const float ry = (static_cast<float>(state.rightY) - 128.0f) / 127.0f;
            const bool l1 = (state.buttons & ps2x::iop::PadButton::L1) != 0u;
            const bool hold = holdNow();
            if (gameHasCamera())
            {
                // The game's own moment: everything as on the PS2 (L1 included, with Hold too); an R3 held through it
                // does not switch after, and an L1 press here pairs with none after it.
                g_l1Was = l1;
                g_l1PressMs = -100000;
                g_holdL1 = hold && (l1 || holdCamNow()); // Hold to Cam rests in brawl mode
                g_r3Was = r3;
                g_r3Stray = r3;
                g_stickX = 0.0f;
                g_stickY = 0.0f;
                return;
            }
            if (hold)
                holdInput(state, l1, r3, std::fmax(std::fabs(rx), std::fabs(ry)) > kSwitchMaxStick);
            else
            {
                // R3 switches on its release, unless the stick was pushed out during the press (a dodge flick that
                // clicked).
                if (r3 && std::fmax(std::fabs(rx), std::fabs(ry)) > kSwitchMaxStick)
                    g_r3Stray = true;
                if (!r3 && g_r3Was)
                {
                    if (!g_r3Stray)
                    {
                        const int d = device();
                        g_classic[d] = !g_classic[d].load(std::memory_order_relaxed);
                        g_classicByAuto[d] = false;
                    }
                    g_r3Stray = false;
                }
                g_r3Was = r3;
                // L1 is the game's quick turn; in the free camera the camera swings back behind Gene with him.
                if (l1 && !g_l1Was && !classicByChoice())
                    g_recenter.fetch_add(1u, std::memory_order_relaxed);
                g_l1Was = l1;
            }

            if (classicByChoice() || !g_free.load(std::memory_order_relaxed))
            {
                // The original controls (or the camera still easing back to them).
                g_stickX = 0.0f;
                g_stickY = 0.0f;
                g_moving = false;
                if (!classicByChoice() && !keyDodge)
                {
                    state.rightX = 0x80u; // easing out of a quick turn: no dodges from the camera stick
                    state.rightY = 0x80u;
                }
                return;
            }
            if (keyDodge)
            {
                // Shift + the move keys in the free camera: the game's dodge from Gene's facing (the left stick's keys
                // went to the right stick); the camera keeps still.
                g_stickX = 0.0f;
                g_stickY = 0.0f;
                g_moving = false;
                return;
            }
            // The free camera has the right stick; the game sees it centred (no dodges).
            g_stickX = stickAxis(state.rightX);
            g_stickY = stickAxis(state.rightY);
            state.rightX = 0x80u;
            state.rightY = 0x80u;

            if (g_controlsOff.load(std::memory_order_relaxed))
            {
                g_moving = false; // nor is Gene turned toward it
                return;
            }
            // The left stick, relative to the camera: the direction it asks for, as the stick the original controls
            // would need for it (relative to where Gene faces).
            const float lx = (static_cast<float>(state.leftX) - 128.0f) / 127.0f;
            const float ly = (static_cast<float>(state.leftY) - 128.0f) / 127.0f;
            const float magnitude = std::fmin(std::sqrt(lx * lx + ly * ly), 1.0f);
            if (magnitude < kStickDeadzone)
            {
                g_moving = false;
                return;
            }
            const float desired = wrapAngle(g_cameraYaw.load(std::memory_order_relaxed) - std::atan2(lx, -ly));
            g_moveYaw = desired;
            g_moving = true;
            const float relative = wrapAngle(desired - g_geneYaw.load(std::memory_order_relaxed));
            const float x = -std::sin(relative) * magnitude, up = std::cos(relative) * magnitude;
            const auto axis = [](float value) { return static_cast<uint8_t>(std::lround(std::fmin(std::fmax(128.0f + value * 127.0f, 0.0f), 255.0f))); };
            state.leftX = axis(x);
            state.leftY = axis(-up);
        }

        // Camera side (game thread).
        float g_yaw = 0.0f;
        float g_pitch = 0.0f;
        bool g_tracking = true;      // the free camera is on the original one (eased in, or just reset)
        bool g_following = false;   // a quick turn: easing back behind Gene before it is free again
        int64_t g_followUntilMs = 0; // ... and staying there at least until then (his turn takes a moment)
        uint32_t g_recenterSeen = 0;
        int g_mouseState = -1; // the mouse camera as last set (-1 = not yet)

        // The runtime's mouse camera on while the Modern style and the mouse option are; its movement since the last
        // frame (always taken, so nothing piles up while it is not used).
        ps2_host_input::MouseCameraInput takeMouse()
        {
            // The mouse is the keyboard's: its camera follows the keyboard & mouse Control Style.
            const int wanted = (g_kbmModern.load(std::memory_order_relaxed) && g_mouseCamera.load(std::memory_order_relaxed)) ? 1 : 0;
            if (wanted != g_mouseState)
            {
                g_mouseState = wanted;
                ps2_host_input::setMouseStickEnabled(false); // the mouse is never the game's right stick
                ps2_host_input::setMouseCameraEnabled(wanted != 0); // the window thread frees the cursor when off
            }
            return ps2_host_input::consumeMouseCameraInput();
        }

        uint32_t ram32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        float ramFloat(const uint8_t *rdram, uint32_t address)
        {
            float value;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        bool validPointer(uint32_t pointer) { return pointer >= 0x100000u && pointer < PS2_RDRAM_SIZE; }

        // Gene stands or walks: major step 0, sub-step 0 / 10 (standing) or 1 / 2 (walking).
        bool geneMoves(const uint8_t *rdram, uint32_t player)
        {
            const uint8_t major = rdram[(player + 0x2F4u) & PS2_RAM_MASK], sub = rdram[(player + 0x2F5u) & PS2_RAM_MASK];
            return major == 0u && (sub == 0u || sub == 1u || sub == 2u || sub == 10u);
        }

        // Metres from Gene to the nearest live enemy (cEmManage's list; enemies are kinds 0x200..0x27F, alive while
        // +0x54A > 0 and +0x250 has no release bit). Hold's Auto (the alpha's fight rule, 599e045^).
        float nearestEnemy(const uint8_t *rdram)
        {
            const uint32_t player = ram32(rdram, kPlayerPointer);
            if (!validPointer(player) || !validPointer(ram32(rdram, player + 0xF0u)))
                return 1e9f;
            const uint32_t at = ram32(rdram, player + 0xF0u);
            const float px = ramFloat(rdram, at), pz = ramFloat(rdram, at + 8u);
            float best = 1e18f;
            uint32_t slot = ram32(rdram, kEnemyList);
            for (int guard = 0; guard < 64 && validPointer(slot); ++guard)
            {
                const uint32_t em = ram32(rdram, slot + 8u);
                slot = ram32(rdram, slot + 4u);
                if (!validPointer(em))
                    continue;
                uint16_t kind;
                int16_t hp;
                std::memcpy(&kind, rdram + ((em + 0x2FEu) & PS2_RAM_MASK), sizeof(kind));
                std::memcpy(&hp, rdram + ((em + 0x54Au) & PS2_RAM_MASK), sizeof(hp));
                const int32_t flags = static_cast<int32_t>(ram32(rdram, em + 0x250u));
                if (kind < 0x200u || kind >= 0x280u || hp <= 0 || flags < 0)
                    continue;
                const uint32_t pos = ram32(rdram, em + 0xF0u);
                if (!validPointer(pos))
                    continue;
                const float dx = ramFloat(rdram, pos) - px, dz = ramFloat(rdram, pos + 8u) - pz;
                best = std::fmin(best, dx * dx + dz * dz);
            }
            return best < 1e17f ? std::sqrt(best) : 1e9f;
        }
        int64_t g_lastNearMs = -100000; // game thread: an enemy was within kFightRange (or a hit landed) then

        std::filesystem::path g_iniPath;
        std::mutex g_statusMutex;
        std::string g_status;

        void setStatus(std::string text)
        {
            std::lock_guard<std::mutex> lock(g_statusMutex);
            g_status = std::move(text);
        }

        std::string number(float value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", std::round(value * 10.0f) / 10.0f);
            return text;
        }

        std::string number2(float value)
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", std::round(value * 1000.0f) / 1000.0f);
            return text;
        }

        void saveToIni()
        {
            IniFile ini;
            if (g_iniPath.empty() || !ini.load(g_iniPath))
            {
                setStatus("Could not read GHR.ini.");
                return;
            }
            ini.set("Game", "ControlStyle", !g_modern.load() ? "classic" : g_hold.load() ? "hold" : "modern");
            ini.set("Game", "HoldMode", g_holdCam.load() ? "cam" : "brawl");
            ini.setBool("Game", "ModernAuto", g_auto.load());
            ini.set("Game", "KbmControlStyle", g_kbmModern.load() ? "modern" : "classic");
            ini.setBool("Game", "KbmModernAuto", g_kbmAuto.load());
            ini.set("Game", "CameraSpeed", number(g_yawSpeed.load()));
            ini.set("Game", "CameraPitchSpeed", number(g_pitchSpeed.load()));
            ini.setBool("Game", "CameraInvertX", g_invertX.load());
            ini.setBool("Game", "CameraInvertY", g_invertY.load());
            ini.setBool("Game", "MouseCamera", g_mouseCamera.load());
            ini.set("Game", "MouseSensitivity", number2(g_mouseSensitivity.load()));
            ini.set("Game", "MenuButtons", menu_buttons::styleName(menu_buttons::styleSetting()));
            ini.set("Game", "ButtonPrompts", prompt_hd::choiceName(prompt_hd::choice()));
            setStatus(ini.save() ? "Saved to GHR.ini." : "Could not write GHR.ini.");
        }

#if defined(KAMITE_HAS_GS_MENU)
        void slider(const Pcsx2GsUi *ui, const char *label, std::atomic<float> &setting, float low, float high, const char *format)
        {
            float value = setting.load(std::memory_order_relaxed);
            ui->set_next_width(340.0f);
            if (ui->slider_float(label, &value, low, high, format))
                setting.store(value, std::memory_order_relaxed);
        }

        void checkbox(const Pcsx2GsUi *ui, const char *label, std::atomic<bool> &setting)
        {
            int value = setting.load() ? 1 : 0;
            if (ui->checkbox(label, &value))
                setting = value != 0;
        }

        void drawControlsPanel(const Pcsx2GsUi *ui)
        {
            if (ui->version < 3u)
            {
                ui->text_wrapped("This pcsx2gs.dll is older than the game: build again to get these controls.");
                return;
            }
            ui->separator_text("Control style");
            static const char *const kStyles[] = {"Classic: as on the PS2", "Modern: Toggle (the right stick press switches)",
                                                  "Modern: Hold (brawl mode while L1 is held)"};
            int style = !g_modern.load() ? 0 : g_hold.load() ? 2 : 1;
            ui->text("Controller");
            ui->set_next_width(340.0f);
            if (ui->combo("##style", &style, kStyles, 3))
            {
                g_modern = style >= 1;
                g_hold = style == 2;
            }
            if (style == 1)
                checkbox(ui, "Auto: fighting starts brawl mode (the right stick press switches back)##auto", g_auto);
            else if (style == 2)
            {
                checkbox(ui, "Hold to Cam: brawl mode unless L1 is held (L1 held: free camera; a tap turns)##holdcam", g_holdCam);
                if (!g_holdCam.load())
                    checkbox(ui, "Auto: fighting starts brawl mode, the fight's end ends it##autohold", g_auto);
            }
            static const char *const kKbmStyles[] = {"Classic: as on the PS2", "Modern: Toggle (free camera on the mouse)"};
            int kbmStyle = g_kbmModern.load() ? 1 : 0;
            ui->text("Keyboard & mouse");
            ui->set_next_width(340.0f);
            if (ui->combo("##kbmstyle", &kbmStyle, kKbmStyles, 2))
                g_kbmModern = kbmStyle == 1;
            if (kbmStyle == 1)
                checkbox(ui, "Auto: fighting starts brawl mode (the middle click switches back)##kbmauto", g_kbmAuto);
            ui->text_disabled("The style in effect is the device used last.");
            // The Modern settings show only with Modern selected (K, Oct 27).
            if (style >= 1 || kbmStyle == 1)
            {
                ui->text_wrapped(style == 2 && g_holdCam.load()
                                     ? "Hold to Cam: brawl mode (classic controls) unless L1 is held; holding L1 is the free "
                                       "camera, a tap of L1 the quick turn. R3 turns too if the Button Mapping page gave it the turn."
                                 : style == 2 ? "Hold: brawl mode (classic controls) while L1 is held; double-tap L1 for the "
                                              "quick turn. R3 turns too if the Button Mapping page gave it the turn."
                                            : "Toggle: the stick press (middle click) switches between the free camera and "
                                              "brawl mode (classic controls); each device keeps its choice across rooms. L1 "
                                              "is the game's quick turn.");
                ui->separator_text("Free camera");
                slider(ui, "Turn speed##yaw", g_yawSpeed, 40.0f, 400.0f, "%.0f deg/s");
                slider(ui, "Up/down speed##pitch", g_pitchSpeed, 20.0f, 250.0f, "%.0f deg/s");
                checkbox(ui, "Invert left/right##invx", g_invertX);
                checkbox(ui, "Invert up/down##invy", g_invertY);
            }
            if (kbmStyle == 1)
            {
                ui->separator_text("Mouse");
                checkbox(ui, "The mouse turns the free camera##mouse", g_mouseCamera);
                ui->begin_disabled(!g_mouseCamera.load());
                slider(ui, "Sensitivity##mousesens", g_mouseSensitivity, 0.02f, 0.5f, "%.3f deg/pixel");
                ui->end_disabled();
                ui->text_disabled("Click in the game to capture the mouse; Esc frees it (so does this menu).");
            }

            ui->separator_text("Now");
            const int d = device();
            const char *now = !modernNow()        ? (d ? "Keyboard & mouse: Classic" : "Controller: Classic")
                              : gameHasCamera()   ? "The game has the camera: brawl mode"
                              : classicByChoice() ? (g_classicByAuto[d].load() ? "Brawl mode (Auto: a hit landed)"
                                                     : holdCamNow()             ? "Brawl mode (Hold to Cam: L1 not held)"
                                                     : holdNow()                ? "Brawl mode (L1 held)"
                                                                                : "Brawl mode (stick press / middle click)")
                                                  : "Free camera, camera-relative movement";
            ui->text(now);

            ui->separator_text("Menu controls");
            // In the order of the launcher's list; the index is menu_buttons::Style's order 1, 2, 0.
            static const char *const kMenuButtons[] = {"Western: the bottom button confirms, the right one cancels",
                                                       "Japanese: the right button confirms, the bottom one cancels",
                                                       "Classic: circle and cross confirm, triangle cancels"};
            static constexpr menu_buttons::Style kMenuStyles[] = {menu_buttons::Style::Western, menu_buttons::Style::Japanese,
                                                                  menu_buttons::Style::Classic};
            const menu_buttons::Style menuStyle = menu_buttons::styleSetting();
            int menus = menuStyle == menu_buttons::Style::Japanese ? 1 : menuStyle == menu_buttons::Style::Classic ? 2 : 0;
            ui->set_next_width(340.0f);
            if (ui->combo("##menubuttons", &menus, kMenuButtons, 3) && menus >= 0 && menus < 3)
                menu_buttons::setStyle(kMenuStyles[menus]);
            ui->text_disabled("Menus, dialogs and the roulette; the moves keep their buttons. With the keyboard the menus are "
                              "always Western.");

            ui->separator_text("GHR.ini");
            if (ui->button("Save##controls"))
                saveToIni();
            {
                std::lock_guard<std::mutex> lock(g_statusMutex);
                if (!g_status.empty())
                {
                    ui->same_line();
                    ui->text(g_status.c_str());
                }
            }
            ui->separator_text("Button prompts");
            static const char *const kPrompts[] = {"Automatic: from the controller in use", "Xbox", "PlayStation",
                                                   "Nintendo", "Steam"};
            int prompts = static_cast<int>(prompt_hd::choice());
            ui->set_next_width(340.0f);
            if (ui->combo("##prompts", &prompts, kPrompts, 5))
            {
                prompt_hd::setChoice(static_cast<prompt_hd::Choice>(prompts));
                button_prompts::requestRebuild();
            }
            ui->text_disabled("The buttons the game shows for a controller; keys and mouse always show keys. Redrawn at once.");
            ui->separator_text("Bindings");
            ui->text_disabled("The game's buttons: the launcher's Controls tab edits controls.ini.");
            ui->spacing();
            hotkeys::drawSection(ui);
        }
#endif

        float envFloat(const char *name, float fallback, float low, float high)
        {
            const char *text = std::getenv(name);
            const float value = (text && *text) ? static_cast<float>(std::atof(text)) : fallback;
            return std::fmin(std::fmax(value, low), high);
        }

        bool envIs(const char *name, const char *value)
        {
            const char *text = std::getenv(name);
            if (!text)
                return false;
            std::string lower(text);
            for (char &c : lower)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return lower == value;
        }

        bool envBool(const char *name, bool fallback)
        {
            const char *text = std::getenv(name);
            return (text && *text) ? *text != '0' : fallback;
        }
    }

    void noteCamera(const uint8_t *rdram, bool battle, bool gameCamera)
    {
        const int64_t now = nowMs();
        g_battleCamera.store(battle, std::memory_order_relaxed);
        // The shop's jukebox: the game's camera stands still on the radio, as on the PS2 (jukebox.h). The roulette:
        // the game swings its camera round to the roulette angle while the wheel is up; Modern hands it back too.
        g_gameCamera.store(gameCamera || jukebox::open(rdram) || rouletteUp(), std::memory_order_relaxed);
        g_controlsOff.store((ram32(rdram, kControlWord) & kControlsOffBits) != 0u, std::memory_order_relaxed);
        g_lastCameraMs.store(now, std::memory_order_relaxed);
        // Hold's Auto ends its brawl mode by itself (Hold has no switch): the fight is over once the battle camera is
        // off and no live enemy has been within kFightRange for kFightLingerMs.
        if (g_hold.load(std::memory_order_relaxed) && g_classicByAuto[0].load(std::memory_order_relaxed))
        {
            if (nearestEnemy(rdram) <= kFightRange)
                g_lastNearMs = now;
            if (!battle && now - g_lastNearMs > kFightLingerMs)
            {
                g_classic[0] = false;
                g_classicByAuto[0] = false;
            }
        }
    }

    namespace
    {
        bool g_turned = false;     // updateCameraLive turned Gene this call...
        float g_turnedYaw = 0.0f;  // ... to this yaw
    }

    FreeCamera updateCameraLive(uint8_t *rdram, float gameYaw, float geneYaw, float dt);

    // A case replay plays the recorded camera and turns back (not in the save state: test_mode.h HostCamera); a case
    // being recorded keeps them.
    FreeCamera updateCamera(uint8_t *rdram, float gameYaw, float geneYaw, float dt)
    {
        test_mode::HostCamera host;
        if (test_mode::replayHost(host))
        {
            takeMouse();
            float yaw = geneYaw;
            const uint32_t player = ram32(rdram, kPlayerPointer);
            if (host.turned && validPointer(player))
            {
                yaw = host.geneYaw;
                std::memcpy(rdram + ((player + 0x104u) & PS2_RAM_MASK), &yaw, sizeof(yaw));
            }
            g_geneYaw.store(yaw, std::memory_order_relaxed);
            g_cameraYaw.store(host.yaw, std::memory_order_relaxed);
            g_free = host.free;
            FreeCamera out;
            out.free = host.free;
            out.yaw = host.yaw;
            out.pitch = host.pitch;
            return out;
        }
        g_turned = false;
        const FreeCamera out = updateCameraLive(rdram, gameYaw, geneYaw, dt);
        host.free = out.free;
        host.yaw = out.yaw;
        host.pitch = out.pitch;
        host.turned = g_turned;
        host.geneYaw = g_turnedYaw;
        test_mode::recordHost(host);
        return out;
    }

    FreeCamera updateCameraLive(uint8_t *rdram, float gameYaw, float geneYaw, float dt)
    {
        FreeCamera out;
        g_geneYaw.store(geneYaw, std::memory_order_relaxed);
        const ps2_host_input::MouseCameraInput mouse = takeMouse();
        if (!modernNow())
        {
            g_tracking = true;
            g_free = false;
            return out;
        }
        dt = std::fmin(std::fmax(dt, 0.0f), 0.1f);
        const int64_t now = nowMs();
        const uint32_t recenter = g_recenter.load(std::memory_order_relaxed);
        if (recenter != g_recenterSeen)
        {
            g_recenterSeen = recenter;
            g_following = true;
            g_followUntilMs = now + kQuickTurnFollowMs;
        }
        const bool original = gameHasCamera() || classicByChoice();
        if (g_tracking && !original && !g_following)
        {
            g_yaw = gameYaw; // starts free from where the original camera is
            g_pitch = 0.0f;
            g_tracking = false;
        }
        if (original || g_following)
        {
            // Back to the original camera (faster while following a quick turn); once there, it is the original
            // camera exactly. A quick turn keeps it behind Gene until his turn is over. The L1 glide was 7.0 (K,
            // Oct 27: "a touch too slow", 30% faster).
            const float ease = 1.0f - std::exp(-dt * (g_following ? 12.0f : 9.1f));
            g_yaw = wrapAngle(g_yaw + wrapAngle(gameYaw - g_yaw) * ease);
            g_pitch += (0.0f - g_pitch) * ease;
            const bool there = std::fabs(wrapAngle(gameYaw - g_yaw)) < 0.01f && std::fabs(g_pitch) < 0.005f;
            if (there && now >= g_followUntilMs)
            {
                g_following = false;
                if (original)
                    g_tracking = true;
            }
            g_free = false;
            if (g_tracking)
                return out;
        }
        else
        {
            const float x = g_stickX.load(std::memory_order_relaxed) * (g_invertX.load() ? -1.0f : 1.0f);
            const float y = g_stickY.load(std::memory_order_relaxed) * (g_invertY.load() ? -1.0f : 1.0f);
            float yawStep = x * g_yawSpeed.load(std::memory_order_relaxed) * kDegToRad * dt;
            float pitchStep = y * g_pitchSpeed.load(std::memory_order_relaxed) * kDegToRad * dt;
            if (mouse.active)
            {
                // Mouse right = turn right (yaw decreasing), mouse down = look down (pitch up), as the stick.
                const float perPixel = g_mouseSensitivity.load(std::memory_order_relaxed) * kDegToRad;
                yawStep += mouse.x * perPixel * (g_invertX.load() ? -1.0f : 1.0f);
                pitchStep += mouse.y * perPixel * (g_invertY.load() ? -1.0f : 1.0f);
            }
            g_yaw = wrapAngle(g_yaw - yawStep);
            const float floor = std::fmax(kPitchMin, std::fmin(g_pitchFloor.load(std::memory_order_relaxed), 0.0f));
            g_pitch = std::fmin(std::fmax(g_pitch + pitchStep, floor), kPitchMax);
            g_free = true;

            // Gene faces where the left stick points while he stands or walks (elsewhere the stick the filter made
            // steers him the original way).
            const uint32_t player = ram32(rdram, kPlayerPointer);
            if (g_moving.load(std::memory_order_relaxed) && validPointer(player) && geneMoves(rdram, player))
            {
                const float target = g_moveYaw.load(std::memory_order_relaxed);
                const float step = std::fmin(std::fmax(wrapAngle(target - geneYaw), -kTurnRate * dt), kTurnRate * dt);
                const float turned = wrapAngle(geneYaw + step);
                std::memcpy(rdram + ((player + 0x104u) & PS2_RAM_MASK), &turned, sizeof(turned));
                g_geneYaw.store(turned, std::memory_order_relaxed);
                g_turned = true;
                g_turnedYaw = turned;
            }
        }
        g_cameraYaw.store(g_yaw, std::memory_order_relaxed);
        out.free = true;
        out.yaw = g_yaw;
        out.pitch = g_pitch;
        return out;
    }

    void setPitchFloor(float floor)
    {
        g_pitchFloor.store(floor, std::memory_order_relaxed);
    }

    void resetCamera()
    {
        g_tracking = true;
        g_following = false;
    }

    bool enabled()
    {
        return modernNow();
    }

    void setModern(bool on) { g_modern = on; }
    void setHold(bool on) { g_hold = on; }
    void setHoldCam(bool on) { g_holdCam = on; }
    bool holdEnabled() { return modernNow() && holdNow(); }
    bool holdCamEnabled() { return modernNow() && holdCamNow(); }
    void setAuto(bool on) { g_auto = on; }
    void setKbmModern(bool on) { g_kbmModern = on; }
    void setKbmAuto(bool on) { g_kbmAuto = on; }
    void setMouseCamera(bool on) { g_mouseCamera = on; }
    void setInvertY(bool on) { g_invertY = on; }

    bool autoEnabled() { return autoNow(); }

    CaseStyle caseStyle()
    {
        return CaseStyle{modernNow(), autoNow(), holdNow(), holdCamNow()};
    }

    void noteContact()
    {
        const int d = device();
        if (d == 0)
            g_lastNearMs = nowMs(); // Hold's Auto: a hit is a moment of the fight (a throw that hit from afar too)
        if (autoNow() && modernNow() && !g_classic[d].load(std::memory_order_relaxed))
        {
            g_classic[d] = true;
            g_classicByAuto[d] = true;
        }
    }

    bool freeCameraNow()
    {
        return modernNow() && !gameHasCamera() && !classicByChoice();
    }

    void install()
    {
        g_hold = envIs("KAMITE_CONTROL_STYLE", "hold");
        g_modern = envIs("KAMITE_CONTROL_STYLE", "modern") || g_hold.load();
        g_holdCam = envIs("KAMITE_HOLD_MODE", "cam");
        g_auto = envBool("KAMITE_MODERN_AUTO", true);
        g_kbmModern = !envIs("KAMITE_KBM_CONTROL_STYLE", "classic"); // keyboard & mouse: Modern unless set (K, Nov 16)
        g_kbmAuto = envBool("KAMITE_KBM_MODERN_AUTO", false);
        g_yawSpeed = envFloat("KAMITE_CAMERA_SPEED", 200.0f, 40.0f, 400.0f);
        g_pitchSpeed = envFloat("KAMITE_CAMERA_PITCH_SPEED", 80.0f, 20.0f, 250.0f);
        g_invertX = envBool("KAMITE_CAMERA_INVERT_X", false);
        g_invertY = envBool("KAMITE_CAMERA_INVERT_Y", false);
        g_mouseCamera = envBool("KAMITE_MOUSE_CAMERA", true);
        g_mouseSensitivity = envFloat("KAMITE_MOUSE_SENSITIVITY", 0.1f, 0.01f, 1.0f);
        g_iniPath = cameraIniPath();
        ps2_host_input::setPadFilter(&padFilter);
        KM_INFO(Game, "control style: controller " << (g_modern.load() ? (g_hold.load() ? (g_holdCam.load() ? "hold to cam" : g_auto.load() ? "hold, auto" : "hold")
                                                                                         : (g_auto.load() ? "modern, auto" : "modern"))
                                                                       : "classic")
                                                    << "; keyboard & mouse "
                                                    << (g_kbmModern.load() ? (g_kbmAuto.load() ? "modern, auto" : "modern") : "classic"));
#if defined(KAMITE_HAS_GS_MENU)
        static bool s_panel = false;
        if (!s_panel)
        {
            s_panel = true;
            ps2x::menu::addPanel("Controls", &drawControlsPanel);
        }
#endif
    }
}
