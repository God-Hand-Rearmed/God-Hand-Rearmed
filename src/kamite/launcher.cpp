#include "kamite/launcher.h"
#include "kamite/paths.h"
#include "kamite/launcher_bark.h"
#include "kamite/launcher_book.h"
#include "kamite/launcher_paper.h"
#include "kamite/launcher_saloon.h"

#include "kamite/character_mods.h"
#include "kamite/disc_files.h"
#include "kamite/game_disc.h"
#include "kamite/ini_file.h"
#include "kamite/launcher_platform.h"
#include "kamite/launcher_update.h"
#if defined(KAMITE_STEAM_SHORTCUT) && KAMITE_STEAM_SHORTCUT
#include "kamite/launcher_steam.h"
#define KAMITE_LAUNCHER_STEAM 1
#endif
#include "kamite/log.h"
#include "kamite/mod_list.h"
#include "kamite/mod_zip.h"
#include "kamite/presets.h"
#include "kamite/settings_table.h"
#include "kamite/version.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_hotkeys.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "raylib.h"
#include "rlImGui.h"
#include "rlgl.h"

#include <cstdint>
#include <algorithm>
#include <array>
#include <cfloat>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace kamite::launcher
{
    namespace
    {
        namespace fs = std::filesystem;

        // Items whose activation makes the cursor's tick, not the confirm sound (boxes, dropdowns, radio buttons, folds:
        // K), and the buttons that play their own sound when let go (Back: cancel, Play: Play's; a confirm on the press
        // made both sound). Refilled each frame by the widgets right after they are drawn; read by frameSounds.
        std::vector<ImGuiID> g_tickItems, g_ownSoundItems, g_silentItems;
        void tickItem() { g_tickItems.push_back(ImGui::GetItemID()); }
        void silentItem() { g_silentItems.push_back(ImGui::GetItemID()); } // sliders make no sound (K)
        void ownSoundItem() { g_ownSoundItems.push_back(ImGui::GetItemID()); }

        // The updater's threads outlive the launcher's state (a slow check is left running when Play is pressed).
        update::Checker g_updateCheck;
        update::Installer g_updater;

        // ---- choices ------------------------------------------------------------------------------------------------
        struct Choice
        {
            const char *label;
            const char *value;
        };

        constexpr Choice kWindowModes[] = {{"Fullscreen (Borderless)", "fullscreen"}, {"Maximized Window", "maximized"}, {"Window", "window"}};

        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // A combo over fixed choices; a value that is none of them (set by hand) is shown as one more entry and kept
        // unless another is picked. Returns true when changed (value updated).
        template <size_t N>
        bool choiceCombo(const char *id, const Choice (&choices)[N], std::string &value)
        {
            int current = -1;
            for (size_t i = 0; i < N; ++i)
                if (lower(value) == lower(choices[i].value))
                    current = static_cast<int>(i);
            std::vector<const char *> labels;
            for (const Choice &choice : choices)
                labels.push_back(choice.label);
            const std::string custom = "Custom: " + value;
            if (current < 0)
            {
                labels.push_back(custom.c_str());
                current = static_cast<int>(N);
            }
            int picked = current;
            ImGui::SetNextItemWidth(-1.0f);
            const bool changed = ImGui::Combo(id, &picked, labels.data(), static_cast<int>(labels.size()));
            tickItem();
            if (!changed || picked == current || picked >= static_cast<int>(N))
                return false;
            value = choices[picked].value;
            return true;
        }

        // ---- look ---------------------------------------------------------------------------------------------------
        const ImVec4 kAccent(0.91f, 0.38f, 0.16f, 1.0f);      // The game's fire orange
        const ImVec4 kAccentHover(1.00f, 0.50f, 0.22f, 1.0f);
        const ImVec4 kAccentActive(0.78f, 0.28f, 0.10f, 1.0f);
        const ImVec4 kPanel(0.10f, 0.09f, 0.09f, 0.92f);
        const ImVec4 kTextDim(0.66f, 0.62f, 0.58f, 1.0f);
        const ImVec4 kTextSoft(0.80f, 0.77f, 0.73f, 1.0f); // dimmer text straight on the background
        const ImVec4 kText(0.95f, 0.92f, 0.88f, 1.0f);
        const ImVec4 kHeading(1.0f, 0.62f, 0.30f, 1.0f); // the Options title and the categories' headings
        bool g_hotkeysOpen = false;                       // the Controls tab's Hotkeys: folded each time Options opens

        void applyStyle()
        {
            ImGuiStyle &style = ImGui::GetStyle();
            ImGui::StyleColorsDark(&style);
            style.WindowRounding = 0.0f;
            style.FrameRounding = 6.0f;
            style.PopupRounding = 6.0f;
            style.GrabRounding = 6.0f;
            style.TabRounding = 6.0f;
            style.FramePadding = ImVec2(12.0f, 8.0f);
            style.ItemSpacing = ImVec2(12.0f, 12.0f);
            style.WindowPadding = ImVec2(0.0f, 0.0f);
            ImVec4 *colors = style.Colors;
            colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
            colors[ImGuiCol_ChildBg] = kPanel;
            colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.11f, 0.10f, 0.98f);
            colors[ImGuiCol_Border] = ImVec4(0.35f, 0.22f, 0.15f, 0.6f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.16f, 0.15f, 1.0f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.20f, 0.17f, 1.0f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.22f, 0.17f, 1.0f);
            colors[ImGuiCol_Button] = ImVec4(0.20f, 0.17f, 0.15f, 1.0f);
            colors[ImGuiCol_ButtonHovered] = kAccentHover;
            colors[ImGuiCol_ButtonActive] = kAccentActive;
            colors[ImGuiCol_Header] = ImVec4(0.30f, 0.20f, 0.15f, 1.0f);
            colors[ImGuiCol_HeaderHovered] = kAccentHover;
            colors[ImGuiCol_HeaderActive] = kAccentActive;
            colors[ImGuiCol_CheckMark] = kAccent;
            colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.15f, 0.13f, 1.0f);
            colors[ImGuiCol_TabHovered] = kAccentHover;
            colors[ImGuiCol_TabSelected] = kAccent;
            colors[ImGuiCol_NavCursor] = kAccentHover;
            colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.22f, 0.15f, 0.6f);
            colors[ImGuiCol_Text] = ImVec4(0.95f, 0.92f, 0.88f, 1.0f);
            colors[ImGuiCol_TextDisabled] = kTextDim;
        }

        void drawBackground()
        {
            const int width = GetScreenWidth(), height = GetScreenHeight();
            DrawRectangleGradientV(0, 0, width, height, Color{20, 15, 13, 255}, Color{48, 20, 10, 255});
            // A low glow behind the title.
            DrawCircleGradient(width / 2, height / 4, static_cast<float>(std::max(width, height)) * 0.45f, Color{120, 45, 15, 70},
                               Color{0, 0, 0, 0});
        }

        // Every controller, as ImGui's gamepad keys (SDL3 through ps2_host_gamepads: the same devices the game uses).
        void feedGamepads(ImGuiIO &io)
        {
            using namespace ps2_host_gamepads;
            ps2_host_gamepads::update();
            bool held[16] = {};
            float lx = 0.0f, ly = 0.0f;
            for (int device = 0; device < kMaxDevices; ++device)
            {
                if (!connected(device))
                    continue;
                held[0] |= button(device, DPadUp);
                held[1] |= button(device, DPadDown);
                held[2] |= button(device, DPadLeft);
                held[3] |= button(device, DPadRight);
                held[4] |= button(device, South);
                held[5] |= button(device, East);
                held[6] |= button(device, West);
                held[7] |= button(device, North);
                held[8] |= button(device, LeftShoulder);
                held[9] |= button(device, RightShoulder);
                held[10] |= button(device, Start);
                held[11] |= button(device, Back);
                if (std::fabs(axis(device, LeftX)) > std::fabs(lx))
                    lx = axis(device, LeftX);
                if (std::fabs(axis(device, LeftY)) > std::fabs(ly))
                    ly = axis(device, LeftY);
            }
            const ImGuiKey keys[] = {ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                                     ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadFaceUp,
                                     ImGuiKey_GamepadL1,       ImGuiKey_GamepadR1,       ImGuiKey_GamepadStart,    ImGuiKey_GamepadBack};
            for (int i = 0; i < 12; ++i)
                io.AddKeyEvent(keys[i], held[i]);
            auto stick = [&](ImGuiKey negative, ImGuiKey positive, float value)
            {
                const float dead = 0.35f;
                const float n = value < -dead ? std::min(1.0f, (-value - dead) / (1.0f - dead)) : 0.0f;
                const float p = value > dead ? std::min(1.0f, (value - dead) / (1.0f - dead)) : 0.0f;
                io.AddKeyAnalogEvent(negative, n > 0.0f, n);
                io.AddKeyAnalogEvent(positive, p > 0.0f, p);
            };
            stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, lx);
            stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ly);
        }

        // ---- the screens --------------------------------------------------------------------------------------------
        enum class Screen
        {
            Home,
            Install,
            Options,
            FirstRun, // the wanted posters: a preset to start with (once, on a new install)
        };

        struct State
        {
            const Context *context = nullptr;
            IniFile ini;
            Screen screen = Screen::Home;
            bool installed = false;
            std::string installMessage;
            bool installError = false;
            char isoText[1024] = {};
            bool focusFirst = true; // put the gamepad/keyboard cursor on the screen's first button
            bool done = false;
            Result result = Result::Quit;
            ImFont *font = nullptr;
            float unit = 1.0f; // UI scale: window height / 720
            // The logo's layers (tools/logo/export_launcher_logo.py), all the same canvas, drawn into one rectangle:
            Texture2D logo{};        // resources/images/logo.png: GOD HAND and its shadow (it stays still)
            Texture2D kana{};        // resources/images/kana.png: the flat kana behind it (moves a little with the cursor)
            Texture2D rearmed{};     // resources/images/rearmed.png: the REARMED plate without its letters
            Texture2D rearmedText{}; // resources/images/rearmed_text.png: its letters, white (tinted gold / hot)
            Texture2D rearmedGlow{}; // resources/images/rearmed_glow.png: the letters blurred, for the hot glow
            Texture2D burst{}; // resources/images/burst.png: the speed lines behind the logo
            // The saloon (launcher_saloon.h, claude/saloon-launcher.md): the room behind every screen, Home's menu in
            // ink on its bill of fare, the logo in the doorway's glare, the Options zoom. Off (the plain gradient and
            // the old buttons) when resources/saloon is missing or can't be decoded.
            saloon::Scene scene;
            bool saloonOn = false;
            saloon::BillFonts billFonts;
            saloon::Bill bill;            // the menu's focus (its loop) and items, drawn before ImGui from last frame's
            bool focusOptions = false;    // back from Options: the loop goes back on Options
            float optionsShown = 0.0f;    // Options' panel over the zoom's last frame: fades in once it lands
            bool optionsLeaving = false;  // Back: the panel fades out, then the zoom plays backwards
            bool navCleared = false;      // the mouse left everything: the next key focuses the first item, with a tick
            bool zoomPending = false;     // Options came before the zoom's frames were read
            int slipFocus = 0;            // the corner slips' focused item (SlipItem), drawn before ImGui
            std::vector<Sound> snackVoices; // Gene's dry lines from se/pl00.afs (eating a snack)
            float voiceAt = -1.0f;        // a snack line waiting for the slurp to end (GetTime)
            Music ambience{};             // resources/sfx/launcher: the bar's murmur under the music (K, Nov 18)
            bool hasAmbience = false;
            Sound pen{}, slurp{}, bite{}; // the pencil loop drawn; the smoothie's last sip; a fruit or a garnish eaten
            Sound bookOpen{}, bookClose{}; // the Options zoom in to the book and back out (with the whoosh)
            bool hasPen = false, hasSlurp = false, hasBite = false, hasBookOpen = false, hasBookClose = false;
            // Options in the book (stage 2, K Nov 18): the open slip, Controls' index tab, the focus loop, the right
            // page's help, the paper rustle of a slip, the prompts by the slips (for the device in use).
            struct BookFonts
            {
                // heading: Wendyville, in capitals (the wanted posters' title font); text: Dominican (the splash's,
                // as on the posters); stamp: Holtwood One SC
                ImFont *heading = nullptr, *text = nullptr, *stamp = nullptr;
            } bookFonts;
            int bookTab = 0;      // kBookTabs
            int bookControls = 0; // kControlsPages
            bool bookTabChanged = true;
            ImGuiID bookFocusId = 0;
            float bookFocusAge = 1.0f;
            uint32_t bookFocusSeed = 1;
            int bookQuiet = 0;    // frames the focus moves without the pen (a slip switched)
            std::string bookTitle, bookHelp, bookNote; // the right page: the focused row's
            float bookHelpRoom = 0.0f;  // the help's height on the right page (page units; 0: any): it shrinks to fit
            float bookHelpSize = 28.0f; // the help's size as drawn (page units)
            int bookChoice = -1;  // the setting whose list is open (kSettings index)
            Sound rustle{};
            bool hasRustle = false;
            int lastDevice = 0;   // 0 mouse, 1 keyboard, 2 controller: which prompts show
            std::string promptSet; // the loaded set's folder ("" none)
            std::map<std::string, Texture2D> promptArt; // the Controls pages' button / key pictures, by file
            Texture2D prompt[5]{}; // tab left, tab right, sub-tab left, sub-tab right, back
            bool penThisFrame = false;    // the pen played: no cursor tick on top of it
            int lastSlipFocus = 0;
            int snackVoice = -1;          // the one playing (lines never overlap or queue)
            Font splashFont{}; // resources/splash/splash.fnt, the Dominican font baked as an atlas
            std::string splash; // a random line of resources/splash/splash.txt, "" when there is none
            // The title's motion: the cursor (eased) shifts the burst layers and the kana by different amounts,
            // the bursts turn slowly and pulse; the wordmark does not move.
            float time = 0.0f;
            ImVec2 parallax{0.0f, 0.0f}; // -1..1 each way; 0 while the cursor is away
            float burstShown = 0.0f;     // fades the burst in on the screens with the logo, out on the others
            bool logoPlaced = false;     // logoMin/logoMax hold last frame's logo rectangle (without the parallax shift)
            bool logoThisFrame = false;  // title() laid the logo out this frame (drawLogo draws it after ImGui)
            ImVec2 logoMin{}, logoMax{};
            float playTime = -1.0f;      // seconds since Play was pressed (the plate's animation, the fade); -1: not yet
            bool audio = false;          // the audio device is open (closed again before the game opens its own)
            Music music{};               // resources/music/launcher.ogg, looped; fades out with Play
            bool hasMusic = false;
            // The game's menu sounds (read from the player's disc image): kMenuSoundIds' order.
            Sound menuSounds[4]{};
            float menuSoundBase[4]{};    // each sound's own volume in the game's bank
            bool hasMenuSound[4]{};
            // The start bark (launcher_bark.h, K Nov 10): one line, dealt from [Launcher] BarkDeck, rendered in the
            // background from the disc image; the music's tail is rendered when Play is pressed.
            std::thread barkWorker;
            bool barkStarted = false;
            int barkLine = -1;
            launcher_bark::Stereo barkPcm;   // empty: no voice (not read)
            launcher_bark::Wash wash;
            std::vector<int16_t> musicPcm;   // resources/music/launcher.ogg decoded, for its tail
            int musicRate = 0;
            fs::path musicPath;
            bool musicHandedOff = false;     // Play replaced the music stream with its tail
            float volume = 0.8f;         // [Launcher] Volume: music and sounds (the corner slider)
            bool muted = false;          // [Launcher] Mute
            ImGuiID volumeSliderId = 0;  // makes no sound when grabbed
            ImGuiID lastNavId = 0;       // the keyboard / gamepad cursor's item last frame
            bool popupWasOpen = false;
            bool soundThisFrame = false; // a confirm / cancel / Play sound already played this frame
            int quietFrames = 0;         // no cursor tick for a frame or two after a confirm / cancel (focus moves then)
            Screen soundScreen = Screen::Home;
            // Options > Presets (presets.h): the photos from the disc image (read once, when the tab first shows) and the
            // preset waiting for "Replace your custom settings?".
            Texture2D presetPhotos[presets::kCount]{};
            bool presetPhotosRead = false;
            bool posterPhotosSet = false; // the wall posters in the saloon have them
            int presetAsk = -1;
            bool presetOpenPopup = false;
            // The first run's wanted posters (resources/presets: tools/launcher/make_posters.py), shown once on a new
            // install (no boot ELF in game_data yet and no [Launcher] PresetPicked): the poster picked, the layers.
            bool firstRunPending = false;
            int firstRunPick = presets::kClassic;
            bool postersLoaded = false;
            Texture2D poster[presets::kCount]{}, posterTape[presets::kCount]{}, posterShadow[presets::kCount]{}, stamp{};
            float posterW = 0.0f, posterH = 0.0f, posterPad = 0.0f;
            float photoBox[presets::kCount][4]{}, stampAt[presets::kCount][2]{};
            float pickShown = 0.0f; // the raised poster, eased toward firstRunPick
            // Updates (launcher_update.h): above the volume; What's new and the progress in a popup.
            enum class UpdatePopup
            {
                None,
                Notes,   // What's new
                Working, // g_updater downloading / installing
                Failed,  // g_updater.error
            };
            UpdatePopup updatePopup = UpdatePopup::None;
            bool updateOpenPopup = false;
            // The newspaper (launcher_paper.h, claude/newspaper.md; the saloon's version / update element).
            paper::Paper paper;
            bool paperOn = false;      // its fonts loaded (else the old slip)
            bool paperOpen = false;    // opened (sliding up, or up)
            float paperT = 0.0f;       // 0 in the corner .. 1 open
            int paperFocus = 0;        // 0 BACK, 1 UPDATE NOW
            bool paperFocusSet = false;
            float paperScroll = 0.0f;  // page units
            bool paperPrinting = false; // the update runs (shown on the paper)
            std::string paperStop;     // a failed update's reason
            ImGuiID paperCornerId = 0; // the corner's item (last frame), for the bill's pad routes
            fs::path exePath; // read at start: the update renames the running file aside
#if KAMITE_LAUNCHER_STEAM
            // Add to Steam (launcher_steam.h): the lower left corner's button and its popup.
            enum class SteamPopup
            {
                None,
                Confirm, // Steam runs: restart it?
                Working, // the job on steamWorker; steamStage follows it
                Done     // steamResult's message
            };
            steam::Status steam;              // probed when the launcher opens and after each job
            std::thread steamWorker;
            std::atomic<int> steamStage{-1};  // steam::Stage while a job runs
            steam::Result steamResult;        // read once the worker is joined
            SteamPopup steamPopup = SteamPopup::None;
            bool steamOpenPopup = false;
            bool steamFocus = false;          // put the cursor on the popup's first button (it changed page)
#endif
        };

        // GOD HAND's centre in the logo's canvas (tools/logo/export_launcher_logo.py prints it); the burst is centred
        // there. kRearmedRect: the REARMED plate (left, top, right, bottom as fractions of the canvas, same script).
        constexpr float kLogoCentreX = 0.5048f, kLogoCentreY = 0.5197f;
        constexpr float kRearmedRect[4] = {0.3110f, 0.7152f, 0.6961f, 0.8930f};
        // Play: the plate's animation, after the game's roulette pick (K's capture at 1/8 speed): the letters
        // heat up and bloom, a yellow bar shoots out both ways, the bar thins to streaks, all gone at 0.28 s;
        // the rest of the launcher fades to black meanwhile and the game starts at kPlayLength.
        constexpr float kPlayLength = 0.34f;
        // The game's menu sounds (se/core.ses on the disc; K's picks): the cursor's tick when the keyboard or gamepad
        // moves it (0x164), confirm (0x15E: buttons, boxes, tabs, dropdown picks; clicks too), cancel (0x161: Back, B /
        // Esc), Play (0x160, the main menu's confirm; it rings on into the game's boot: the launcher leaves the audio
        // device open for the game). The volume slider makes none.
        enum MenuSound
        {
            kTick,
            kConfirm,
            kCancel,
            kPlay
        };
        constexpr int kMenuSoundIds[4] = {0x164, 0x15E, 0x161, 0x160};
        constexpr float kMusicVolume = 0.6f; // under the player's volume

        // A sound of the game's SE bank (.ses: "SES\0", f32 version; u32s at +0x10.. = sections: +0x10 the sound table
        // (u32 count, then from +0x20 0x30-byte entries: +4 u16 tone, +0xF 0x80 = unused, +0x10 u8 volume 0..127), +0x20
        // the tone table (16 bytes each: u32 offset into the sample data, u32 sample rate), +0x28 / +0x2C the sample
        // data (PS-ADPCM)) as 16-bit mono PCM. False when the bank or the sound is not what that describes.
        bool decodeSesSound(const std::vector<uint8_t> &ses, int id, std::vector<int16_t> &pcm, int &rate, float &volume)
        {
            auto u32 = [&](size_t o) -> uint32_t {
                return o + 4 > ses.size() ? 0u : static_cast<uint32_t>(ses[o]) | (static_cast<uint32_t>(ses[o + 1]) << 8) |
                                                 (static_cast<uint32_t>(ses[o + 2]) << 16) | (static_cast<uint32_t>(ses[o + 3]) << 24);
            };
            if (ses.size() < 0x40 || std::memcmp(ses.data(), "SES", 4) != 0)
                return false;
            const uint32_t table = u32(0x10), tones = u32(0x20), toneBytes = u32(0x24), data = u32(0x28), dataSize = u32(0x2C);
            if (id < 0 || static_cast<uint32_t>(id) >= u32(table) || data + static_cast<uint64_t>(dataSize) > ses.size())
                return false;
            const size_t entry = table + 0x20 + 0x30 * static_cast<size_t>(id);
            if (entry + 0x30 > ses.size() || (ses[entry + 0xF] & 0x80) != 0)
                return false;
            const uint32_t tone = static_cast<uint32_t>(ses[entry + 4]) | (static_cast<uint32_t>(ses[entry + 5]) << 8);
            if (16ull * (tone + 1) > toneBytes)
                return false;
            const uint32_t offset = u32(tones + 16 * tone);
            rate = static_cast<int>(u32(tones + 16 * tone + 4));
            volume = std::clamp(static_cast<float>(ses[entry + 0x10]) / 127.0f, 0.0f, 1.0f);
            if (rate < 4000 || rate > 96000 || offset >= dataSize)
                return false;
            // PS-ADPCM: 16-byte frames (shift | filter << 4, flags (bit 0: last frame), 14 bytes = 28 samples).
            static constexpr int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
            int s1 = 0, s2 = 0;
            pcm.clear();
            for (size_t p = data + offset; p + 16 <= data + static_cast<size_t>(dataSize); p += 16)
            {
                const int shift = ses[p] & 15, filter = std::min(ses[p] >> 4, 4);
                for (int i = 0; i < 28; ++i)
                {
                    const uint8_t b = ses[p + 2 + i / 2];
                    int v = (i & 1) ? (b >> 4) : (b & 15);
                    if (v > 7)
                        v -= 16;
                    v = shift <= 12 ? (v << 12) >> shift : 0;
                    v += (s1 * kFilter[filter][0] + s2 * kFilter[filter][1]) >> 6; // floor, as the SPU2
                    v = std::clamp(v, -32768, 32767);
                    pcm.push_back(static_cast<int16_t>(v));
                    s2 = s1;
                    s1 = v;
                }
                if ((ses[p + 1] & 1) != 0)
                    break;
            }
            return !pcm.empty();
        }

        // The menu sounds from the player's own disc (never shipped): loaded at the start and after an install.
        void loadMenuSounds(State &state, const fs::path &image)
        {
            if (!state.audio || state.hasMenuSound[kPlay] || image.empty())
                return;
            std::vector<uint8_t> ses;
            if (!disc_files::readFromImage(image, "se/core.ses", ses))
            {
                KM_INFO(Boot, "launcher: no menu sounds (se/core.ses not read from the disc image)");
                return;
            }
            for (int i = 0; i < 4; ++i)
            {
                std::vector<int16_t> pcm;
                int rate = 0;
                float volume = 1.0f;
                if (!decodeSesSound(ses, kMenuSoundIds[i], pcm, rate, volume))
                {
                    KM_INFO(Boot, "launcher: menu sound 0x" << std::hex << kMenuSoundIds[i] << std::dec << " not in se/core.ses");
                    continue;
                }
                Wave wave{};
                wave.frameCount = static_cast<unsigned int>(pcm.size());
                wave.sampleRate = static_cast<unsigned int>(rate);
                wave.sampleSize = 16;
                wave.channels = 1;
                wave.data = pcm.data();
                state.menuSounds[i] = LoadSoundFromWave(wave); // copies the samples
                state.hasMenuSound[i] = state.menuSounds[i].frameCount > 0;
                state.menuSoundBase[i] = volume;
            }
        }

        float userVolume(const State &state) { return state.muted ? 0.0f : state.volume; }

        // The launcher's own sound effects (resources/sfx/launcher, K's): one shot, restarted if still playing; a little
        // over the files' own level (K: the effects up a bit).
        constexpr float kEffectGain = 1.4f;

        void playEffect(State &state, Sound &sound, bool loaded, float level = 1.0f)
        {
            if (!loaded)
                return;
            SetSoundVolume(sound, kEffectGain * level * userVolume(state));
            PlayAudioStream(sound.stream); // not PlaySound (winmm's, see playMenuSound)
        }

        // The pencil loop drawn round something (the bill, the slip): its scribble instead of the cursor's tick.
        void penSound(State &state)
        {
            playEffect(state, state.pen, state.hasPen);
            state.penThisFrame = state.hasPen;
        }

        void menuSound(State &state, MenuSound which);

        // Into the book and back out (K, Nov 18): the whoosh (the menu's cancel sound) with the book opening or closing,
        // instead of the confirm. (menuSound marks the frame's sound as played.)
        void bookSound(State &state, bool open)
        {
            menuSound(state, kCancel);
            if (open)
                playEffect(state, state.bookOpen, state.hasBookOpen);
            else
                playEffect(state, state.bookClose, state.hasBookClose);
        }

        // A snack eaten: its sound (the slurp for the drink, a bite for the rest; K's), Gene's line halfway through it;
        // without the sound, the line at once.
        float soundLength(const Sound &sound) { return sound.stream.sampleRate > 0 ? static_cast<float>(sound.frameCount) / sound.stream.sampleRate : 0.0f; }
        void snackVoice(State &state);
        void eatSound(State &state, bool drink)
        {
            Sound &sound = drink ? state.slurp : state.bite;
            const bool loaded = drink ? state.hasSlurp : state.hasBite;
            if (!loaded)
            {
                snackVoice(state);
                return;
            }
            playEffect(state, sound, true, 2.0f); // (K: twice the files' level, against Gene's voice)
            state.voiceAt = static_cast<float>(GetTime()) + soundLength(sound) * 0.5f; // into its quiet roll-off (K)
        }

        void playMenuSound(State &state, MenuSound which, float scale = 1.0f)
        {
            if (!state.hasMenuSound[which])
                return;
            SetSoundVolume(state.menuSounds[which], state.menuSoundBase[which] * userVolume(state) * scale);
            // Not PlaySound: GHR.exe links winmm with /FORCE:MULTIPLE, and winmm's PlaySound took the call (no sound,
            // no error). raylib's PlaySound is exactly this.
            PlayAudioStream(state.menuSounds[which].stream);
        }

        uint32_t fnv1a(std::string_view text, uint32_t hash = 2166136261u)
        {
            for (const unsigned char c : text)
                hash = (hash ^ c) * 16777619u;
            return hash;
        }

        // One pick from a shuffled deck kept in GHR.ini ([Launcher] key = "<list id>:<last>:<rest, ...>", K Nov 10):
        // every entry comes once, in random order, before any comes back; a new deck never starts with the one just
        // used; a changed list (another id: a splash line added) starts a new deck.
        int dealFromDeck(State &state, const char *key, int count, uint32_t listId)
        {
            if (count <= 0)
                return -1;
            char id[16];
            std::snprintf(id, sizeof(id), "%08x", listId);
            const std::string saved = state.ini.get("Launcher", key, "");
            int last = -1;
            std::vector<int> rest;
            const size_t c1 = saved.find(':'), c2 = c1 == std::string::npos ? c1 : saved.find(':', c1 + 1);
            if (c2 != std::string::npos && saved.compare(0, c1, id) == 0)
            {
                last = std::atoi(saved.substr(c1 + 1, c2 - c1 - 1).c_str());
                std::string list = saved.substr(c2 + 1);
                for (size_t at = 0; at < list.size();)
                {
                    const size_t comma = std::min(list.find(',', at), list.size());
                    const std::string item = list.substr(at, comma - at);
                    const int v = item.empty() ? -1 : std::atoi(item.c_str());
                    if (v >= 0 && v < count && std::find(rest.begin(), rest.end(), v) == rest.end())
                        rest.push_back(v);
                    at = comma + 1;
                }
            }
            if (rest.empty())
            {
                for (int i = 0; i < count; ++i)
                    rest.push_back(i);
                std::mt19937 random(std::random_device{}() ^ static_cast<unsigned>(std::time(nullptr)));
                std::shuffle(rest.begin(), rest.end(), random);
                if (count > 1 && rest.front() == last)
                    std::swap(rest[0], rest[1]);
            }
            const int pick = rest.front();
            rest.erase(rest.begin());
            std::string value = std::string(id) + ":" + std::to_string(pick) + ":";
            for (size_t i = 0; i < rest.size(); ++i)
                value += (i ? "," : "") + std::to_string(rest[i]);
            state.ini.set("Launcher", key, value);
            state.ini.save();
            return pick;
        }

        // The start bark: deals this start's line and renders it in the background (with the music's reverb and the
        // music itself, for the tail on Play). Once per start, when the disc image is known.
        void startBark(State &state, const fs::path &image)
        {
            if (!state.audio || state.barkStarted || image.empty())
                return;
            state.barkStarted = true;
            uint32_t listId = 2166136261u;
            for (int i = 0; i < launcher_bark::count(); ++i)
                listId = fnv1a(std::string(launcher_bark::name(i)) + "\n", listId);
            state.barkLine = dealFromDeck(state, "BarkDeck", launcher_bark::count(), listId);
            const fs::path musicPath = state.hasMusic ? state.musicPath : fs::path();
            state.barkWorker = std::thread([&state, image, musicPath, line = state.barkLine] {
                const launcher_bark::ReadFile read = [&image](std::string_view path, std::vector<uint8_t> &bytes) {
                    return disc_files::readFromImage(image, path, bytes);
                };
                if (!launcher_bark::render(line, read, state.barkPcm))
                    state.barkPcm.lr.clear();
                if (musicPath.empty())
                    return;
                Wave wave = LoadWave(utf8(musicPath).c_str());
                if (wave.data != nullptr && wave.frameCount > 0)
                {
                    WaveFormat(&wave, static_cast<int>(wave.sampleRate), 16, 2);
                    const int16_t *pcm = static_cast<const int16_t *>(wave.data);
                    state.musicPcm.assign(pcm, pcm + static_cast<size_t>(wave.frameCount) * 2);
                    state.musicRate = static_cast<int>(wave.sampleRate);
                    launcher_bark::prepareWash(state.wash);
                }
                UnloadWave(wave);
            });
        }

        Sound soundFrom(const launcher_bark::Stereo &pcm)
        {
            std::vector<int16_t> samples(pcm.lr.size());
            for (size_t i = 0; i < samples.size(); ++i)
                samples[i] = static_cast<int16_t>(std::clamp(pcm.lr[i], -1.0f, 1.0f) * 32767.0f);
            Wave wave{};
            wave.frameCount = static_cast<unsigned int>(pcm.frames());
            wave.sampleRate = launcher_bark::kRate;
            wave.sampleSize = 16;
            wave.channels = 2;
            wave.data = samples.data();
            return LoadSoundFromWave(wave); // copies the samples
        }

        // Play: its own sound (lower under a bark, as K approved the mix), the bark, and the music's tail in place of
        // the music: it closes down and washes out under the voice, all faded out right after the last word. The
        // sounds ring on into the game's boot (the audio device is left to the game, as for Play's sound).
        void playBark(State &state)
        {
            if (state.barkWorker.joinable())
                state.barkWorker.join(); // ready long before Play, unless Play came at once
            const bool voice = !state.barkPcm.lr.empty();
            playMenuSound(state, kPlay, voice ? 0.55f : 1.0f);
            const auto start = std::chrono::steady_clock::now();
            float end = 1.5f;
            if (voice)
            {
                const Sound bark = soundFrom(state.barkPcm);
                SetSoundVolume(bark, userVolume(state));
                PlayAudioStream(bark.stream); // not PlaySound (winmm's, see playMenuSound)
                end = static_cast<float>(state.barkPcm.frames()) / launcher_bark::kRate;
            }
            if (state.hasMusic && !state.musicPcm.empty() && state.wash.fftSize != 0)
            {
                const size_t frames = state.musicPcm.size() / 2;
                const size_t from = static_cast<size_t>(GetMusicTimePlayed(state.music) * state.musicRate) % frames;
                launcher_bark::Stereo tail;
                launcher_bark::musicTail(state.musicPcm.data(), frames, state.musicRate, from, end, state.wash, tail);
                if (!tail.lr.empty())
                {
                    const Sound sound = soundFrom(tail);
                    SetSoundVolume(sound, kMusicVolume * userVolume(state));
                    StopMusicStream(state.music);
                    PlayAudioStream(sound.stream);
                    state.musicHandedOff = true;
                }
            }
            KM_INFO(Boot, "launcher: start bark " << (voice ? launcher_bark::name(state.barkLine) : "none (not read from the disc)")
                                                  << ", music tail " << (state.musicHandedOff ? "on" : "off") << " ("
                                                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
                                                  << " ms)");
        }

        // The snacks' voice lines (K, Nov 18): Gene's dry lines from se/pl00.afs (even entries; the odd ones are their
        // echo twins), read from the player's disc like the bark. Dealt from a shuffled deck; one at a time (a bite
        // while one plays is silent: no overlap, no queue).
        constexpr int kSnackLines[] = {0, 2, 4, 6, 8, 10, 12}; // Wow, Nice, I love it, Awesome, Cool, Well how, Great (no Shit, K)

        void loadSnackVoices(State &state, const fs::path &image)
        {
            if (!state.audio || !state.snackVoices.empty() || image.empty())
                return;
            std::vector<uint8_t> afs;
            if (!disc_files::readFromImage(image, "se/pl00.afs", afs))
            {
                KM_INFO(Boot, "launcher: no snack lines (se/pl00.afs not read from the disc image)");
                return;
            }
            for (const int entry : kSnackLines)
            {
                launcher_bark::Stereo pcm;
                if (launcher_bark::voiceLine(afs, entry, pcm))
                    state.snackVoices.push_back(soundFrom(pcm));
            }
        }

        void snackVoice(State &state)
        {
            if (state.snackVoices.empty())
                return;
            if (state.snackVoice >= 0 && IsAudioStreamPlaying(state.snackVoices[static_cast<size_t>(state.snackVoice)].stream))
                return;
            const int count = static_cast<int>(state.snackVoices.size());
            const int pick = dealFromDeck(state, "SnackDeck", count, fnv1a("se/pl00.afs:0,2,4,6,8,10,12\n"));
            if (pick < 0 || pick >= count)
                return;
            Sound &sound = state.snackVoices[static_cast<size_t>(pick)];
            SetSoundVolume(sound, 0.65f * userVolume(state)); // under the effects (K)
            PlayAudioStream(sound.stream); // not PlaySound (winmm's, see playMenuSound)
            state.snackVoice = pick;
        }

        void menuSound(State &state, MenuSound which)
        {
            playMenuSound(state, which);
            state.soundThisFrame = true;
            state.quietFrames = 2;
        }

        // The frame's sounds from what ImGui did (after the launcher's window): an item activated (click or A / Enter:
        // confirm; not the volume slider), a dropdown closed with B / Esc (cancel), the keyboard / gamepad cursor moved
        // to another item (tick; mouse hovering moves nothing). Sounds the screens play themselves (Back, Play) win.
        void frameSounds(State &state)
        {
            ImGuiContext &g = *GImGui;
            const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            const bool backKey = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            if (state.playTime < 0.0f && !state.soundThisFrame)
            {
                if (state.popupWasOpen && !popupOpen && backKey)
                    menuSound(state, kCancel);
                else if (g.ActiveIdIsJustActivated && g.ActiveId != 0 && g.ActiveId != state.volumeSliderId &&
                         std::find(g_silentItems.begin(), g_silentItems.end(), g.ActiveId) == g_silentItems.end())
                {
                    const auto listed = [&](const std::vector<ImGuiID> &items) {
                        return std::find(items.begin(), items.end(), g.ActiveId) != items.end();
                    };
                    const bool inPopup = g.ActiveIdWindow != nullptr && (g.ActiveIdWindow->Flags & ImGuiWindowFlags_Popup) != 0;
                    if (listed(g_ownSoundItems)) // Back / Play / a popup's buttons play their own sound when let go
                        ;
                    else if (listed(g_tickItems) || inPopup) // a box, a dropdown or a pick from its list
                    {
                        playMenuSound(state, kTick);
                        state.quietFrames = 2; // the dropdown's list opening moves the cursor
                    }
                    else
                        menuSound(state, kConfirm);
                }
                else if (g.NavId != state.lastNavId && (state.lastNavId != 0 || state.navCleared) && g.NavId != 0 && g.NavCursorVisible &&
                         !state.penThisFrame &&
                         state.quietFrames == 0 && state.soundScreen == state.screen)
                    playMenuSound(state, kTick);
            }
            if (state.quietFrames > 0 && !state.soundThisFrame)
                --state.quietFrames;
            if (g.NavId != 0)
                state.navCleared = false;
            state.lastNavId = g.NavId;
            state.popupWasOpen = popupOpen;
            state.soundScreen = state.screen;
        }


        void updateMotion(State &state, Screen screen)
        {
            const float dt = std::min(GetFrameTime(), 0.1f);
            state.time += dt;
            const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
            ImVec2 target(0.0f, 0.0f);
            if (IsCursorOnScreen() && width > 0.0f && height > 0.0f)
            {
                const Vector2 mouse = GetMousePosition();
                target.x = std::clamp((mouse.x - width * 0.5f) / (width * 0.5f), -1.0f, 1.0f);
                target.y = std::clamp((mouse.y - height * 0.5f) / (height * 0.5f), -1.0f, 1.0f);
            }
            // The saloon: the room snaps to centre on the zoom's first frame and holds still through it and Options;
            // back at Home it eases in again (K).
            if (state.saloonOn && (state.scene.zoomFrame() > 0 || screen == Screen::Options))
            {
                state.parallax = ImVec2(0.0f, 0.0f);
                target = ImVec2(0.0f, 0.0f);
            }
            const float ease = 1.0f - std::exp(-dt * 3.0f);
            state.parallax.x += (target.x - state.parallax.x) * ease;
            state.parallax.y += (target.y - state.parallax.y) * ease;
            const float shown = screen == Screen::Options ? 0.0f : 1.0f;
            state.burstShown += (shown - state.burstShown) * (1.0f - std::exp(-dt * 5.0f));
        }

        // One layer of the burst: the texture centred at (x, y), radius in pixels, squashed vertically, turned.
        void drawBurstLayer(const Texture2D &burst, float x, float y, float radius, float angle, unsigned char alpha)
        {
            const float size = radius * 2.0f;
            rlPushMatrix();
            rlTranslatef(x, y, 0.0f);
            rlScalef(1.0f, 0.75f, 1.0f);
            rlRotatef(angle, 0.0f, 0.0f, 1.0f);
            DrawTexturePro(burst, Rectangle{0.0f, 0.0f, static_cast<float>(burst.width), static_cast<float>(burst.height)},
                           Rectangle{0.0f, 0.0f, size, size}, Vector2{radius, radius}, 0.0f, Color{255, 255, 255, alpha});
            rlPopMatrix();
        }

        void drawBurst(const State &state)
        {
            if (state.burst.id == 0u || state.burstShown < 0.01f)
                return;
            const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
            const float diagonal = std::hypot(width, height);
            float x = width * 0.5f, y = height * 0.3f;
            if (state.logoPlaced)
            {
                x = state.logoMin.x + (state.logoMax.x - state.logoMin.x) * kLogoCentreX;
                y = state.logoMin.y + (state.logoMax.y - state.logoMin.y) * kLogoCentreY;
            }
            const float unit = state.unit, t = state.time, shown = state.burstShown;
            const ImVec2 p = state.parallax;
            if (state.saloonOn) // in the doorway's glare: sized by the logo, drifting with the room
            {
                const float r = (state.logoMax.x - state.logoMin.x) * 0.77f * state.scene.tuning().burst;
                drawBurstLayer(state.burst, x, y, r * (1.0f + 0.02f * std::sin(t * 1.1f + 1.0f)), t * 1.2f,
                               static_cast<unsigned char>(45.0f * shown));
                drawBurstLayer(state.burst, x, y, r * 0.78f * (1.0f + 0.03f * std::sin(t * 1.6f)), 40.0f - t * 2.0f,
                               static_cast<unsigned char>(70.0f * shown));
                return;
            }
            // Back layer: bigger, dimmer, turns one way and shifts the most; front layer: the other way, less.
            drawBurstLayer(state.burst, x - p.x * 36.0f * unit, y - p.y * 36.0f * unit,
                           diagonal * 0.80f * (1.0f + 0.02f * std::sin(t * 1.1f + 1.0f)), t * 1.2f,
                           static_cast<unsigned char>(45.0f * shown));
            drawBurstLayer(state.burst, x - p.x * 18.0f * unit, y - p.y * 18.0f * unit,
                           diagonal * 0.62f * (1.0f + 0.03f * std::sin(t * 1.6f)), 40.0f - t * 2.0f,
                           static_cast<unsigned char>(70.0f * shown));
        }

        float fontSize(const State &state, float base) { return std::round(base * state.unit); }

        void pushSize(const State &state, float base) { ImGui::PushFont(state.font, fontSize(state, base)); }

        // The splash text (Minecraft's): a random line of resources/splash/splash.txt, one per start, yellow with a hard
        // shadow, tilted up across the wordmark's bottom right corner and pulsing. Long lines are drawn smaller, so
        // each stays within 40% of the wordmark's width. Over the screens with the logo (Home, Install), fading out with
        // the burst on Options.
        void loadSplash(State &state, const fs::path &folder)
        {
            std::ifstream in(folder / "splash.txt", std::ios::binary);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(in, line))
            {
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF)
                    line.erase(0, 3); // a UTF-8 byte order mark (Notepad)
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                    line.pop_back();
                size_t start = 0;
                while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])))
                    ++start;
                if (start < line.size())
                    lines.push_back(line.substr(start));
            }
            // The font is a baked atlas (tools/splash/bake_font.py: splash.fnt + splash.png, Latin-1), not the .ttf:
            // its terms keep the raw file out of the repo, and raylib's own atlas packer clipped and garbled this
            // font's tall glyphs (it assumes none is taller than the font size).
            std::error_code error;
            const fs::path fontPath = folder / "splash.fnt";
            if (lines.empty() || !fs::is_regular_file(fontPath, error))
                return;
            uint32_t listId = 2166136261u;
            for (const std::string &l : lines)
                listId = fnv1a(l + "\n", listId);
            state.splash = lines[static_cast<size_t>(dealFromDeck(state, "SplashDeck", static_cast<int>(lines.size()), listId))];
            state.splashFont = LoadFont(utf8(fontPath).c_str());
            if (state.splashFont.texture.id == 0u || state.splashFont.texture.id == GetFontDefault().texture.id)
            {
                state.splash.clear();
                return;
            }
            GenTextureMipmaps(&state.splashFont.texture);
            SetTextureFilter(state.splashFont.texture, TEXTURE_FILTER_TRILINEAR);
        }

        void drawSplash(const State &state)
        {
            if (state.splash.empty() || !state.logoPlaced || state.burstShown < 0.01f)
                return;
            const float logoWidth = state.logoMax.x - state.logoMin.x, logoHeight = state.logoMax.y - state.logoMin.y;
            const char *text = state.splash.c_str();
            float size = logoWidth * 0.047f;
            const Vector2 measured = MeasureTextEx(state.splashFont, text, size, 0.0f);
            if (measured.x > logoWidth * 0.40f)
                size *= logoWidth * 0.40f / measured.x;
            const Vector2 at{state.logoMin.x + logoWidth * 0.90f, state.logoMin.y + logoHeight * 0.68f}; // on the D's lower right
            // A line too long for the room to the window's right edge (a 4:3 window) tilts steeper, up to 40 degrees,
            // then shrinks (K, Nov 13); judged at the pulse's largest so it holds still.
            float tilt = 18.0f;
            {
                const Vector2 peak = MeasureTextEx(state.splashFont, text, size * 1.05f, 0.0f);
                const float room = static_cast<float>(GetScreenWidth()) - at.x - logoWidth * 0.02f;
                const auto reach = [&](float degrees) {
                    const float r = degrees * 3.14159265f / 180.0f;
                    return 0.5f * peak.x * std::cos(r) + 0.5f * peak.y * std::sin(r);
                };
                while (tilt < 40.0f && reach(tilt) > room)
                    tilt += 1.0f;
                if (room > 1.0f && reach(tilt) > room)
                    size *= room / reach(tilt);
            }
            size *= 1.0f + 0.05f * std::fabs(std::sin(state.time * 3.14159265f)); // the pulse, once a second
            const Vector2 box = MeasureTextEx(state.splashFont, text, size, 0.0f);
            const Vector2 origin{box.x * 0.5f, box.y * 0.5f};
            const float shadow = size * 0.05f; // closer to the letters (K, Nov 13; was 0.085)
            const unsigned char alpha = static_cast<unsigned char>(255.0f * std::clamp(state.burstShown, 0.0f, 1.0f));
            DrawTextPro(state.splashFont, text, Vector2{at.x + shadow, at.y + shadow}, origin, -tilt, size, 0.0f,
                        Color{62, 52, 0, alpha});
            DrawTextPro(state.splashFont, text, at, origin, -tilt, size, 0.0f, Color{255, 236, 64, alpha});
        }

        // Text straight on the background, centred, with a soft dark shadow so it reads over the bright desert.
        void centeredText(const char *text, const ImVec4 &color)
        {
            const float width = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const float size = ImGui::GetFontSize(), r = std::max(1.0f, size * 0.09f);
            for (int ring = 1; ring <= 2; ++ring)
                for (int i = 0; i < 12; ++i)
                {
                    const float a = static_cast<float>(i) * 0.5236f;
                    draw->AddText(ImVec2(at.x + std::cos(a) * r * ring, at.y + r * 0.6f + std::sin(a) * r * ring),
                                  IM_COL32(10, 6, 4, ring == 1 ? 46 : 22), text);
                }
            ImGui::TextColored(color, "%s", text);
        }

        bool bigButton(const State &state, const char *label, float width, bool accent)
        {
            if (accent)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.04f, 1.0f));
            }
            pushSize(state, 30.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            const bool clicked = ImGui::Button(label, ImVec2(width, fontSize(state, 64.0f)));
            ImGui::PopFont();
            if (accent)
                ImGui::PopStyleColor(2);
            return clicked;
        }

        unsigned char alphaOf(float a) { return static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f); }

        // ---- the saloon's paper UI (launcher_saloon.h) ---------------------------------------------------------------
        // An invisible button over a box on a paper (the bill, a slip): ImGui gets the box's inscribed rectangle on
        // screen (keyboard / gamepad focus, clicks); moving the mouse over the box itself (exactly, through the paper's
        // perspective) moves the focus there (K: hover moves the focus), without the cursor's tick.
        saloon::Quad paperQuad(Rectangle box, const saloon::Homography &paper)
        {
            saloon::Quad q;
            q.p[0] = paper.map(box.x, box.y);
            q.p[1] = paper.map(box.x + box.width, box.y);
            q.p[2] = paper.map(box.x + box.width, box.y + box.height);
            q.p[3] = paper.map(box.x, box.y + box.height);
            return q;
        }

        bool paperButton(const char *id, Rectangle box, const saloon::Homography &paper, bool hoverFocus, bool *focused = nullptr)
        {
            const saloon::Quad q = paperQuad(box, paper);
            // ImGui's box: the item's middle, as wide as its middle line and as tall as its middle column (a long line
            // on a tilted paper would have no inner box, and its outer one would reach over its neighbours).
            const Vector2 c = paper.map(box.x + box.width * 0.5f, box.y + box.height * 0.5f);
            const Vector2 l = paper.map(box.x, box.y + box.height * 0.5f), r = paper.map(box.x + box.width, box.y + box.height * 0.5f);
            const Vector2 t = paper.map(box.x + box.width * 0.5f, box.y), b = paper.map(box.x + box.width * 0.5f, box.y + box.height);
            const float halfW = std::max(1.0f, std::fabs(r.x - l.x) * 0.5f), halfH = std::max(1.0f, std::fabs(b.y - t.y) * 0.5f);
            const float x0 = c.x - halfW, x1 = c.x + halfW, y0 = c.y - halfH, y1 = c.y + halfH;
            ImGui::SetCursorScreenPos(ImVec2(x0, y0));
            // EnableNav: an invisible button is no keyboard / gamepad stop unless asked; ImGui's own focus frame hidden
            // (the pencil loop shows the focus)
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, y1 - y0)), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            const ImGuiIO &io = ImGui::GetIO();
            const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
            if (hoverFocus && moved && !ImGui::IsItemFocused() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                saloon::inside(q, Vector2{io.MousePos.x, io.MousePos.y}))
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            if (focused != nullptr)
                *focused = ImGui::IsItemFocused();
            return pressed;
        }

        // The corner slip (K, Nov 17): a paper slip poking up from the picture's bottom right edge, in dark ink: the
        // version / update (Add to Steam and the volume moved onto the bill, K Nov 18). A slip's own units: x right, y down; about
        // kSlipShown of its height shows above the edge.
        enum SlipItem
        {
            kSlipNone,
            kSlipNotes,
            kSlipUpdate,
            kSlipPaper, // the newspaper's corner (claude/newspaper.md)
        };
        constexpr float kSlipH = 240.0f, kSlipShown = 168.0f;
        constexpr Color kSlipInk{40, 28, 20, 238};
        constexpr Color kSlipPencil{164, 30, 22, 220};
        constexpr float kSlipPi = 3.14159265f;
        struct Slip
        {
            saloon::Homography paper;
            saloon::Quad quad;
            float w = 0.0f, h = kSlipH;
        };
        Slip slipAt(const State &state, bool right)
        {
            const Rectangle pic = state.scene.picture();
            const float k = pic.height / 900.0f * 0.56f; // screen pixels a slip unit
            Slip slip;
            slip.w = 420.0f;
            const float angle = (right ? -1.4f : 1.8f) * DEG2RAD, c = std::cos(angle), sn = std::sin(angle);
            const float margin = pic.height * 0.03f;
            const float cx = right ? pic.x + pic.width - margin - slip.w * k * 0.5f : pic.x + margin + slip.w * k * 0.5f;
            const float cy = pic.y + pic.height - kSlipShown * k + slip.h * k * 0.5f;
            const float hw = slip.w * 0.5f, hh = slip.h * 0.5f;
            const Vector2 corners[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
            for (int i = 0; i < 4; ++i)
                slip.quad.p[i] = Vector2{cx + (corners[i].x * c - corners[i].y * sn) * k, cy + (corners[i].x * sn + corners[i].y * c) * k};
            slip.paper = saloon::Homography::rectToQuad(slip.w, slip.h, slip.quad);
            return slip;
        }

        // A slip's paper: a soft shadow, then the paper (its rim a little darker), faded by `alpha`.
        void drawSlipPaper(const State &state, const Slip &slip, float alpha)
        {
            const Texture2D &paper = state.scene.paper();
            if (alpha <= 0.0f || paper.id == 0u)
                return;
            // the shadow: three widening quads, down and out a little
            const Vector2 centre{(slip.quad.p[0].x + slip.quad.p[2].x) * 0.5f, (slip.quad.p[0].y + slip.quad.p[2].y) * 0.5f};
            for (int ring = 3; ring >= 1; --ring)
            {
                const float grow = 3.0f * static_cast<float>(ring);
                Vector2 q[4];
                for (int i = 0; i < 4; ++i)
                {
                    const float dx = slip.quad.p[i].x - centre.x, dy = slip.quad.p[i].y - centre.y, d = std::max(1.0f, std::hypot(dx, dy));
                    q[i] = Vector2{slip.quad.p[i].x + dx / d * grow + 2.0f, slip.quad.p[i].y + dy / d * grow + 4.0f};
                }
                const Color shade{0, 0, 0, alphaOf(0.10f * alpha)};
                DrawTriangle(q[0], q[3], q[2], shade);
                DrawTriangle(q[0], q[2], q[1], shade);
            }
            // the paper, 4x4 cells: the rim's vertices darker
            constexpr int kCells = 4;
            const float tile = 300.0f; // slip units a tile of the paper texture
            rlDisableBackfaceCulling();
            rlSetTexture(paper.id);
            rlBegin(RL_QUADS);
            for (int y = 0; y < kCells; ++y)
                for (int x = 0; x < kCells; ++x)
                {
                    const int corners[4][2] = {{x, y}, {x, y + 1}, {x + 1, y + 1}, {x + 1, y}};
                    for (const auto &corner : corners)
                    {
                        const float u = slip.w * static_cast<float>(corner[0]) / kCells, v = slip.h * static_cast<float>(corner[1]) / kCells;
                        const bool rim = corner[0] == 0 || corner[0] == kCells || corner[1] == 0;
                        const float shade = rim ? 0.80f : 1.0f;
                        rlColor4ub(alphaOf(shade), alphaOf(shade * 0.96f), alphaOf(shade * 0.92f), alphaOf(alpha));
                        rlTexCoord2f(u / tile, v / tile);
                        const Vector2 at = slip.paper.map(u, v);
                        rlVertex2f(at.x, at.y);
                    }
                }
            rlEnd();
            rlSetTexture(0);
            rlDrawRenderBatchActive();
            rlEnableBackfaceCulling();
        }

        // What sits on the slips, in slip units.
        constexpr Vector2 kNotesAt{110.0f, 140.0f}, kUpdateAt{285.0f, 140.0f};

        Rectangle slipTextBox(const Font &font, const char *text, float size, Vector2 centre)
        {
            const float w = saloon::inkWidth(font, text, size);
            return Rectangle{centre.x - w * 0.5f - 14.0f, centre.y - size * 0.6f, w + 28.0f, size * 1.2f};
        }

        bool updateAvailable() { return g_updateCheck.state.load() == static_cast<int>(update::CheckState::Available); }
#if KAMITE_LAUNCHER_STEAM
        const char *steamLabel(const State &state) { return state.steam.added ? "Update Steam Entry" : "Add to Steam"; }
#endif

        // The slips' ink (before ImGui, from last frame's focus): their words, the speaker, the ruler, the loop.
        void drawSlipInk(const State &state, const Slip &slip, bool right, float alpha)
        {
            const saloon::BillFonts &f = state.billFonts;
            Color ink = kSlipInk, pencil = kSlipPencil;
            ink.a = alphaOf(ink.a / 255.0f * alpha);
            pencil.a = alphaOf(pencil.a / 255.0f * alpha);
            const auto loop = [&](Rectangle box, uint32_t seed) { saloon::inkLoop(box, 1.0f, seed, pencil, slip.paper, 4.0f); };
            saloon::beginInk();
            if (right)
            {
                const std::string version = update::versionLabel();
                const float vw = saloon::inkWidth(f.text, version.c_str(), 32.0f);
                const float scale = vw > 340.0f ? 340.0f / vw : 1.0f; // a dev build's long label
                saloon::inkText(f.text, version.c_str(), Vector2{40.0f + vw * scale * 0.5f, 48.0f}, 32.0f * scale, ink, slip.paper);
                if (updateAvailable() && state.screen == Screen::Home)
                {
                    const std::string out = g_updateCheck.release.tag + " is out";
                    const float ow = saloon::inkWidth(f.text, out.c_str(), 26.0f);
                    saloon::inkText(f.text, out.c_str(), Vector2{40.0f + ow * 0.5f, 88.0f}, 26.0f, ink, slip.paper);
                    saloon::inkText(f.text, "What's new", kNotesAt, 34.0f, ink, slip.paper);
                    saloon::inkText(f.text, "Update", kUpdateAt, 34.0f, ink, slip.paper);
                    if (state.slipFocus == kSlipNotes)
                        loop(slipTextBox(f.text, "What's new", 34.0f, kNotesAt), 21u);
                    if (state.slipFocus == kSlipUpdate)
                        loop(slipTextBox(f.text, "Update", 34.0f, kUpdateAt), 22u);
                }
            }
            saloon::endInk();
        }

        // The lower right corner's volume: a mute button (a speaker, crossed out when muted) left of a slider; both kept
        // in GHR.ini.
        void drawVolume(State &state)
        {
            if (state.saloonOn) // on the bill (drawSaloonHome)
                return;

            const float size = fontSize(state, 34.0f), margin = fontSize(state, 22.0f), slider = fontSize(state, 200.0f);
            const float gap = fontSize(state, 8.0f);
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - margin - slider - gap - size, ImGui::GetWindowHeight() - margin - size));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##mute", ImVec2(size, size)))
            {
                state.muted = !state.muted;
                state.ini.setBool("Launcher", "Mute", state.muted);
                state.ini.save();
            }
            const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const ImU32 ink = ImGui::GetColorU32(lit ? kAccent : kText);
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const auto at = [&](float x, float y) { return ImVec2(p.x + x * size, p.y + y * size); };
            draw->AddRectFilled(at(0.14f, 0.38f), at(0.32f, 0.62f), ink);
            const ImVec2 cone[4] = {at(0.31f, 0.38f), at(0.54f, 0.18f), at(0.54f, 0.82f), at(0.31f, 0.62f)};
            draw->AddConvexPolyFilled(cone, 4, ink);
            const float stroke = std::max(1.5f, size * 0.07f);
            if (state.muted || state.volume <= 0.0f)
            {
                draw->AddLine(at(0.66f, 0.36f), at(0.88f, 0.64f), ink, stroke);
                draw->AddLine(at(0.66f, 0.64f), at(0.88f, 0.36f), ink, stroke);
            }
            else
                for (int wave = 0; wave < (state.volume > 0.5f ? 2 : 1); ++wave)
                {
                    draw->PathArcTo(at(0.54f, 0.5f), size * (0.17f + 0.15f * static_cast<float>(wave)), -0.85f, 0.85f, 12);
                    draw->PathStroke(ink, ImDrawFlags_None, stroke);
                }
            ImGui::SameLine(0.0f, gap);
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - margin - size + (size - ImGui::GetFrameHeight()) * 0.5f);
            ImGui::SetNextItemWidth(slider);
            float percent = state.volume * 100.0f;
            if (ImGui::SliderFloat("##volume", &percent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            {
                state.volume = percent / 100.0f;
                if (state.muted && state.volume > 0.0f)
                {
                    state.muted = false;
                    state.ini.setBool("Launcher", "Mute", false);
                }
            }
            state.volumeSliderId = ImGui::GetItemID();
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                char text[16];
                std::snprintf(text, sizeof(text), "%.2f", state.volume);
                state.ini.set("Launcher", "Volume", text);
                state.ini.save();
            }
        }

        // What's new: the release notes (CHANGELOG lines), "# v0.7.0" per release, "## <date>" headings.
        void drawNotes(const State &state, const std::string &notes)
        {
            size_t at = 0;
            while (at <= notes.size())
            {
                size_t end = notes.find('\n', at);
                if (end == std::string::npos)
                    end = notes.size();
                std::string line = notes.substr(at, end - at);
                at = end + 1;
                line.erase(std::remove(line.begin(), line.end(), '`'), line.end());
                for (size_t bold; (bold = line.find("**")) != std::string::npos;)
                    line.erase(bold, 2);
                if (line.rfind("# ", 0) == 0)
                {
                    pushSize(state, 26.0f);
                    ImGui::TextColored(kHeading, "%s", line.c_str() + 2);
                    ImGui::PopFont();
                }
                else if (line.rfind("## ", 0) == 0)
                    ImGui::TextColored(kTextDim, "%s", line.c_str() + 3);
                else if (!line.empty())
                    ImGui::TextWrapped("%s", line.c_str());
            }
        }

        void startUpdate(State &state)
        {
            // The launcher's music and the bar's murmur stream from resources: closed so the files can be replaced.
            if (state.hasMusic)
            {
                StopMusicStream(state.music);
                UnloadMusicStream(state.music);
                state.hasMusic = false;
            }
            if (state.hasAmbience)
            {
                StopMusicStream(state.ambience);
                UnloadMusicStream(state.ambience);
                state.hasAmbience = false;
            }
            update::startInstall(g_updater, g_updateCheck.release, kamite::paths::programDir()); // GHR.ini: the play folder
            state.updatePopup = state.paperOn ? State::UpdatePopup::None : State::UpdatePopup::Working; // (the paper shows it)
        }

        // ---- the newspaper (launcher_paper.h; claude/newspaper.md) --------------------------------------------------
        paper::Content paperContent(const State &state)
        {
            paper::Content c;
            c.edition = update::editionName();
            c.volume = KAMITE_VERSION;
            const auto check = static_cast<update::CheckState>(g_updateCheck.state.load());
            const std::string commit = update::devCommit();
            if (!commit.empty())
                c.date = commit; // a dev build: its commit where the date goes
            else if ((check == update::CheckState::UpToDate || check == update::CheckState::Available) && g_updateCheck.hasCurrent)
                c.date = g_updateCheck.current.date;
            if (check == update::CheckState::Available)
            {
                const update::Release &release = g_updateCheck.release;
                c.outNow = release.tag;
                c.updateAd = true;
                for (const update::Edition &e : release.editions)
                    c.stories.push_back(paper::story(e.tag, e.date, e.body));
            }
            else if (check == update::CheckState::UpToDate)
            {
                c.latest = true;
                if (g_updateCheck.hasCurrent)
                    c.stories.push_back(paper::story(g_updateCheck.current.tag, g_updateCheck.current.date, g_updateCheck.current.body));
                else
                    c.notice = "No news about this edition yet.";
            }
            else if (check == update::CheckState::Checking)
                c.notice = "Checking the wire\xE2\x80\xA6";
            else
                c.notice = "The wire's down. No news today.";
            if (state.paperPrinting)
            {
                const uint64_t got = g_updater.received.load(), total = g_updater.total;
                const bool installing = static_cast<update::Stage>(g_updater.stage.load()) != update::Stage::Downloading;
                c.printingKnown = installing || total > 0;
                c.printing = installing ? 1.0f : total > 0 ? std::min(1.0f, static_cast<float>(got) / static_cast<float>(total)) : 0.0f;
            }
            c.stop = state.paperStop;
            c.focus = state.paperT >= 1.0f ? state.paperFocus : -1;
            c.scroll = state.paperScroll;
            return c;
        }

        void openPaper(State &state)
        {
            state.paperOpen = true;
            state.paperFocusSet = false;
            state.paperScroll = 0.0f;
            bookSound(state, true); // the whoosh and the book's paper (K: never the closing one, it thumps)
        }

        // Home's newspaper: the corner is a button; open, a modal over everything holds the ads (B / Esc / BACK / a click
        // off the page close it; up / down, the sticks and the wheel scroll it). Its update runs on the paper.
        void drawPaper(State &state)
        {
            const paper::Content c = paperContent(state);
            const bool update = !c.outNow.empty();
            if (state.paperPrinting)
            {
                const auto stage = static_cast<update::Stage>(g_updater.stage.load());
                if (stage == update::Stage::Failed)
                {
                    if (g_updater.worker.joinable())
                        g_updater.worker.join();
                    state.paperPrinting = false;
                    state.paperStop = g_updater.error.empty() ? std::string("The update could not be installed.") : g_updater.error;
                }
                else if (stage == update::Stage::Done)
                {
                    if (g_updater.worker.joinable())
                        g_updater.worker.join();
                    KM_INFO(Boot, "launcher: restarting after the update");
                    platform::relaunch(state.exePath);
                    state.result = Result::Quit;
                    state.done = true;
                }
            }
            state.paperCornerId = 0;
            if (!state.paperOpen && state.paperT <= 0.0f && state.screen == Screen::Home)
            {
                const paper::View v = state.paper.view(0.0f, update);
                bool focused = false;
                if (paperButton("##paper", state.paper.cornerBox(update), v.map, true, &focused))
                    openPaper(state);
                state.paperCornerId = ImGui::GetItemID();
                ownSoundItem(); // the paper's own sound
                if (focused)
                    state.slipFocus = kSlipPaper;
            }
            if (state.paperOpen && !ImGui::IsPopupOpen("##paper"))
                ImGui::OpenPopup("##paper");
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(ImGui::GetIO().DisplaySize);
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0.0f, 0.0f, 0.0f, 0.0f)); // the paper dims itself
            const bool shown = ImGui::BeginPopupModal("##paper", nullptr,
                                                      ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoBackground | ImGuiWindowFlags_NoMove |
                                                          ImGuiWindowFlags_NoSavedSettings);
            ImGui::PopStyleColor();
            if (!shown)
                return;
            if (!state.paperOpen)
            {
                ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
                return;
            }
            const paper::View v = state.paper.view(1.0f, update);
            const bool ready = state.paperT >= 1.0f, printing = state.paperPrinting;
            bool backFocused = false, updateFocused = false;
            if (printing)
                ImGui::BeginDisabled();
            const bool back = paperButton("##paperback", state.paper.adBox(c, 0), v.map, true, &backFocused);
            const ImGuiID backId = ImGui::GetItemID();
            ownSoundItem();
            if (printing)
                ImGui::EndDisabled();
            bool go = false;
            ImGuiID updateId = 0;
            if (c.updateAd)
            {
                go = paperButton("##paperupdate", state.paper.adBox(c, 1), v.map, true, &updateFocused);
                updateId = ImGui::GetItemID();
                ownSoundItem();
            }
            if (!state.paperFocusSet)
            {
                // UPDATE NOW first when there is one, else BACK; set until it holds (the modal's own first-item focus
                // lands on BACK in its first frame)
                const bool toUpdate = c.updateAd && !printing;
                ImGui::SetFocusID(toUpdate ? updateId : backId, ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
                state.paperFocus = toUpdate ? 1 : 0;
                state.paperFocusSet = toUpdate ? updateFocused : backFocused;
                backFocused = updateFocused = false;
            }
            const int focus = updateFocused ? 1 : backFocused ? 0 : state.paperFocus;
            if (focus != state.paperFocus && ready)
                penSound(state); // the loop moves
            state.paperFocus = focus;
            // scrolling: the arrows, the d-pad, either stick, the wheel
            {
                float scroll = 0.0f;
                for (const ImGuiKey key : {ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadRStickUp})
                    scroll -= ImGui::GetKeyData(key)->AnalogValue > 0.0f ? ImGui::GetKeyData(key)->AnalogValue : (ImGui::IsKeyDown(key) ? 1.0f : 0.0f);
                for (const ImGuiKey key : {ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadRStickDown})
                    scroll += ImGui::GetKeyData(key)->AnalogValue > 0.0f ? ImGui::GetKeyData(key)->AnalogValue : (ImGui::IsKeyDown(key) ? 1.0f : 0.0f);
                state.paperScroll += scroll * 650.0f * ImGui::GetIO().DeltaTime - ImGui::GetIO().MouseWheel * 70.0f;
                state.paperScroll = std::clamp(state.paperScroll, 0.0f, state.paper.maxScroll());
            }
            const ImVec2 mouse = ImGui::GetIO().MousePos;
            const bool cancelKey = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            const bool clickOff = ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !saloon::inside(v.quad, Vector2{mouse.x, mouse.y});
            if (ready && !printing && (back || cancelKey || clickOff))
            {
                state.paperOpen = false;
                menuSound(state, kCancel); // the whoosh (BACK's cancel)
                ImGui::CloseCurrentPopup();
            }
            else if (ready && go && !printing)
            {
                menuSound(state, kConfirm);
                state.paperStop.clear();
                state.paperPrinting = true;
                startUpdate(state);
            }
            ImGui::EndPopup();
        }

        // Home's lower right corner, over the volume (claude/release.md; K restyles it later): the version, and when a
        // newer release is out, What's new and Update.
        void drawUpdate(State &state)
        {
            using Popup = State::UpdatePopup;
            const float size = fontSize(state, 34.0f), margin = fontSize(state, 22.0f), gap = fontSize(state, 12.0f);
            const bool available = g_updateCheck.state.load() == static_cast<int>(update::CheckState::Available);
            const float right = ImGui::GetWindowWidth() - margin;
            float y = ImGui::GetWindowHeight() - margin - size - gap;
            const update::Release &release = g_updateCheck.release;
            if (state.saloonOn && state.paperOn) // the newspaper (claude/newspaper.md)
                drawPaper(state);
            else if (state.saloonOn) // on the right slip (its ink is drawn before ImGui)
            {
                if (available)
                {
                    const Slip slip = slipAt(state, true);
                    bool focused = false;
                    if (paperButton("##notes", slipTextBox(state.billFonts.text, "What's new", 34.0f, kNotesAt), slip.paper, true, &focused))
                    {
                        state.updatePopup = Popup::Notes;
                        state.updateOpenPopup = true;
                    }
                    if (focused)
                        state.slipFocus = kSlipNotes;
                    if (paperButton("##update", slipTextBox(state.billFonts.text, "Update", 34.0f, kUpdateAt), slip.paper, true, &focused))
                    {
                        startUpdate(state);
                        state.updateOpenPopup = true;
                    }
                    if (focused)
                        state.slipFocus = kSlipUpdate;
                }
            }
            else if (available)
            {
                pushSize(state, 20.0f);
                const ImVec2 pad = ImGui::GetStyle().FramePadding;
                const float updateWidth = ImGui::CalcTextSize("Update").x + pad.x * 2.0f;
                const float notesWidth = ImGui::CalcTextSize("What's new").x + pad.x * 2.0f;
                y -= size;
                ImGui::SetCursorPos(ImVec2(right - updateWidth - gap - notesWidth, y));
                if (ImGui::Button("What's new", ImVec2(notesWidth, size)))
                {
                    state.updatePopup = Popup::Notes;
                    state.updateOpenPopup = true;
                }
                ImGui::SameLine(0.0f, gap);
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.04f, 1.0f));
                if (ImGui::Button("Update", ImVec2(updateWidth, size)))
                {
                    startUpdate(state);
                    state.updateOpenPopup = true;
                }
                ImGui::PopStyleColor(2);
                ImGui::PopFont();
                y -= gap * 0.6f;
            }
            if (!state.saloonOn)
            {
                pushSize(state, 18.0f);
                const std::string label = update::versionLabel() + (available ? "  \xC2\xB7  " + release.tag + " is out" : std::string());
                const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
                ImGui::SetCursorPos(ImVec2(right - textSize.x, y - textSize.y));
                ImGui::TextColored(kTextSoft, "%s", label.c_str());
                ImGui::PopFont();
            }

            if (state.updateOpenPopup)
            {
                state.updateOpenPopup = false;
                ImGui::OpenPopup("##update");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal("##update", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
                return;
            pushSize(state, 22.0f);
            const float width = fontSize(state, 640.0f), buttonWidth = fontSize(state, 200.0f);
            ImGui::Dummy(ImVec2(width, fontSize(state, 4.0f)));
            const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            bool close = false;
            if (state.updatePopup == Popup::Working)
            {
                const auto stage = static_cast<update::Stage>(g_updater.stage.load());
                if (stage == update::Stage::Failed)
                {
                    if (g_updater.worker.joinable())
                        g_updater.worker.join();
                    state.updatePopup = Popup::Failed;
                }
                else if (stage == update::Stage::Done)
                {
                    if (g_updater.worker.joinable())
                        g_updater.worker.join();
                    KM_INFO(Boot, "launcher: restarting after the update");
                    platform::relaunch(state.exePath);
                    state.result = Result::Quit;
                    state.done = true;
                }
            }
            switch (state.updatePopup)
            {
            case Popup::Notes:
            {
                pushSize(state, 26.0f);
                ImGui::TextColored(kHeading, "What's new");
                ImGui::PopFont();
                ImGui::BeginChild("##notes", ImVec2(width, fontSize(state, 380.0f)));
                // Text only (nothing to put the cursor on): the arrows, the d-pad and either stick scroll it.
                {
                    float scroll = 0.0f;
                    for (const ImGuiKey key : {ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadRStickUp})
                        scroll -= ImGui::GetKeyData(key)->AnalogValue > 0.0f ? ImGui::GetKeyData(key)->AnalogValue : (ImGui::IsKeyDown(key) ? 1.0f : 0.0f);
                    for (const ImGuiKey key : {ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadRStickDown})
                        scroll += ImGui::GetKeyData(key)->AnalogValue > 0.0f ? ImGui::GetKeyData(key)->AnalogValue : (ImGui::IsKeyDown(key) ? 1.0f : 0.0f);
                    if (scroll != 0.0f)
                        ImGui::SetScrollY(ImGui::GetScrollY() + scroll * fontSize(state, 900.0f) * ImGui::GetIO().DeltaTime);
                }
                pushSize(state, 19.0f);
                drawNotes(state, release.notes);
                ImGui::PopFont();
                ImGui::EndChild();
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.04f, 1.0f));
                const bool go = ImGui::Button("Update", ImVec2(buttonWidth, 0.0f));
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                const bool shut = ImGui::Button("Close", ImVec2(buttonWidth, 0.0f));
                ownSoundItem();
                if (go)
                    startUpdate(state);
                else if (shut || back)
                {
                    menuSound(state, kCancel);
                    close = true;
                }
                break;
            }
            case Popup::Working:
            {
                const uint64_t got = g_updater.received.load(), total = g_updater.total;
                if (static_cast<update::Stage>(g_updater.stage.load()) == update::Stage::Downloading)
                {
                    if (total > 0)
                        ImGui::Text("Downloading %s...  %llu of %llu MB", release.tag.c_str(),
                                    static_cast<unsigned long long>(got >> 20), static_cast<unsigned long long>((total + (1u << 20) - 1) >> 20));
                    else
                        ImGui::Text("Downloading %s...  %llu MB", release.tag.c_str(), static_cast<unsigned long long>(got >> 20));
                }
                else
                    ImGui::Text("Installing %s...", release.tag.c_str());
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
                ImGui::ProgressBar(total > 0 ? std::min(1.0f, static_cast<float>(got) / static_cast<float>(total)) : 0.0f,
                                   ImVec2(width, fontSize(state, 24.0f)), "");
                ImGui::PopStyleColor();
                pushSize(state, 17.0f);
                ImGui::TextColored(kTextDim, "The game restarts by itself when it's done. Your settings and saves stay.");
                ImGui::PopFont();
                break;
            }
            case Popup::Failed:
            {
                ImGui::TextUnformatted("The update didn't go through. Nothing was changed.");
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
                ImGui::TextColored(kTextDim, "%s", g_updater.error.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                const bool ok = ImGui::Button("OK", ImVec2(buttonWidth, 0.0f));
                if (ok || back)
                    close = true;
                break;
            }
            case Popup::None:
                close = true;
                break;
            }
            if (close)
            {
                state.updatePopup = Popup::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopFont();
            ImGui::EndPopup();
        }

#if KAMITE_LAUNCHER_STEAM
        void startSteamJob(State &state)
        {
            if (state.steamWorker.joinable())
                state.steamWorker.join();
            state.steamStage = static_cast<int>(steam::Stage::Downloading);
            state.steamPopup = State::SteamPopup::Working;
            const fs::path resources = kamite::paths::resources();
            state.steamWorker = std::thread([&state, resources] { state.steamResult = steam::addOrUpdate(resources, state.steamStage); });
        }

        // Add to Steam, or Update Steam Entry once it is there. Steam reads its shortcut list only when it starts, so a
        // running Steam is restarted, after asking.
        void pressSteam(State &state)
        {
            if (steam::running())
                state.steamPopup = State::SteamPopup::Confirm;
            else
                startSteamJob(state);
            state.steamOpenPopup = true;
        }

        // Add to Steam: Steam's mark in the bill's lower left corner in the saloon (K, Nov 18; pressed there), else a
        // button in the lower left corner; hidden without Steam. Then its popup.
        void drawSteam(State &state)
        {
            using Popup = State::SteamPopup;
            if (!state.steam.available)
                return;
            if (!state.saloonOn)
            {
                const float size = fontSize(state, 34.0f), margin = fontSize(state, 22.0f);
                pushSize(state, 20.0f);
                ImGui::SetCursorPos(ImVec2(margin, ImGui::GetWindowHeight() - margin - size));
                if (ImGui::Button(steamLabel(state), ImVec2(0.0f, size)))
                    pressSteam(state);
                ImGui::PopFont();
            }
            if (state.steamOpenPopup)
            {
                state.steamOpenPopup = false;
                ImGui::OpenPopup("##steam");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal("##steam", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
                return;
            pushSize(state, 22.0f);
            ImGui::Dummy(ImVec2(fontSize(state, 460.0f), fontSize(state, 4.0f)));
            const float buttonWidth = fontSize(state, 200.0f);
            const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            bool close = false;
            if (state.steamPopup == Popup::Working && state.steamStage.load() == static_cast<int>(steam::Stage::Done))
            {
                state.steamWorker.join();
                state.steam = steam::probe();
                state.steamPopup = Popup::Done;
                state.steamFocus = true;
            }
            switch (state.steamPopup)
            {
            case Popup::Confirm:
            {
                ImGui::TextUnformatted("Steam needs to restart to show God Hand: Rearmed.");
                ImGui::TextUnformatted("Restart Steam now?");
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (ImGui::IsWindowAppearing() || state.steamFocus)
                {
                    ImGui::SetKeyboardFocusHere();
                    state.steamFocus = false;
                }
                const bool yes = ImGui::Button("Restart Steam", ImVec2(buttonWidth, 0.0f));
                ownSoundItem();
                ImGui::SameLine();
                const bool no = ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f));
                ownSoundItem();
                if (yes)
                {
                    menuSound(state, kConfirm);
                    startSteamJob(state);
                }
                else if (no || back)
                {
                    menuSound(state, kCancel);
                    close = true;
                }
                break;
            }
            case Popup::Working:
            {
                const char *text = "Downloading the library art";
                switch (static_cast<steam::Stage>(state.steamStage.load()))
                {
                case steam::Stage::ClosingSteam:
                    text = "Closing Steam";
                    break;
                case steam::Stage::Writing:
                    text = "Adding God Hand: Rearmed to Steam";
                    break;
                case steam::Stage::StartingSteam:
                    text = "Starting Steam";
                    break;
                default:
                    break;
                }
                const int dots = static_cast<int>(GetTime() * 3.0) % 4;
                ImGui::Text("%s%.*s", text, dots, "...");
                break;
            }
            case Popup::Done:
            {
                ImGui::TextUnformatted(state.steamResult.message.c_str());
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (ImGui::IsWindowAppearing() || state.steamFocus)
                {
                    ImGui::SetKeyboardFocusHere();
                    state.steamFocus = false;
                }
                const bool ok = ImGui::Button("OK", ImVec2(buttonWidth, 0.0f));
                ownSoundItem();
                if (ok)
                {
                    menuSound(state, kConfirm);
                    close = true;
                }
                else if (back)
                {
                    menuSound(state, kCancel);
                    close = true;
                }
                break;
            }
            case Popup::None:
                close = true;
                break;
            }
            if (close)
            {
                state.steamPopup = Popup::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopFont();
            ImGui::EndPopup();
        }
#endif

        void drawLayer(const Texture2D &texture, const ImVec2 &min, const ImVec2 &max, Color tint)
        {
            if (texture.id == 0u || tint.a == 0)
                return;
            DrawTexturePro(texture, Rectangle{0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)},
                           Rectangle{min.x, min.y, max.x - min.x, max.y - min.y}, Vector2{0.0f, 0.0f}, 0.0f, tint);
        }

        float ramp(float t, float a, float b) { return std::clamp((t - a) / (b - a), 0.0f, 1.0f); }

        // Smooth in and out (smoothstep), so no step lands as a pop at 60 Hz.
        float ease(float t, float a, float b)
        {
            const float x = ramp(t, a, b);
            return x * x * (3.0f - 2.0f * x);
        }

        // A horizontal band from (x0) fading to nothing at (x1), either way (float edges: no pixel jitter).
        void band(float x0, float x1, float y, float h, Color c)
        {
            Color clear = c;
            clear.a = 0;
            const float left = std::min(x0, x1), width = std::fabs(x1 - x0);
            DrawRectangleGradientEx(Rectangle{left, y, width, std::max(1.0f, h)}, x1 >= x0 ? c : clear, x1 >= x0 ? c : clear,
                                    x1 >= x0 ? clear : c, x1 >= x0 ? clear : c);
        }

        // A layer scaled about a point (the glow's bloom rings).
        void drawLayerScaled(const Texture2D &texture, ImVec2 min, ImVec2 max, float cx, float cy, float sx, float sy, Color tint)
        {
            drawLayer(texture, ImVec2(cx + (min.x - cx) * sx, cy + (min.y - cy) * sy), ImVec2(cx + (max.x - cx) * sx, cy + (max.y - cy) * sy),
                      tint);
        }

        // The REARMED plate. t < 0: at rest; t >= 0: seconds since Play (the roulette pick, both ways from the plate).
        // K Nov 10: the plate no longer smears out to the screen edges; the letters bloom (additive glow rings, a hot
        // core and a soft flare) before the bar; every step eased.
        void drawPlate(const State &state, float t)
        {
            const ImVec2 min = state.logoMin, max = state.logoMax;
            const float w = max.x - min.x, h = max.y - min.y;
            const Color gold{240, 206, 90, 255};
            if (t < 0.0f)
            {
                drawLayer(state.rearmed, min, max, WHITE);
                drawLayer(state.rearmedText, min, max, gold);
                return;
            }
            const float px = min.x + w * kRearmedRect[0], py = min.y + h * kRearmedRect[1];
            const float pw = w * (kRearmedRect[2] - kRearmedRect[0]), ph = h * (kRearmedRect[3] - kRearmedRect[1]);
            const float cx = px + pw * 0.5f, cy = py + ph * 0.5f, screen = static_cast<float>(GetScreenWidth());
            const float reach = std::max(cx, screen - cx); // from the plate's centre to the farther screen edge
            const float glow = ease(t, 0.02f, 0.09f);                                // the letters heat up
            const float bloom = ease(t, 0.03f, 0.10f) * (1.0f - ease(t, 0.15f, 0.24f)); // their glow spreading
            const float bar = ease(t, 0.09f, 0.14f) * (1.0f - ease(t, 0.16f, 0.22f));
            const float streak = ease(t, 0.15f, 0.20f) * (1.0f - ease(t, 0.22f, 0.275f));
            const float live = 1.0f - ease(t, 0.24f, 0.28f), body = 1.0f - ease(t, 0.06f, 0.13f);
            drawLayer(state.rearmed, min, max, Color{255, 255, 255, alphaOf(body)});
            if (bar > 0.0f)
            {
                const float y0 = py + ph * 0.18f, bh = ph * 0.64f, a = bar * live;
                for (const float side : {-1.0f, 1.0f})
                {
                    band(cx, cx + side * reach * (0.4f + 0.6f * bar), y0, bh, Color{255, 226, 90, alphaOf(0.9f * a)});
                    band(cx, cx + side * reach * (0.3f + 0.4f * bar), y0 + bh * 0.1f, bh * 0.8f, Color{255, 250, 215, alphaOf(0.6f * a)});
                }
            }
            if (streak > 0.0f)
            {
                const float k = streak * live, thick = std::max(1.0f, ph / 45.0f);
                const float lines[5][3] = {{0.38f, 0.95f, 3.0f}, {0.5f, 0.7f, 2.0f}, {0.62f, 1.0f, 3.0f},
                                           {0.45f, 0.5f, 2.0f}, {0.56f, 0.85f, 2.0f}};
                for (const auto &line : lines)
                    for (const float side : {-1.0f, 1.0f})
                    {
                        const float y = py + ph * line[0], th = thick * line[2];
                        band(cx, cx + side * reach * line[1], y - th, th * 2.0f, Color{255, 170, 40, alphaOf(k)});
                        band(cx, cx + side * reach * line[1], y - th * 0.4f, th * 0.8f, Color{255, 245, 190, alphaOf(0.7f * k)});
                    }
            }
            // The letters: gold heating to a white-hot core, the glow blooming out in rings with a soft flare through
            // them (all additive, so it lights what is behind instead of only brightening the letters).
            drawLayer(state.rearmedText, min, max, Color{gold.r, gold.g, gold.b, alphaOf((1.0f - glow) * live)});
            if (glow > 0.0f || bloom > 0.0f)
            {
                BeginBlendMode(BLEND_ADDITIVE);
                const float a = live;
                const struct
                {
                    float sx, sy, alpha;
                    Color color;
                } rings[] = {{1.0f, 1.0f, 0.9f, {255, 120, 20, 255}},
                             {1.06f, 1.35f, 0.65f, {255, 140, 40, 255}},
                             {1.14f, 1.9f, 0.45f, {255, 110, 20, 255}},
                             {1.26f, 2.6f, 0.28f, {255, 90, 10, 255}}};
                for (const auto &ring : rings)
                {
                    const float grow = 1.0f + (ring.sx - 1.0f) * (0.5f + 0.5f * bloom), growY = 1.0f + (ring.sy - 1.0f) * (0.5f + 0.5f * bloom);
                    Color c = ring.color;
                    c.a = alphaOf(ring.alpha * std::max(glow * 0.6f, bloom) * a);
                    drawLayerScaled(state.rearmedGlow, min, max, cx, cy, grow, growY, c);
                }
                // The flare: a soft horizontal light through the letters' middle.
                const float fh = ph * (0.5f + 0.3f * bloom), fa = 0.55f * bloom * a;
                for (const float side : {-1.0f, 1.0f})
                {
                    band(cx, cx + side * pw * (0.55f + 0.5f * bloom), cy - fh * 0.5f, fh, Color{255, 150, 50, alphaOf(fa * 0.6f)});
                    band(cx, cx + side * pw * (0.45f + 0.35f * bloom), cy - fh * 0.12f, fh * 0.24f, Color{255, 235, 190, alphaOf(fa)});
                }
                EndBlendMode();
                drawLayer(state.rearmedText, min, max, Color{255, 200, 90, alphaOf(glow * a)});
                BeginBlendMode(BLEND_ADDITIVE);
                drawLayer(state.rearmedText, min, max, Color{255, 230, 170, alphaOf(0.75f * glow * a)}); // the white-hot core
                drawLayer(state.rearmedGlow, min, max, Color{255, 200, 120, alphaOf(0.5f * bloom * a)});
                EndBlendMode();
            }
        }

        // The logo, after ImGui laid it out: the kana behind (moved a little with the cursor; in the saloon the scene
        // draws it outside the doorway), GOD HAND, the plate.
        // While Play's animation runs the plate is drawn later, over the fade.
        // force: draw last frame's placement (before ImGui, under an open popup).
        void drawLogo(const State &state, bool force = false)
        {
            if (!(state.logoThisFrame || (force && state.logoPlaced)))
                return;
            if (!state.saloonOn) // (in the saloon the kana is outside the doorway, behind the doors: the scene draws it)
            {
                const ImVec2 shift(state.parallax.x * fontSize(state, 10.0f), state.parallax.y * fontSize(state, 7.0f));
                drawLayer(state.kana, ImVec2(state.logoMin.x + shift.x, state.logoMin.y + shift.y),
                          ImVec2(state.logoMax.x + shift.x, state.logoMax.y + shift.y), WHITE);
            }
            drawLayer(state.logo, state.logoMin, state.logoMax, WHITE);
            if (state.playTime < 0.0f)
                drawPlate(state, -1.0f);
        }

        void title(State &state)
        {
            if (state.saloonOn) // the logo is in the doorway (placed before ImGui): the screen starts under it
            {
                const float below = state.logoPlaced ? state.logoMax.y - ImGui::GetCursorScreenPos().y : fontSize(state, 300.0f);
                ImGui::Dummy(ImVec2(0.0f, std::max(fontSize(state, 24.0f), below + fontSize(state, 12.0f))));
                return;
            }
            if (state.logo.id != 0u && state.logo.height > 0)
            {
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 24.0f)));
                // Big: as wide as fits, at most half the window's height. Only laid out here: drawLogo draws the
                // layers after ImGui (tints, the additive glow and the Play animation need raylib).
                const float aspect = static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width);
                const float width = std::min({ImGui::GetWindowWidth() * 0.92f, fontSize(state, 1300.0f),
                                              ImGui::GetWindowHeight() * 0.50f / aspect});
                const float height = width * aspect;
                const ImVec2 start((ImGui::GetWindowWidth() - width) * 0.5f, ImGui::GetCursorPosY());
                ImGui::SetCursorPos(start);
                const ImVec2 screenStart = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, height));
                state.logoMin = screenStart;
                state.logoMax = ImVec2(screenStart.x + width, screenStart.y + height);
                state.logoPlaced = true;
                state.logoThisFrame = true;
                ImGui::SetCursorPos(ImVec2(0.0f, start.y + height));
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            }
            else
            {
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 70.0f)));
                pushSize(state, 92.0f);
                centeredText("GOD HAND: REARMED", ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
                ImGui::PopFont();
            }
            pushSize(state, 22.0f);
            centeredText("God Hand\u2122 for PS2 Recompiled, Refined, Rearmed.", kText);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 40.0f)));
        }


        // ---- a baked glyph atlas as an ImGui font (BMFont text .fnt + its PNG; resources/splash: Dominican, whose .ttf
        // can't ship). Each size ImGui asks for is scaled down from the atlas, its ascent + descent = the size (as
        // ImGui sizes a .ttf).
        saloon::GlyphAtlas g_atlasFont;

        bool atlasContains(ImFontAtlas *, ImFontConfig *, ImWchar c) { return g_atlasFont.has(c); }

        bool atlasBakedInit(ImFontAtlas *, ImFontConfig *src, ImFontBaked *baked, void *)
        {
            if (!src->MergeMode)
            {
                const float s = baked->Size / static_cast<float>(g_atlasFont.lineHeight);
                baked->Ascent = std::ceil(g_atlasFont.base * s);
                baked->Descent = std::floor(-(g_atlasFont.lineHeight - g_atlasFont.base) * s);
            }
            return true;
        }

        bool atlasLoadGlyph(ImFontAtlas *atlas, ImFontConfig *src, ImFontBaked *baked, void *, ImWchar c, ImFontGlyph *out, float *advanceOut)
        {
            if (!atlasContains(atlas, src, c))
                return false;
            const saloon::GlyphAtlas::Glyph &g = g_atlasFont.glyphs.find(c)->second;
            const float s = baked->Size / static_cast<float>(g_atlasFont.lineHeight);
            if (advanceOut)
            {
                *advanceOut = g.advance * s;
                return true;
            }
            out->Codepoint = c;
            out->AdvanceX = g.advance * s;
            if (g.w <= 0 || g.h <= 0)
                return true;
            const float density = src->RasterizerDensity * baked->RasterizerDensity, r = s * density; // atlas px -> bitmap px
            // the glyph's box in bitmap pixels (whole pixels, a pixel of room for the filter)
            const float fx0 = g.xoff * r, fy0 = g.yoff * r;
            const int x0 = static_cast<int>(std::floor(fx0)), y0 = static_cast<int>(std::floor(fy0));
            const int w = static_cast<int>(std::ceil(fx0 + g.w * r)) - x0 + 1, h = static_cast<int>(std::ceil(fy0 + g.h * r)) - y0 + 1;
            const ImFontAtlasRectId id = ImFontAtlasPackAddRect(atlas, w, h);
            if (id == ImFontAtlasRectId_Invalid)
                return false;
            ImTextureRect *rect = ImFontAtlasPackGetRect(atlas, id);
            std::vector<unsigned char> pixels(static_cast<size_t>(w) * h, 0);
            // each bitmap pixel: the atlas area under it, averaged (a box filter, sampled n x n)
            const int n = std::clamp(static_cast<int>(std::ceil(1.0f / r)) * 2, 2, 16);
            const auto at = [&](float ax, float ay) {
                const int ix = static_cast<int>(std::floor(ax)), iy = static_cast<int>(std::floor(ay));
                if (ix < 0 || iy < 0 || ix >= g.w || iy >= g.h)
                    return 0.0f;
                return static_cast<float>(g_atlasFont.alpha[static_cast<size_t>(g.y + iy) * g_atlasFont.width + g.x + ix]);
            };
            for (int py = 0; py < h; ++py)
                for (int px = 0; px < w; ++px)
                {
                    float sum = 0.0f;
                    for (int sy = 0; sy < n; ++sy)
                        for (int sx = 0; sx < n; ++sx)
                        {
                            const float bx = x0 + px + (sx + 0.5f) / n, by = y0 + py + (sy + 0.5f) / n; // bitmap px
                            sum += at(bx / r - g.xoff, by / r - g.yoff);
                        }
                    pixels[static_cast<size_t>(py) * w + px] = static_cast<unsigned char>(std::min(255.0f, sum / (n * n) + 0.5f));
                }
            out->X0 = x0 / density;
            out->Y0 = y0 / density;
            out->X1 = (x0 + w) / density;
            out->Y1 = (y0 + h) / density;
            out->Visible = true;
            out->PackId = id;
            ImFontAtlasBakedSetFontGlyphBitmap(atlas, baked, src, out, rect, pixels.data(), ImTextureFormat_Alpha8, w);
            return true;
        }

        // The atlas font, the glyphs it lacks (curly quotes, dashes, ellipses) from `fallback` (a .ttf).
        ImFont *addAtlasFont(ImGuiIO &io, const fs::path &fnt, const fs::path &fallback)
        {
            static ImFontLoader loader;
            loader.Name = "atlas";
            loader.FontSrcContainsGlyph = atlasContains;
            loader.FontBakedInit = atlasBakedInit;
            loader.FontBakedLoadGlyph = atlasLoadGlyph;
            if (!saloon::readGlyphAtlas(fnt, g_atlasFont))
                return nullptr;
            ImFontConfig config;
            config.FontLoader = &loader;
            config.SizePixels = 26.0f;
            std::snprintf(config.Name, sizeof(config.Name), "%s", "atlas");
            ImFont *font = io.Fonts->AddFont(&config);
            std::error_code error;
            if (font && fs::is_regular_file(fallback, error))
            {
                ImFontConfig merge;
                merge.MergeMode = true;
                io.Fonts->AddFontFromFileTTF(utf8(fallback).c_str(), 26.0f, &merge);
            }
            return font;
        }

        // ---- Home in the saloon (K, Nov 18): the menu on the bill of fare --------------------------------------------
        // The logo's GOD HAND + plate box in logo.png's canvas (tools/logo: placed in the doorway by that box).
        constexpr float kLogoContent[4] = {0.1225f, 0.3679f, 0.8968f, 0.9002f};

        // The bill's items: Play (Install when not installed), Options, Quit, then Skip Launcher (installed) and Steam's
        // mark (Steam found).
        constexpr size_t kBillOptions = 1, kBillQuit = 2;

        void openOptions(State &state, bool sound = true)
        {
            g_hotkeysOpen = false;
            if (sound)
                bookSound(state, true);
            saloon::Scene &scene = state.scene;
            if (!scene.zoomBroken() && !scene.zoomReady())
            {
                state.zoomPending = true; // its frames are still being read (the first moments): it starts when they are
                return;
            }
            if (scene.zoomBroken())
            {
                state.screen = Screen::Options; // straight there, over Home darkened
                state.focusFirst = true;
                state.optionsShown = 0.0f;
                state.optionsLeaving = false;
                return;
            }
            scene.zoomIn(); // the main loop opens Options when it lands
        }

        void drawSaloonHome(State &state)
        {
            if (state.installed && state.firstRunPending)
            {
                state.screen = Screen::FirstRun; // a new install whose disc image was found without the Install screen
                state.focusFirst = true;
                return;
            }
            saloon::Scene &scene = state.scene;
            const ImGuiIO &io = ImGui::GetIO();
            const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            const bool home = scene.atHome() && state.playTime < 0.0f;
            // Through the zoom only Back counts: it reverses from the frame showing.
            if (scene.zoomingIn() && !popup &&
                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            {
                scene.zoomOut();
                bookSound(state, false);
            }
            saloon::Bill &bill = state.bill;
            bill.items = {state.installed ? "Play" : "Install", "Options", "Quit"};
            bill.splash = state.splash;
            bill.showSkip = state.installed;
            bill.skip = !state.ini.getBool("Launcher", "Show", true);
#if KAMITE_LAUNCHER_STEAM
            bill.steam = state.steam.available;
            bill.steamAdded = state.steam.added;
#else
            bill.steam = false;
#endif
            bill.volume = true;
            bill.level = state.volume;
            bill.muted = state.muted;
            const size_t skipItem = bill.showSkip ? bill.items.size() : SIZE_MAX;
            [[maybe_unused]] const size_t steamItem = bill.steam ? bill.items.size() + (bill.showSkip ? 1u : 0u) : SIZE_MAX;
            const size_t muteItem = bill.items.size() + (bill.showSkip ? 1u : 0u) + (bill.steam ? 1u : 0u);
            bool overTarget = false;
            saloon::Quad quad;
            int focus = -1;
            bool billShown = false;
            const Vector2 mouse{io.MousePos.x, io.MousePos.y};
            const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
            const bool overBook = home && !popup && scene.bookHit(mouse);
            // The REARMED plate under the logo is Play as well (K, Nov 19): hovering it puts the loop on Play, a click
            // plays (Install before there is a game).
            bool overPlate = false;
            if (home && !popup && state.logoPlaced)
            {
                const ImVec2 min = state.logoMin, max = state.logoMax;
                const float w = max.x - min.x, h = max.y - min.y;
                overPlate = mouse.x >= min.x + w * kRearmedRect[0] && mouse.x <= min.x + w * kRearmedRect[2] &&
                            mouse.y >= min.y + h * kRearmedRect[1] && mouse.y <= min.y + h * kRearmedRect[3];
            }
            const auto playItem = [&] {
                if (state.installed)
                {
                    if (state.playTime < 0.0f)
                    {
                        state.playTime = 0.0f;       // the plate's animation and the fade; the main loop starts the game after
                        state.soundThisFrame = true; // Play's own sound, next frame
                        scene.fling();               // the doors fling open into the fade
                    }
                }
                else
                {
                    state.screen = Screen::Install;
                    state.focusFirst = true;
                }
            };
            if (scene.bill(quad))
            {
                billShown = true;
                const saloon::Homography paper = saloon::Homography::rectToQuad(saloon::kPaperW, saloon::kPaperH, quad);
                const std::vector<Rectangle> targets = saloon::billTargets(state.billFonts, bill);
                std::vector<ImGuiID> ids(targets.size(), 0u); // for the pad's own routes below
                if (!home)
                    ImGui::BeginDisabled();
                for (size_t i = 0; i < targets.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    bool focused = false;
                    const bool pressed = paperButton("##bill", targets[i], paper, home, &focused);
                    ids[i] = ImGui::GetItemID();
                    overTarget = overTarget || saloon::inside(paperQuad(targets[i], paper), mouse);
                    if (home && ((state.focusFirst && i == 0) || (state.focusOptions && i == kBillOptions)))
                    {
                        // Play starts focused, its loop showing, so A / Enter plays at once (K Oct 29); back from
                        // Options the loop is on Options again. (Set on the item itself: SetKeyboardFocusHere tabs
                        // to the next text-input item, here the volume slider, and opens it.)
                        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                        ImGui::SetNavCursorVisible(true);
                        focused = true;
                    }
                    // hovering the book is hovering Options (the book lights, the loop goes to Options); the plate, Play
                    if (home && ((i == kBillOptions && overBook) || (i == 0 && overPlate)) && moved && !focused)
                    {
                        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                        focused = true;
                    }
                    if (focused)
                        focus = static_cast<int>(i);
                    if ((i == 0 && state.installed) || i == kBillOptions || i == kBillQuit)
                        ownSoundItem(); // Play's own sound; Options has the whoosh and the book; Quit just goes (K: no menu sound)
                    else if (i == skipItem || i == muteItem)
                        tickItem(); // the tick box, the mute mark
                    if (pressed && home)
                    {
                        if (i == 0)
                            playItem();
                        else if (i == kBillOptions)
                            openOptions(state);
                        else if (i == kBillQuit)
                        {
                            state.result = Result::Quit;
                            state.done = true;
                        }
                        else if (i == skipItem)
                        {
                            bill.skip = !bill.skip;
                            state.ini.setBool("Launcher", "Show", !bill.skip);
                            state.ini.save();
                        }
#if KAMITE_LAUNCHER_STEAM
                        else if (i == steamItem)
                            pressSteam(state);
#endif
                        else if (i == muteItem)
                        {
                            state.muted = !state.muted;
                            state.ini.setBool("Launcher", "Mute", state.muted);
                            state.ini.save();
                        }
                    }
                    ImGui::PopID();
                }
                // the volume ruler down the bill's right edge: an ImGui slider with nothing of its own showing over it; a
                // mouse drag follows the paperclip through the bill's perspective. Its focus draws no loop (K), so the
                // keyboard / pad never land on it (K, Nov 19: they went there from Steam's mark, invisibly): the mute
                // mark is their volume (up / down below).
                {
                    const Rectangle r = saloon::billRuler();
                    const Vector2 top = paper.map(r.x + r.width * 0.5f, r.y), bottom = paper.map(r.x + r.width * 0.5f, r.y + r.height);
                    const Vector2 left = paper.map(r.x, r.y + r.height * 0.5f), right = paper.map(r.x + r.width, r.y + r.height * 0.5f);
                    const float x0 = std::min(left.x, right.x), x1 = std::max(left.x, right.x);
                    ImGui::SetCursorScreenPos(ImVec2(x0, std::min(top.y, bottom.y)));
                    const ImGuiCol hidden[] = {ImGuiCol_FrameBg,         ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab,
                                               ImGuiCol_SliderGrabActive, ImGuiCol_Text,           ImGuiCol_Border,        ImGuiCol_NavCursor};
                    for (const ImGuiCol col : hidden)
                        ImGui::PushStyleColor(col, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    float percent = state.volume * 100.0f;
                    ImGui::PushItemFlag(ImGuiItemFlags_NoNav, true);
                    bool changed = ImGui::VSliderFloat("##volume", ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, std::fabs(bottom.y - top.y))), &percent,
                                                       0.0f, 100.0f, "", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
                    ImGui::PopItemFlag();
                    ImGui::PopStyleColor(static_cast<int>(std::size(hidden)));
                    state.volumeSliderId = ImGui::GetItemID();
                    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    {
                        percent = saloon::billRulerLevel(paper.inverse().map(mouse).y) * 100.0f;
                        changed = true;
                    }
                    if (changed && home)
                    {
                        state.volume = percent / 100.0f;
                        if (state.muted && state.volume > 0.0f)
                        {
                            state.muted = false;
                            state.ini.setBool("Launcher", "Mute", false);
                        }
                    }
                    if (home && ImGui::IsItemHovered() && moved && !ImGui::IsItemFocused() && !popup)
                        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                    if (ImGui::IsItemDeactivated())
                    {
                        char text[16];
                        std::snprintf(text, sizeof(text), "%.2f", state.volume);
                        state.ini.set("Launcher", "Volume", text);
                        state.ini.save();
                    }
                    overTarget = overTarget || ImGui::IsItemHovered();
                }
                // The pad's (and arrow keys') routes on the bill (K, Nov 19), over ImGui's own by position: Steam's mark
                // down / right to Play, Play right to the mute mark, left to Steam's mark and up round to the Skip
                // line, the mute mark left to Play; right from Options, Quit, the Skip line and the mute mark to the
                // newspaper's corner, and left from it to Quit; on the mute mark (or
                // the ruler, focused by the mouse) up / down turn the volume 5% a step (held: repeats), A mutes.
                ImGuiContext &g = *GImGui;
                if (home && !popup && g.NavMoveSubmitted && g.NavWindow == ImGui::GetCurrentWindow() && !ids.empty())
                {
                    const ImGuiDir dir = g.NavMoveDir;
                    const ImGuiID at = g.NavId;
                    const bool onMute = muteItem < ids.size() && at == ids[muteItem];
                    const bool onSteam = steamItem < ids.size() && at == ids[steamItem];
                    const bool onRuler = at != 0u && at == state.volumeSliderId;
                    ImGuiID to = 0u;
                    if ((onMute || onRuler) && (dir == ImGuiDir_Up || dir == ImGuiDir_Down))
                    {
                        ImGui::NavMoveRequestCancel();
                        const float level = std::round(state.volume * 20.0f) + (dir == ImGuiDir_Up ? 1.0f : -1.0f);
                        state.volume = std::clamp(level / 20.0f, 0.0f, 1.0f);
                        if (state.muted && dir == ImGuiDir_Up)
                        {
                            state.muted = false;
                            state.ini.setBool("Launcher", "Mute", false);
                        }
                        char text[16];
                        std::snprintf(text, sizeof(text), "%.2f", state.volume);
                        state.ini.set("Launcher", "Volume", text);
                        state.ini.save();
                    }
                    else if (onMute && dir == ImGuiDir_Left)
                        to = ids[0];
                    else if (state.paperCornerId != 0u && dir == ImGuiDir_Right &&
                             (onMute || at == ids[kBillOptions] || at == ids[kBillQuit] || (skipItem < ids.size() && at == ids[skipItem])))
                        to = state.paperCornerId; // the newspaper's corner (K, Nov 19)
                    else if (state.paperCornerId != 0u && at == state.paperCornerId && dir == ImGuiDir_Left)
                        to = ids[kBillQuit];
                    else if (onSteam && (dir == ImGuiDir_Down || dir == ImGuiDir_Right))
                        to = ids[0];
                    else if (at == ids[0] && dir == ImGuiDir_Right && muteItem < ids.size())
                        to = ids[muteItem];
                    else if (at == ids[0] && dir == ImGuiDir_Left && steamItem < ids.size())
                        to = ids[steamItem];
                    else if (at == ids[0] && dir == ImGuiDir_Up && skipItem < ids.size())
                        to = ids[skipItem]; // wraps to the Skip Launcher line at the bill's foot
                    if (to != 0u)
                    {
                        ImGui::NavMoveRequestCancel();
                        ImGui::SetFocusID(to, ImGui::GetCurrentWindow());
                        ImGui::SetNavCursorVisible(true);
                    }
                }
                if (!home)
                    ImGui::EndDisabled();
            }
            if (home && billShown)
                state.focusOptions = false;
            // A click on the book opens Options; on the plate, plays; on a snack, it is eaten (an egg: no hover hint, K).
            if (home && !popup && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
            {
                if (overBook)
                    openOptions(state);
                else if (overPlate && billShown)
                    playItem();
                else if (const saloon::Scene::Bite bite = scene.snackClick(mouse); bite != saloon::Scene::Bite::None)
                    eatSound(state, bite == saloon::Scene::Bite::Drink);
            }
            // The pad's snacks (K, Nov 19): north (Y / triangle) a random piece of fruit, west (X / square) the smoothie.
            if (home && !popup && billShown)
            {
                const bool fruit = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceUp, false);
                const bool smoothie = !fruit && ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false);
                if (fruit || smoothie)
                    if (const saloon::Scene::Bite bite = scene.eatSnack(smoothie); bite != saloon::Scene::Bite::None)
                        eatSound(state, bite == saloon::Scene::Bite::Drink);
            }
            // The mouse moved off everything (the bill's lines, the book, the slip's controls): nothing stays focused, so
            // no loop lingers and the book goes dark (K, Nov 18). The next key or button focuses the first item.
            if (home && billShown && !popup && moved && !overTarget && !overBook && !overPlate && !ImGui::IsAnyItemHovered())
            {
                ImGuiContext &g = *GImGui;
                if (g.NavId != 0 && g.NavWindow == ImGui::GetCurrentWindow())
                {
                    ImGui::SetNavID(0, ImGuiNavLayer_Main, 0, ImRect());
                    state.navCleared = true;
                    focus = -1;
                }
            }
            // The loop follows the focus; through the zoom it stays where it was (on Options).
            if (home && focus != bill.focus)
            {
                if (focus >= 0)
                    penSound(state);
                bill.focus = focus;
                bill.focusSeed = static_cast<uint32_t>(std::rand()) * 2654435761u + 1u;
                bill.focusAge = 0.0f;
            }
            if (billShown)
                state.focusFirst = false;
        }

        // The saloon's ink before ImGui (so popups cover it), from last frame's focus: the bill, the corner slip.
        void drawSaloonInk(State &state)
        {
            const saloon::Scene &scene = state.scene;
            if (!scene.ready())
                return;
            saloon::Quad quad;
            if (state.screen == Screen::Home && scene.bill(quad))
                saloon::drawBill(state.billFonts, state.bill, saloon::Homography::rectToQuad(saloon::kPaperW, saloon::kPaperH, quad));
            if (state.screen == Screen::Options)
                return;
            const float alpha = scene.overlays();
            if (state.paperOn)
            {
                // the newspaper's corner (open, it is drawn over everything after the logo); its pencil loop when focused
                if (state.paperT <= 0.0f)
                {
                    const paper::Content c = paperContent(state);
                    const bool update = !c.outNow.empty();
                    state.paper.draw(c, 0.0f, alpha);
                    if (state.slipFocus == kSlipPaper && state.screen == Screen::Home)
                    {
                        Color pencil = kSlipPencil;
                        pencil.a = alphaOf(pencil.a / 255.0f * alpha);
                        const Rectangle box{24.0f, 18.0f, 690.0f, update ? 178.0f : 146.0f};
                        saloon::beginInk();
                        saloon::inkLoop(box, 1.0f, 41u, pencil, state.paper.view(0.0f, update).map, 10.0f, 4.0f);
                        saloon::endInk();
                    }
                }
                return;
            }
            const Slip right = slipAt(state, true);
            drawSlipPaper(state, right, alpha);
            drawSlipInk(state, right, true, alpha);
        }

        bool checkImage(State &state, const fs::path &image)
        {
            if (image.empty())
            {
                state.installMessage = "Choose your disc image (USA, SLUS-21503).";
                state.installError = true;
                return false;
            }
            std::error_code error;
            if (!fs::is_regular_file(image, error))
            {
                state.installMessage = "There is no file at " + utf8(image) + ".";
                state.installError = true;
                return false;
            }
            const DiscCheckResult check = verifyDiscAndPrepare(image, state.context->cacheDir);
            if (!check.ok)
            {
                state.installMessage = check.error;
                state.installError = true;
                return false;
            }
            state.ini.set("Game", "Iso", utf8(image));
            state.ini.save();
            state.installed = true;
            loadMenuSounds(state, image);
            loadSnackVoices(state, image);
            startBark(state, image);
            state.installMessage.clear();
            state.installError = false;
            KM_INFO(Boot, "launcher: disc image installed: " << utf8(image));
            return true;
        }

        void drawHome(State &state)
        {
            if (state.saloonOn)
            {
                drawSaloonHome(state);
                return;
            }
            if (state.installed && state.firstRunPending)
            {
                state.screen = Screen::FirstRun; // a new install whose disc image was found without the Install screen
                state.focusFirst = true;
                return;
            }
            title(state);
            const float buttonWidth = fontSize(state, 420.0f);
            if (!state.installed)
            {
                pushSize(state, 22.0f);
                centeredText("To play, point the port to your own copy of the game.", kTextSoft);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (bigButton(state, "Install", buttonWidth, true))
                {
                    state.screen = Screen::Install;
                    state.focusFirst = true;
                }
                if (state.focusFirst)
                    ImGui::SetItemDefaultFocus();
            }
            else
            {
                // Play starts selected, its highlight showing, so A / Enter plays at once (SetItemDefaultFocus alone
                // left the cursor hidden until a d-pad press, K Oct 29).
                if (state.focusFirst)
                {
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                const bool play = bigButton(state, "Play", buttonWidth, true);
                ownSoundItem();
                if (play && state.playTime < 0.0f)
                {
                    state.playTime = 0.0f; // the plate's animation and the fade; the main loop starts the game after
                    state.soundThisFrame = true; // Play's own sound, next frame
                }
                if (bigButton(state, "Options", buttonWidth, false))
                {
                    state.screen = Screen::Options;
                    state.focusFirst = true;
                    g_hotkeysOpen = false;
                }
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
                pushSize(state, 20.0f);
                bool hide = !state.ini.getBool("Launcher", "Show", true);
                const char *label = "Don't show this again (hold Shift while starting to see it)";
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(label).x - ImGui::GetFrameHeight() -
                                      ImGui::GetStyle().ItemInnerSpacing.x) *
                                     0.5f);
                const bool toggled = ImGui::Checkbox(label, &hide);
                tickItem();
                if (toggled)
                {
                    state.ini.setBool("Launcher", "Show", !hide);
                    state.ini.save();
                }
                ImGui::PopFont();
            }
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 24.0f)));
            pushSize(state, 22.0f);
            const float quitWidth = fontSize(state, 160.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - quitWidth) * 0.5f);
            if (ImGui::Button("Quit", ImVec2(quitWidth, 0.0f)))
            {
                state.result = Result::Quit;
                state.done = true;
            }
            ImGui::PopFont();
            // The rights notice at the foot of the title screen.
            {
                pushSize(state, 16.0f);
                const char *lines[] = {"God Hand\u2122 is a trademark of CAPCOM CO., LTD. All rights to the game belong to Capcom.",
                                       "God Hand: Rearmed is an unofficial fan-made project, not affiliated with or endorsed by Capcom or Clover Studio. "
                                       "It contains no game data: play your own damn copy!"};
                const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                const float y = ImGui::GetWindowHeight() - lineHeight * 2.0f - fontSize(state, 18.0f);
                if (ImGui::GetCursorPosY() < y)
                    ImGui::SetCursorPosY(y);
                for (const char *line : lines)
                    centeredText(line, kTextSoft);
                ImGui::PopFont();
            }
            state.focusFirst = false;
        }

        void drawInstall(State &state)
        {
            title(state);
            const float width = fontSize(state, 760.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 22.0f);
            ImGui::BeginChild("install", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders |
                                                                  ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::TextWrapped("The port plays from your own disc image (USA, SLUS-21503: an .iso, or a .chd of it). "
                               "Nothing is copied: the game reads it while you play, so keep it where it is.");
            ImGui::Spacing();
            if (ImGui::Button("Choose disc image..."))
            {
                const fs::path picked = platform::pick(GetWindowHandle(), false);
                if (!picked.empty())
                {
                    const std::string text = utf8(picked);
                    std::snprintf(state.isoText, sizeof(state.isoText), "%s", text.c_str());
                    checkImage(state, picked);
                }
            }
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            ImGui::TextColored(kTextDim, "or type its path:");
            ImGui::SetNextItemWidth(-1.0f);
            const bool entered = ImGui::InputText("##iso", state.isoText, sizeof(state.isoText), ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Check and install") || entered)
            {
                std::string text = state.isoText;
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                checkImage(state, fromUtf8(text));
            }
            if (!state.installMessage.empty())
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, state.installError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : kTextDim);
                ImGui::TextWrapped("%s", state.installMessage.c_str());
                ImGui::PopStyleColor();
            }
            if (state.installed)
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.45f, 1.0f), "Installed. Your disc image checks out.");
            }
            ImGui::Spacing();
            const bool leave = ImGui::Button(state.installed ? "Continue" : "Back");
            if (!state.installed)
                ownSoundItem();
            if (leave)
            {
                if (!state.installed)
                    menuSound(state, kCancel);
                state.screen = state.installed && state.firstRunPending ? Screen::FirstRun : Screen::Home;
                state.focusFirst = true;
            }
            ImGui::EndChild();
            ImGui::PopFont();
            state.focusFirst = false;
        }

        // ---- settings tables --------------------------------------------------------------------------------------
        // Every setting is a row (name and help on the left, the control on the right) with a line above it; a
        // setting's own sub-options (subSetting) share its section, with no line between them.
        int g_settingRows = 0;

        void dividerLine()
        {
            ImGuiWindow *window = ImGui::GetCurrentWindow();
            const float y = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
            const float x0 = window->InnerRect.Min.x + ImGui::GetStyle().WindowPadding.x;
            const float x1 = window->InnerRect.Max.x - ImGui::GetStyle().WindowPadding.x;
            ImGui::PushClipRect(window->InnerRect.Min, window->InnerRect.Max, false);
            window->DrawList->AddLine(ImVec2(x0, y), ImVec2(x1, y), ImGui::GetColorU32(ImVec4(0.35f, 0.22f, 0.15f, 0.55f)), 1.0f);
            ImGui::PopClipRect();
        }

        void describe(const char *name, const char *help, const char *note)
        {
            ImGui::TextUnformatted(name);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            if (help && *help)
                ImGui::TextWrapped("%s", help);
            if (note && *note)
                ImGui::TextWrapped("NOTE: %s", note);
            ImGui::PopStyleColor();
        }

        // A setting in a section of its own: a line with some room above and below it, then the name and help on the
        // left (`note`: a line of its own at the bottom of the help, "NOTE: ..."; `below`: more controls under the help,
        // such as Camera's "Show advanced settings"), the control on the right.
        void settingLabel(const char *name, const char *help, const char *note = nullptr, const std::function<void()> &below = {})
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool divided = g_settingRows++ > 0;
            const float gap = std::round(ImGui::GetFontSize() * 0.45f), spacing = ImGui::GetStyle().ItemSpacing.y;
            if (divided)
            {
                ImGui::Dummy(ImVec2(0.0f, gap));
                dividerLine();
                ImGui::Dummy(ImVec2(0.0f, gap));
            }
            describe(name, help, note);
            if (below)
                below();
            ImGui::TableSetColumnIndex(1);
            if (divided)
                ImGui::Dummy(ImVec2(0.0f, 2.0f * gap + spacing)); // the control lines up with the name
        }

        // A sub-option in the section of the setting above it.
        void subSetting(const char *name, const char *help, const char *note = nullptr)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(ImGui::GetFontSize());
            describe(name, help, note);
            ImGui::Unindent(ImGui::GetFontSize());
            ImGui::TableSetColumnIndex(1);
        }

        bool beginSettings(const char *id)
        {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
                return false;
            g_settingRows = 0;
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.58f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.42f);
            return true;
        }

        // A tab's settings come in categories: an orange heading with a rule across the panel (it is also the line above
        // the category's first setting), then the category's own settings table (the columns line up from table to
        // table). Only one tab is drawn a frame, so g_categories counts that tab's headings: room above all but the first.
        int g_categories = 0;

        void categoryGap() { ImGui::Dummy(ImVec2(0.0f, std::round(ImGui::GetFontSize() * (g_categories++ > 0 ? 0.8f : 0.2f)))); }

        void categoryHeading(const char *title)
        {
            categoryGap();
            ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
            ImGui::SeparatorText(title);
            ImGui::PopStyleColor();
        }

        bool beginCategory(const char *title)
        {
            categoryHeading(title);
            return beginSettings(title);
        }

        // A heading that folds its category away: the arrow takes the place of the rule's short left end.
        bool collapsibleHeading(const char *title, bool &open)
        {
            categoryGap();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float right = start.x + ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemOpen(open, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
            open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_NoTreePushOnOpen);
            tickItem();
            ImGui::PopStyleColor();
            const ImGuiStyle &style = ImGui::GetStyle();
            const float x0 = start.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(title, nullptr, true).x +
                             style.ItemSpacing.x;
            const float y = std::floor((ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f + 0.99f);
            if (right > x0 && style.SeparatorTextBorderSize > 0.0f)
                ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(right, y), ImGui::GetColorU32(ImGuiCol_Separator),
                                                    style.SeparatorTextBorderSize);
            return open;
        }

        void iniCombo(State &state, const char *id, const char *section, const char *key, const char *fallback,
                      bool (*combo)(const char *, std::string &))
        {
            std::string value = state.ini.get(section, key, fallback);
            if (value.empty())
                value = fallback;
            if (combo(id, value))
                state.ini.set(section, key, value);
        }

        void iniCheckbox(State &state, const char *id, const char *section, const char *key, bool fallback)
        {
            bool value = state.ini.getBool(section, key, fallback);
            const bool toggled = ImGui::Checkbox(id, &value);
            tickItem();
            if (toggled)
                state.ini.setBool(section, key, value);
        }

        // ---- Controls: player 1's bindings (controls.ini through ps2_host_input's model) ----------
        namespace pc = ps2_host_input::controls;
        namespace gp = ps2_host_gamepads;

        struct ControlRow
        {
            const char *key;    // controls.ini [Pad1] key
            const char *action; // what it does in the game ("" = nothing known)
            const char *button; // the PS2 button
            bool keysOnly = false; // keyboard & mouse only (the Dodge Modifier)
        };
        // The rows: the PS2 inputs in controls.ini's order.
        constexpr ControlRow kControlRows[] = {
            {"Cross", "Attack / OK", "Cross"},
            {"Circle", "Action / prompt", "Circle"},
            {"Square", "Combo", "Square"},
            {"Triangle", "Attack / cancel", "Triangle"},
            {"L1", "Turn around", "L1"},
            {"R1", "God Reel", "R1"},
            {"L2", "Taunt", "L2"},
            {"R2", "Unleash", "R2"},
            {"L3", "", "L3 (left stick click)"},
            {"R3", "", "R3 (right stick click)"},
            {"Start", "Pause menu", "Start"},
            {"Select", "", "Select"},
            {"Up", "Style S / menu up", "D-pad up"},
            {"Down", "Style X / menu down", "D-pad down"},
            {"Left", "Style Y / menu left", "D-pad left"},
            {"Right", "Style E / menu right", "D-pad right"},
            {"LUp", "Move forward", "Left stick up"},
            {"LDown", "Step back", "Left stick down"},
            {"LLeft", "Move left", "Left stick left"},
            {"LRight", "Move right", "Left stick right"},
            {"RUp", "Duck", "Right stick up"},
            {"RDown", "Back flip", "Right stick down"},
            {"RLeft", "Sidestep left", "Right stick left"},
            {"RRight", "Sidestep right", "Right stick right"},
            {"DodgeModifier", "Hold: the move keys dodge", "Dodge modifier", true},
        };
        constexpr int kControlRowCount = static_cast<int>(std::size(kControlRows));
        constexpr int kKeyColumns = 2; // Key 1, Key 2 (a third binding in the file is kept as it is)
        constexpr uint32_t kPlayer = 0u;

        struct Capture
        {
            bool active = false;
            bool controller = false; // false: keys and mouse
            int row = 0;
            int column = 0;          // keys: 0..kKeyColumns-1
            bool waitRelease = true; // the click or button press that started it must end first
            double started = 0.0;
        };
        Capture g_capture;
        bool g_openCapture = false;   // open the popup at drawControls' ID level (cells push their own IDs)
        bool g_captureFrame = false;  // a capture ran this frame: its Esc / B must not also leave Options
        std::string g_controlsStatus;
        bool g_resetArmed = false;

        struct Split
        {
            std::vector<std::string> keys, pads, other;
        };

        Split splitRow(int row)
        {
            Split split;
            for (std::string &binding : pc::bindings(kPlayer, kControlRows[row].key))
            {
                switch (pc::kindOf(binding))
                {
                case pc::Kind::Keys: split.keys.push_back(std::move(binding)); break;
                case pc::Kind::Controller: split.pads.push_back(std::move(binding)); break;
                default: split.other.push_back(std::move(binding)); break;
                }
            }
            return split;
        }

        void storeRow(int row, const Split &split)
        {
            std::vector<std::string> list = split.keys;
            list.insert(list.end(), split.pads.begin(), split.pads.end());
            list.insert(list.end(), split.other.begin(), split.other.end());
            pc::setBindings(kPlayer, kControlRows[row].key, std::move(list));
        }

        std::string rowName(int row)
        {
            const ControlRow &r = kControlRows[row];
            return *r.action ? std::string(r.action) + " (" + r.button + ")" : std::string(r.button);
        }

        // The binding goes into the cell; within one action it appears once; another action may
        // share it (the status line says so).
        void bindCell(int row, bool controller, int column, const std::string &binding)
        {
            Split split = splitRow(row);
            std::vector<std::string> &list = controller ? split.pads : split.keys;
            size_t at = controller ? 0u : static_cast<size_t>(column);
            if (at < list.size())
                list[at] = binding;
            else
            {
                list.push_back(binding);
                at = list.size() - 1u;
            }
            for (size_t i = list.size(); i-- > 0u;)
                if (i != at && pc::sameBinding(list[i], binding))
                {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                    if (i < at)
                        --at;
                }
            storeRow(row, split);
            g_controlsStatus = rowName(row) + ": " + pc::displayName(binding);
            for (int other = 0; other < kControlRowCount; ++other)
            {
                if (other == row)
                    continue;
                bool shared = false;
                for (const std::string &existing : pc::bindings(kPlayer, kControlRows[other].key))
                    shared = shared || pc::sameBinding(existing, binding);
                if (shared)
                {
                    g_controlsStatus += "   (also on " + rowName(other) + ")";
                    break;
                }
            }
            KM_INFO(Boot, "launcher: controls " << kControlRows[row].key << " = " << binding);
        }

        void clearCell(int row, bool controller, int column)
        {
            Split split = splitRow(row);
            std::vector<std::string> &list = controller ? split.pads : split.keys;
            const size_t at = controller ? 0u : static_cast<size_t>(column);
            if (at >= list.size())
                return;
            const std::string removed = list[at];
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(at));
            storeRow(row, split);
            g_controlsStatus = rowName(row) + ": " + pc::displayName(removed) + " removed";
        }

        bool anyPadInput()
        {
            for (int device = 0; device < gp::kMaxDevices; ++device)
            {
                if (!gp::connected(device))
                    continue;
                for (int b = 0; b < gp::ButtonCount; ++b)
                    if (gp::button(device, static_cast<gp::Button>(b)))
                        return true;
                for (int a = 0; a < gp::AxisCount; ++a)
                    if (std::fabs(gp::axis(device, static_cast<gp::Axis>(a))) > 0.3f)
                        return true;
            }
            return false;
        }

        // A key both a game input and a hotkey would hold (K, Nov 14): a modal asks before assigning, whichever side
        // is being set. Plain keys only (a Shift / Ctrl hotkey does not fire on the bare key).
        struct KeyClash
        {
            bool game = false;     // true: a game input (row / column / binding); false: a hotkey (action / key)
            int row = 0, column = 0;
            std::string binding;
            ps2x::hotkeys::Action action = ps2x::hotkeys::Action::Count;
            ps2x::hotkeys::Binding key;
            std::string message;
        };
        KeyClash g_clash;
        bool g_openClash = false;

        void askKeyClash(KeyClash clash)
        {
            g_clash = std::move(clash);
            g_openClash = true;
        }

        std::string lowerFirst(std::string text)
        {
            if (!text.empty())
                text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
            return text;
        }

        // The game action a controls.ini key ("Cross", "LUp") stands for, as the table names it.
        std::string gameInputName(const std::string &key)
        {
            const auto same = [](std::string_view a, std::string_view b)
            {
                return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                                                          { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
            };
            for (int row = 0; row < kControlRowCount; ++row)
                if (same(key, kControlRows[row].key))
                    return rowName(row);
            return key;
        }

        // The capture in progress (every frame while its popup is open): true when it ended.
        bool captureTick()
        {
            if (IsKeyPressed(KEY_ESCAPE) || GetTime() - g_capture.started > 10.0)
            {
                g_controlsStatus = IsKeyPressed(KEY_ESCAPE) ? "Cancelled." : "Nothing pressed for 10 s: cancelled.";
                return true;
            }
            if (g_capture.waitRelease)
            {
                const bool held = g_capture.controller ? anyPadInput()
                                                       : (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
                                                          IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) || IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_SPACE));
                if (!held)
                    g_capture.waitRelease = false;
                return false;
            }
            if (!g_capture.controller)
            {
                for (int key : pc::bindableKeys())
                    if (IsKeyPressed(key))
                    {
                        const std::string binding = pc::keyBinding(key);
                        if (!binding.empty())
                        {
                            if (const ps2x::hotkeys::Action holder = ps2x::hotkeys::plainKeyHolder(key);
                                holder != ps2x::hotkeys::Action::Count)
                                askKeyClash(KeyClash{true, g_capture.row, g_capture.column, binding, {}, {},
                                                     pc::displayName(binding) + " is the hotkey for " +
                                                         lowerFirst(ps2x::hotkeys::info(holder).label) +
                                                         ". Pressing it will do both. Assign it anyway?"});
                            else
                                bindCell(g_capture.row, false, g_capture.column, binding);
                            return true;
                        }
                    }
                for (int mouse : {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_SIDE, MOUSE_BUTTON_EXTRA})
                    if (IsMouseButtonPressed(mouse))
                    {
                        bindCell(g_capture.row, false, g_capture.column, pc::mouseBinding(mouse));
                        return true;
                    }
                return false;
            }
            for (int device = 0; device < gp::kMaxDevices; ++device)
            {
                if (!gp::connected(device))
                    continue;
                for (int b = 0; b < gp::ButtonCount; ++b)
                    if (gp::button(device, static_cast<gp::Button>(b)))
                    {
                        bindCell(g_capture.row, true, 0, pc::padButtonBinding(device, b));
                        return true;
                    }
                for (int a = 0; a < gp::AxisCount; ++a)
                {
                    const float v = gp::axis(device, static_cast<gp::Axis>(a));
                    if (std::fabs(v) > 0.6f)
                    {
                        bindCell(g_capture.row, true, 0, pc::padAxisBinding(device, a, v < 0.0f));
                        return true;
                    }
                }
            }
            return false;
        }

        void startCapture(int row, bool controller, int column)
        {
            g_capture = Capture{true, controller, row, column, true, GetTime()};
            g_openCapture = true;
        }

        // One cell: click (or A/Enter) to bind, right-click (or X / Delete when focused) to clear.
        void bindingCell(int row, bool controller, int column, const std::string &binding, float width)
        {
            ImGui::PushID(row * 8 + (controller ? 7 : column));
            const std::string label = (binding.empty() ? std::string("-") : pc::displayName(binding)) + "##cell";
            if (ImGui::Button(label.c_str(), ImVec2(width, 0.0f)))
                startCapture(row, controller, column);
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                clearCell(row, controller, column);
            if (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                                           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)))
                clearCell(row, controller, column);
            ImGui::PopID();
        }

        // The port's shortcut keys (runtime/ps2_hotkeys.h): GHR.ini [Hotkeys], one line per action. The runtime's
        // table is set too, so a key bound here is live when the game starts from this launcher.
        namespace hk = ps2x::hotkeys;
        int g_hotkeyCapture = -1;
        double g_hotkeyStarted = 0.0;
        bool g_hotkeyWaitRelease = false; // the Enter / Space / click that opened the capture must go up first
        bool g_openHotkeyCapture = false;
        std::string g_hotkeyStatus;

        void storeHotkey(State &state, hk::Action action, hk::Binding binding)
        {
            hk::setBinding(action, binding);
            state.ini.set("Hotkeys", hk::info(action).id, binding.bound() ? hk::text(binding) : "none");
        }

        void loadHotkeys(State &state)
        {
            for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
            {
                const hk::Action action = static_cast<hk::Action>(i);
                const std::string value = state.ini.get("Hotkeys", hk::info(action).id, "\x01");
                hk::Binding binding;
                if (value == "\x01")
                    continue;
                if (hk::parse(value, binding))
                    hk::setBinding(action, binding);
                else // a controller button from an older build (keyboard-only since Nov 14): the default, written back
                    storeHotkey(state, action, hk::info(action).def);
            }
        }

        // The hotkey capture's frame: true when it is over (a key bound, a clash asked about, cancelled, timed out).
        bool hotkeyCaptureTick(State &state)
        {
            const hk::Action action = static_cast<hk::Action>(g_hotkeyCapture);
            if (g_hotkeyWaitRelease)
            {
                (void)hk::capturePressed(); // the queue holds the activating key still
                if (!(IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER) || IsKeyDown(KEY_SPACE) || IsMouseButtonDown(MOUSE_BUTTON_LEFT)))
                    g_hotkeyWaitRelease = false;
                return false;
            }
            if (IsKeyPressed(KEY_ESCAPE) || GetTime() - g_hotkeyStarted > 10.0)
            {
                g_hotkeyStatus = IsKeyPressed(KEY_ESCAPE) ? "Cancelled." : "Nothing pressed for 10 s: cancelled.";
                return true;
            }
            const int key = hk::capturePressed();
            if (key == 0)
                return false;
            const hk::Binding binding{key, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT),
                                      IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)};
            const hk::Action other = hk::conflict(action, binding);
            if (other != hk::Action::Count)
                g_hotkeyStatus = hk::text(binding) + " is already " + hk::info(other).label + "; not changed.";
            else if (const std::string use = pc::keyboardKeyUse(kPlayer, key); !use.empty() && !binding.shift && !binding.ctrl)
            {
                KeyClash clash;
                clash.action = action;
                clash.key = binding;
                clash.message = hk::text(binding) + " is assigned to " + gameInputName(use) +
                                " in the game's controls. Pressing it will do both. Assign it anyway?";
                askKeyClash(std::move(clash));
            }
            else
            {
                storeHotkey(state, action, binding);
                g_hotkeyStatus = std::string(hk::info(action).label) + ": " + hk::text(binding);
            }
            return true;
        }

        void drawHotkeys(State &state)
        {
            static bool loaded = false;
            if (!loaded)
            {
                loaded = true;
                loadHotkeys(state);
            }
            if (collapsibleHeading("Hotkeys", g_hotkeysOpen))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextWrapped("The port's own keys. Click one, then press the key (with Shift or Ctrl held if you want). "
                                   "Right-click clears it. The F2 menu's Controls tab edits the same list.");
                ImGui::PopStyleColor();
                if (ImGui::BeginTable("hotkeys", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.4f);
                    ImGui::TableHeadersRow();
                    for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
                    {
                        const hk::Action action = static_cast<hk::Action>(i);
                        if (!hk::available(action))
                            continue; // the dev keys in a release build
                        const hk::Info &info = hk::info(action);
                        const hk::Binding binding = hk::binding(action);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(info.label);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID(1000 + i);
                        const std::string label = (binding.bound() ? hk::text(binding) : std::string("-")) + "##hk";
                        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f)))
                        {
                            g_hotkeyCapture = i;
                            g_hotkeyStarted = GetTime();
                            g_openHotkeyCapture = true;
                            g_hotkeyWaitRelease = true;
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                        {
                            storeHotkey(state, action, hk::Binding{});
                            g_hotkeyStatus = std::string(info.label) + ": no key.";
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if (!g_hotkeyStatus.empty())
                    ImGui::TextColored(kTextDim, "%s", g_hotkeyStatus.c_str());
                if (ImGui::Button("Default hotkeys"))
                {
                    for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
                        if (hk::available(static_cast<hk::Action>(i)))
                            storeHotkey(state, static_cast<hk::Action>(i), hk::info(static_cast<hk::Action>(i)).def);
                    g_hotkeyStatus = "Default hotkeys restored.";
                }
            }

            if (g_openHotkeyCapture)
            {
                g_openHotkeyCapture = false;
                ImGui::OpenPopup("##hotkeycapture");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("##hotkeycapture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
            {
                g_captureFrame = true;
                const hk::Action action = static_cast<hk::Action>(g_hotkeyCapture);
                ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 0.5f));
                ImGui::TextUnformatted(hk::info(action).label);
                ImGui::TextColored(kTextDim, "Press a key (Shift or Ctrl with it if you like)...");
                ImGui::TextColored(kTextDim, "Esc cancels (%d s)", std::max(0, 10 - static_cast<int>(GetTime() - g_hotkeyStarted)));
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
                if (hotkeyCaptureTick(state))
                {
                    g_hotkeyCapture = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // The key-clash question (askKeyClash), opened the frame after the capture that found it closed.
        void drawKeyClash(State &state)
        {
            if (g_openClash)
            {
                g_openClash = false;
                ImGui::OpenPopup("##keyclash");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal("##keyclash", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
                return;
            g_captureFrame = true; // its Esc / B must not also leave Options
            const float width = ImGui::GetFontSize() * 22.0f;
            ImGui::Dummy(ImVec2(width, ImGui::GetFontSize() * 0.25f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextUnformatted(g_clash.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
            bool close = false;
            if (ImGui::Button("Assign"))
            {
                if (g_clash.game)
                    bindCell(g_clash.row, false, g_clash.column, g_clash.binding);
                else
                {
                    storeHotkey(state, g_clash.action, g_clash.key);
                    g_hotkeyStatus = std::string(hk::info(g_clash.action).label) + ": " + hk::text(g_clash.key);
                }
                close = true;
            }
            tickItem();
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            {
                (g_clash.game ? g_controlsStatus : g_hotkeyStatus) = "Not changed.";
                close = true;
            }
            tickItem();
            if (close)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // The Controls tab after its table settings: the bindings and hotkeys (launcher only).
        void drawControls(State &state)
        {
            categoryHeading("Bindings");
            static int device = 0; // 0 keyboard & mouse, 1 controller
            ImGui::TextUnformatted("Bindings for");
            ImGui::SameLine();
            ImGui::RadioButton("Keyboard & mouse", &device, 0);
            tickItem();
            ImGui::SameLine();
            ImGui::RadioButton("Controller", &device, 1);
            tickItem();
            const bool controller = device == 1;
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            ImGui::TextWrapped("%s", controller ? "Click a button, then press the controller button or move the stick or "
                                                  "trigger it should be. Right-click (or X) clears it. Esc cancels."
                                                : "Click a box, then press the key or click the mouse button it should be. "
                                                  "Right-click (or Delete) clears it. Esc cancels; F-keys stay the port's "
                                                  "shortcuts. Menus always take the arrows / WASD, Enter / Space (confirm), "
                                                  "Esc / Backspace (back) and Q / E (tabs).");
            if (pc::envOverride(kPlayer))
                ImGui::TextWrapped("PS2X_PAD1 is set in the environment: it wins over these bindings while it is set.");
            ImGui::PopStyleColor();
            if (ImGui::BeginTable("bindings", controller ? 2 : 1 + kKeyColumns,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("PS2 button", ImGuiTableColumnFlags_WidthStretch, 0.46f);
                if (controller)
                    ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 0.54f);
                else
                {
                    ImGui::TableSetupColumn("Key 1", ImGuiTableColumnFlags_WidthStretch, 0.27f);
                    ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthStretch, 0.27f);
                }
                ImGui::TableHeadersRow();
                for (int row = 0; row < kControlRowCount; ++row)
                {
                    if (controller && kControlRows[row].keysOnly)
                        continue;
                    const Split split = splitRow(row);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(kControlRows[row].button);
                    if (*kControlRows[row].action)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(kTextDim, "%s", kControlRows[row].action);
                    }
                    if (controller)
                    {
                        ImGui::TableSetColumnIndex(1);
                        bindingCell(row, true, 0, split.pads.empty() ? std::string() : split.pads[0], -1.0f);
                    }
                    else
                    {
                        for (int column = 0; column < kKeyColumns; ++column)
                        {
                            ImGui::TableSetColumnIndex(1 + column);
                            bindingCell(row, false, column, column < static_cast<int>(split.keys.size()) ? split.keys[column] : std::string(),
                                        -1.0f);
                        }
                    }
                }
                ImGui::EndTable();
            }
            if (!g_controlsStatus.empty())
                ImGui::TextColored(kTextDim, "%s", g_controlsStatus.c_str());
            if (ImGui::Button(g_resetArmed ? "Click again to reset every binding" : "Reset all bindings to the defaults"))
            {
                if (g_resetArmed)
                {
                    pc::resetDefaults(kPlayer);
                    g_controlsStatus = "Default bindings restored.";
                }
                g_resetArmed = !g_resetArmed;
            }
            ImGui::SameLine();
            if (ImGui::Button("Open controls.ini"))
            {
                std::string message;
                pc::save(message);
                platform::open(state.context->exeDir / "controls.ini");
            }

            drawHotkeys(state);

            // The capture popup.
            if (g_openCapture)
            {
                g_openCapture = false;
                ImGui::OpenPopup("##capture");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("##capture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
            {
                g_captureFrame = true;
                ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 0.5f));
                ImGui::TextUnformatted(rowName(g_capture.row).c_str());
                ImGui::TextColored(kTextDim, "%s", g_capture.controller ? "Press a controller button or move a stick or trigger..."
                                                                       : "Press a key or click a mouse button...");
                ImGui::TextColored(kTextDim, "Esc cancels (%d s)", std::max(0, 10 - static_cast<int>(GetTime() - g_capture.started)));
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
                if (captureTick())
                {
                    g_capture.active = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            drawKeyClash(state);
        }

        void saveControls()
        {
            if (!pc::dirty())
                return;
            std::string message;
            if (!pc::save(message))
                KM_WARN(Boot, "launcher: controls not saved: " << message);
        }

        // ---- the settings tabs (settings_table.cpp: the in-game OPTIONS screen draws the same list) ----------------
        // A combo over a setting's choices; a value that is none of them (set by hand) shows as one more entry and is
        // kept unless another is picked. Returns true when changed (value updated).
        bool tableCombo(const char *id, const settings::Setting &setting, std::string &value)
        {
            const int current = settings::choiceIndex(setting, value);
            std::vector<const char *> labels;
            for (int i = 0; i < setting.choiceCount; ++i)
                labels.push_back(setting.choices[i].label);
            const std::string custom = "Custom: " + value;
            int shown = current;
            if (current < 0)
            {
                labels.push_back(custom.c_str());
                shown = setting.choiceCount;
            }
            int picked = shown;
            ImGui::SetNextItemWidth(-1.0f);
            const bool changed = ImGui::Combo(id, &picked, labels.data(), static_cast<int>(labels.size()));
            tickItem();
            if (!changed || picked == shown || picked >= setting.choiceCount)
                return false;
            value = setting.choices[picked].value;
            return true;
        }

        // The chosen character's costumes (the Costume row shows only for a character that has some).
        const std::vector<characters::Costume> &costumesOf(const State &state)
        {
            static const std::vector<characters::Character> list = characters::list(state.context->exeDir);
            static const std::vector<characters::Costume> none;
            const std::string current = lower(state.ini.get("Mods", "Character", "gene"));
            for (const characters::Character &c : list)
                if (lower(c.id) == current)
                    return c.costumes;
            return none;
        }
        int costumeIndex(const State &state, const std::vector<characters::Costume> &costumes)
        {
            const std::string current = lower(state.ini.get("Mods", "Costume", ""));
            for (size_t i = 0; i < costumes.size(); ++i)
                if (costumes[i].id == current)
                    return static_cast<int>(i);
            return 0;
        }

        // The settings drawn by hand: Display Mode (two keys), Character, Costume, texture packs.
        void customControl(State &state, const settings::Setting &setting)
        {
            const std::string id = setting.id;
            if (id == "DisplayMode")
            {
                std::string mode = state.ini.getBool("Video", "StartFullscreen", true)    ? "fullscreen"
                                   : state.ini.getBool("Video", "StartMaximized", false) ? "maximized"
                                                                                         : "window";
                if (choiceCombo("##window", kWindowModes, mode))
                {
                    state.ini.setBool("Video", "StartFullscreen", mode == "fullscreen");
                    state.ini.setBool("Video", "StartMaximized", mode == "maximized");
                }
            }
            else if (id == "Character")
            {
                static std::vector<characters::Character> list;
                if (list.empty())
                    list = characters::list(state.context->exeDir);
                const std::string current = lower(state.ini.get("Mods", "Character", "gene"));
                int picked = 0;
                std::vector<const char *> names;
                for (size_t i = 0; i < list.size(); ++i)
                {
                    names.push_back(list[i].name.c_str());
                    if (lower(list[i].id) == current)
                        picked = static_cast<int>(i);
                }
                ImGui::SetNextItemWidth(-1.0f);
                const bool pickedNew = ImGui::Combo("##character", &picked, names.data(), static_cast<int>(names.size()));
                tickItem();
                if (pickedNew)
                    state.ini.set("Mods", "Character", list[static_cast<size_t>(picked)].id);
                if (!list.empty() && !list[static_cast<size_t>(picked)].credit.empty())
                    ImGui::TextColored(kTextDim, "%s", list[static_cast<size_t>(picked)].credit.c_str());
            }
            else if (id == "Costume")
            {
                const std::vector<characters::Costume> &costumes = costumesOf(state);
                int picked = costumeIndex(state, costumes);
                std::vector<const char *> names;
                for (const characters::Costume &c : costumes)
                    names.push_back(c.name.c_str());
                ImGui::SetNextItemWidth(-1.0f);
                const bool pickedNew = ImGui::Combo("##costume", &picked, names.data(), static_cast<int>(names.size()));
                tickItem();
                if (pickedNew && picked >= 0 && picked < static_cast<int>(costumes.size()))
                    state.ini.set("Mods", "Costume", costumes[static_cast<size_t>(picked)].id);
            }
            else if (id == "Textures")
            {
                iniCheckbox(state, "Use replacement textures##tex", "Runtime", "PS2X_PCSX2_TEXTURES", true);
                subSetting("Texture Pack Folder", "The folder that holds replacements/. Empty: textures/SLUS-21503 next to "
                                                  "the game.");
                const std::string folder = state.ini.get("Runtime", "PS2X_PCSX2_TEXTURE_DIR");
                ImGui::TextWrapped("%s", folder.empty() ? "(next to the game)" : folder.c_str());
                if (ImGui::Button("Choose folder..."))
                {
                    const fs::path picked = platform::pick(GetWindowHandle(), true);
                    if (!picked.empty())
                        state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", utf8(picked));
                }
                if (!folder.empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Use default"))
                        state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", "");
                }
            }
        }

        void settingControl(State &state, const settings::Setting &setting)
        {
            const std::string id = std::string("##") + setting.id;
            switch (setting.kind)
            {
            case settings::Kind::Toggle:
                iniCheckbox(state, ("On" + id).c_str(), setting.section, setting.key, std::strcmp(setting.fallback, "1") == 0);
                break;
            case settings::Kind::Choice:
            {
                std::string value = settings::value(state.ini, setting);
                if (tableCombo(id.c_str(), setting, value))
                {
                    if (std::strcmp(setting.key, "CameraPreset") == 0)
                        settings::writeCameraPreset(state.ini, value); // its four values (Custom: from Action's)
                    else
                        state.ini.set(setting.section, setting.key, value);
                }
                break;
            }
            case settings::Kind::Slider:
            {
                const int number = std::atoi(settings::value(state.ini, setting).c_str());
                int index = 0;
                for (int i = 1; i < setting.valueCount; ++i)
                    if (std::abs(setting.values[i] - number) < std::abs(setting.values[index] - number))
                        index = i;
                std::string format = std::to_string(setting.values[index]);
                for (const char *c = setting.unit; *c; ++c)
                    format += *c == '%' ? std::string("%%") : std::string(1, *c);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt(id.c_str(), &index, 0, setting.valueCount - 1, format.c_str(), ImGuiSliderFlags_AlwaysClamp))
                    state.ini.set(setting.section, setting.key, std::to_string(setting.values[index]));
                silentItem();
                break;
            }
            case settings::Kind::Custom:
                customControl(state, setting);
                break;
            }
        }

        bool otherCharacters(const State &state)
        {
            static const bool any = characters::list(state.context->exeDir).size() > 1;
            return any;
        }


        // One tab: its settings in the table's order, a heading per category. Sub-options show while their parent
        // allows them (Window Size greyed instead; Camera & FOV's values only with Custom).
        void drawTab(State &state, settings::Tab tab)
        {
            const bool narrow = lower(state.ini.get("Video", "AspectRatio", "16:9")) == "4:3";
            const char *category = nullptr;
            bool table = false;
            for (size_t i = 0; i < settings::kSettingCount; ++i)
            {
                const settings::Setting &s = settings::kSettings[i];
                if (s.tab != tab)
                    continue;
                const bool allowed = settings::parentAllows(state.ini, s);
                if (!allowed && !s.greyed)
                    continue;
                // Character only with a character besides Gene (Xavier, built from the disc at boot, ships), Costume only
                // for a character with costumes.
                if (std::strcmp(s.id, "Character") == 0 && !otherCharacters(state))
                    continue;
                if (std::strcmp(s.id, "Costume") == 0 && costumesOf(state).empty())
                    continue;
                if (!category || std::strcmp(category, s.category) != 0)
                {
                    if (table)
                        ImGui::EndTable();
                    category = s.category;
                    table = beginCategory(s.category);
                }
                if (!table)
                    continue;
                if (s.parent)
                    subSetting(s.name, s.description, s.note);
                else
                    settingLabel(s.name, s.description, s.note);
                ImGui::BeginDisabled(!allowed || (s.wideOnly && narrow));
                settingControl(state, s);
                ImGui::EndDisabled();
            }
            if (table)
                ImGui::EndTable();
        }

        // ---- Options > Presets (K, Nov 16; claude/presets-design.md) ------------------------------------------------
        // The pause menu's photos, decoded from the player's disc image (never shipped).
        void readPresetPhotos(State &state)
        {
            if (state.presetPhotosRead || !state.installed)
                return;
            state.presetPhotosRead = true;
            std::string iso = state.ini.get("Game", "Iso");
            const fs::path image = iso.empty() ? state.context->iso : fromUtf8(iso);
            for (int p = 0; p < presets::kCount; ++p)
            {
                std::vector<uint8_t> tm2, rgba;
                int width = 0, height = 0;
                if (!disc_files::readFromImage(image, presets::photoPath(presets::kText[p].photo), tm2) ||
                    !presets::decodePhoto(tm2, width, height, rgba))
                {
                    KM_WARN(Boot, "launcher: the " << presets::kText[p].name << " preset's photo could not be read");
                    continue;
                }
                ::Image picture{rgba.data(), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                state.presetPhotos[p] = LoadTextureFromImage(picture);
                if (state.presetPhotos[p].id != 0u)
                    SetTextureFilter(state.presetPhotos[p], TEXTURE_FILTER_BILINEAR);
            }
        }

        void usePreset(State &state, int preset)
        {
            presets::write(state.ini, preset);
            state.ini.save();
            KM_INFO(Boot, "launcher: preset " << presets::kText[preset].name);
        }

        // A row per preset (its photo, name, tagline and what it adds, Use / In use); while the settings match none a
        // Custom banner on top, and Use asks before replacing them.
        void drawPresets(State &state)
        {
            readPresetPhotos(state);
            const int now = presets::current(state.ini);
            categoryGap();
            if (now == presets::kCustom)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
                ImGui::SeparatorText("Custom");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextWrapped("%s %s", presets::kCustomText.tagline, presets::kCustomText.band);
                ImGui::PopStyleColor();
                categoryGap();
            }
            const float photoH = std::round(ImGui::GetFontSize() * 5.6f), photoW = std::round(photoH * presets::kPhotoAspect);
            const float buttonWidth = std::round(ImGui::GetFontSize() * 6.0f);
            if (!ImGui::BeginTable("presets", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX))
                return;
            ImGui::TableSetupColumn("photo", ImGuiTableColumnFlags_WidthFixed, photoW);
            ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("use", ImGuiTableColumnFlags_WidthFixed, buttonWidth);
            const float gap = std::round(ImGui::GetFontSize() * 0.45f);
            for (int p = 0; p < presets::kCount; ++p)
            {
                const presets::Text &text = presets::kText[p];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (p > 0)
                {
                    ImGui::Dummy(ImVec2(0.0f, gap));
                    dividerLine();
                    ImGui::Dummy(ImVec2(0.0f, gap));
                }
                if (state.presetPhotos[p].id != 0u)
                    rlImGuiImageSize(&state.presetPhotos[p], static_cast<int>(photoW), static_cast<int>(photoH));
                else
                    ImGui::Dummy(ImVec2(photoW, photoH));
                ImGui::TableSetColumnIndex(1);
                if (p > 0)
                    ImGui::Dummy(ImVec2(0.0f, 2.0f * gap + ImGui::GetStyle().ItemSpacing.y));
                pushSize(state, 26.0f);
                ImGui::TextUnformatted(text.name);
                ImGui::PopFont();
                ImGui::PushStyleColor(ImGuiCol_Text, kTextSoft);
                ImGui::TextWrapped("%s", text.tagline);
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextWrapped("%s", text.band);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(2);
                if (p > 0)
                    ImGui::Dummy(ImVec2(0.0f, 2.0f * gap + ImGui::GetStyle().ItemSpacing.y));
                ImGui::Dummy(ImVec2(0.0f, std::round(ImGui::GetFontSize() * 0.4f)));
                if (p == now)
                {
                    const char *inUse = "In use";
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 0.5f * (buttonWidth - ImGui::CalcTextSize(inUse).x));
                    ImGui::TextColored(kHeading, "%s", inUse);
                }
                else
                {
                    ImGui::PushID(p);
                    if (ImGui::Button("Use", ImVec2(buttonWidth, 0.0f)))
                    {
                        if (now == presets::kCustom)
                        {
                            state.presetAsk = p;
                            state.presetOpenPopup = true;
                        }
                        else
                            usePreset(state, p);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();

            // "Replace your custom settings?"
            if (state.presetOpenPopup)
            {
                state.presetOpenPopup = false;
                ImGui::OpenPopup("##presetreplace");
            }
            if (!ImGui::BeginPopupModal("##presetreplace", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
                return;
            const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            const float popupButton = std::round(ImGui::GetFontSize() * 7.5f);
            ImGui::TextUnformatted("Replace your custom settings?");
            if (state.presetAsk >= 0 && state.presetAsk < presets::kCount)
                ImGui::TextColored(kTextDim, "%s sets them to its own.", presets::kText[state.presetAsk].name);
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
            const bool keep = ImGui::Button("Keep mine", ImVec2(popupButton, 0.0f)); // first: the safe one
            ownSoundItem();
            if (ImGui::IsWindowAppearing())
                ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            const bool replace = ImGui::Button("Replace", ImVec2(popupButton, 0.0f));
            ownSoundItem();
            if (replace && state.presetAsk >= 0 && state.presetAsk < presets::kCount)
            {
                menuSound(state, kConfirm);
                usePreset(state, state.presetAsk);
                state.presetAsk = -1;
                ImGui::CloseCurrentPopup();
            }
            else if (keep || back)
            {
                menuSound(state, kCancel);
                state.presetAsk = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ---- the first run: a wanted poster per preset (K, Nov 16) ---------------------------------------------------
        void loadPosters(State &state)
        {
            if (state.postersLoaded)
                return;
            state.postersLoaded = true;
            const fs::path dir = kamite::paths::resources() / "presets";
            std::ifstream layout(dir / "posters.txt");
            std::string word;
            while (layout >> word)
            {
                if (word == "size")
                    layout >> state.posterW >> state.posterH >> state.posterPad;
                else if (word == "poster")
                {
                    int n = -1;
                    float v[6] = {};
                    layout >> n >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5];
                    if (n >= 0 && n < presets::kCount)
                    {
                        std::copy(v, v + 4, state.photoBox[n]);
                        std::copy(v + 4, v + 6, state.stampAt[n]);
                    }
                }
                else
                    std::getline(layout, word); // a comment
            }
            const auto load = [&](Texture2D &texture, const std::string &name) {
                const fs::path path = dir / name;
                std::error_code error;
                if (!fs::is_regular_file(path, error))
                    return;
                texture = LoadTexture(utf8(path).c_str());
                if (texture.id != 0u)
                {
                    GenTextureMipmaps(&texture);
                    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
                }
            };
            for (int n = 0; n < presets::kCount; ++n)
            {
                load(state.poster[n], "poster_" + std::to_string(n) + ".png");
                load(state.posterTape[n], "poster_" + std::to_string(n) + "_tape.png");
                load(state.posterShadow[n], "poster_" + std::to_string(n) + "_shadow.png");
            }
            load(state.stamp, "stamp.png");
            readPresetPhotos(state);
        }

        // One layer of a poster: its rectangle (x, y, w, h in the poster's pixels) drawn with the poster at `centre`,
        // `scale` screen pixels per poster pixel, turned `angle` degrees (clockwise) round the poster's centre.
        void posterLayer(const State &state, const Texture2D &texture, float x, float y, float w, float h, Vector2 centre,
                         float scale, float angle, Color tint)
        {
            if (texture.id == 0u)
                return;
            DrawTexturePro(texture, Rectangle{0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)},
                           Rectangle{centre.x, centre.y, w * scale, h * scale},
                           Vector2{(0.5f * state.posterW - x) * scale, (0.5f * state.posterH - y) * scale}, angle, tint);
        }

        // Where the posters go this frame (screen pixels): the first one's left, their top, width and the gap.
        struct PosterRow
        {
            float left = 0.0f, top = 0.0f, width = 0.0f, height = 0.0f, gap = 0.0f;
        };
        PosterRow posterRow(const State &state)
        {
            PosterRow row;
            const float screenW = static_cast<float>(GetScreenWidth());
            row.gap = fontSize(state, 22.0f);
            const float margin = fontSize(state, 40.0f);
            row.width = std::min(fontSize(state, 267.0f), (screenW - 2.0f * margin - 3.0f * row.gap) / 4.0f);
            row.height = row.width * (state.posterH > 0.0f ? state.posterH / state.posterW : 1.6f);
            row.left = 0.5f * (screenW - 4.0f * row.width - 3.0f * row.gap);
            row.top = fontSize(state, 112.0f);
            return row;
        }

        // The posters (after ImGui: raylib draws them over the launcher's window): the picked one raised and stamped,
        // the others a little lower and darker, each turned a little, on its own soft shadow.
        void drawPosters(State &state)
        {
            if (state.screen != Screen::FirstRun || state.posterW <= 0.0f)
                return;
            constexpr float kTurn[presets::kCount] = {1.6f, -1.1f, 0.8f, -1.4f}; // degrees, clockwise (the mockup's)
            state.pickShown += (static_cast<float>(state.firstRunPick) - state.pickShown) * std::min(1.0f, GetFrameTime() * 14.0f);
            const PosterRow row = posterRow(state);
            const float scale = row.width / state.posterW;
            for (int n = 0; n < presets::kCount; ++n)
            {
                const float lift = std::clamp(1.0f - std::fabs(state.pickShown - static_cast<float>(n)), 0.0f, 1.0f);
                const Vector2 centre{row.left + (row.width + row.gap) * static_cast<float>(n) + 0.5f * row.width,
                                     row.top + 0.5f * row.height + fontSize(state, 9.0f) * (1.0f - lift)};
                const float angle = kTurn[n];
                const unsigned char shade = static_cast<unsigned char>(184.0f + 71.0f * lift);
                const Color tint{shade, shade, shade, 255};
                const Vector2 shadowAt{centre.x + fontSize(state, 3.3f + 3.4f * lift), centre.y + fontSize(state, 5.3f + 5.4f * lift)};
                posterLayer(state, state.posterShadow[n], -state.posterPad, -state.posterPad, state.posterW + 2.0f * state.posterPad,
                            state.posterH + 2.0f * state.posterPad, shadowAt, scale, angle,
                            Color{255, 255, 255, static_cast<unsigned char>(120.0f + 40.0f * lift)});
                posterLayer(state, state.poster[n], 0.0f, 0.0f, state.posterW, state.posterH, centre, scale, angle, tint);
                const float *box = state.photoBox[n];
                posterLayer(state, state.presetPhotos[n], box[0], box[1], box[2] - box[0], box[3] - box[1], centre, scale, angle, tint);
                posterLayer(state, state.posterTape[n], box[0], box[1], box[2] - box[0], box[3] - box[1], centre, scale, angle, tint);
                if (n == state.firstRunPick && state.stamp.id != 0u)
                {
                    const float sw = static_cast<float>(state.stamp.width), sh = static_cast<float>(state.stamp.height);
                    posterLayer(state, state.stamp, state.stampAt[n][0] - 0.5f * sw, state.stampAt[n][1] - 0.5f * sh, sw, sh, centre,
                                scale, angle, WHITE);
                }
            }
        }

        // "How do you want to play?": left / right (or a click) pick a poster, Continue uses its preset.
        void drawFirstRun(State &state)
        {
            loadPosters(state);
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 26.0f)));
            pushSize(state, 40.0f);
            centeredText("How do you want to play?", kHeading);
            ImGui::PopFont();
            pushSize(state, 20.0f);
            centeredText("Pick a preset to start with. You can change it, or any single option, later in Options or in game.",
                         kTextSoft);
            ImGui::PopFont();
            int pick = state.firstRunPick;
            const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            if (!popup)
            {
                const auto pressed = [](std::initializer_list<ImGuiKey> keys) {
                    for (const ImGuiKey key : keys)
                        if (ImGui::IsKeyPressed(key, true))
                            return true;
                    return false;
                };
                if (pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}))
                    pick = std::max(pick - 1, 0);
                else if (pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}))
                    pick = std::min(pick + 1, static_cast<int>(presets::kCount) - 1);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    const PosterRow row = posterRow(state);
                    const ImVec2 mouse = ImGui::GetMousePos();
                    for (int n = 0; n < presets::kCount; ++n)
                    {
                        const float x = row.left + (row.width + row.gap) * static_cast<float>(n);
                        if (mouse.x >= x && mouse.x < x + row.width && mouse.y >= row.top && mouse.y < row.top + row.height)
                            pick = n;
                    }
                }
            }
            if (pick != state.firstRunPick)
            {
                state.firstRunPick = pick;
                playMenuSound(state, kTick);
            }
            const PosterRow row = posterRow(state);
            ImGui::SetCursorPosY(row.top + row.height + fontSize(state, 40.0f));
            const std::string label = std::string("Continue with ") + presets::kText[state.firstRunPick].name;
            if (state.focusFirst)
            {
                ImGui::SetKeyboardFocusHere();
                ImGui::SetNavCursorVisible(true);
            }
            if (bigButton(state, label.c_str(), fontSize(state, 420.0f), true))
            {
                usePreset(state, state.firstRunPick);
                state.ini.setBool("Launcher", "PresetPicked", true);
                state.ini.save();
                state.firstRunPending = false;
                state.screen = Screen::Home;
                state.focusFirst = true;
            }
            state.focusFirst = false;
        }

        // ---- Options in the book (stage 2, K Nov 18; claude/saloon-launcher.md "Stage 2") -----------------------------
        // The open book on the zoom's last frame: bookmark slips on its top edge for the tabs (LB / RB, Q / E, a click),
        // the tab's rows on the left page (they scroll), the focused row's help on the right page, Controls' own index
        // tabs on the right page's outer edge (LT / RT, Z / C), a ribbon from the spine's foot for Back. The rows keep
        // the launcher's logic (settings_table: the same names, order, sub-options hidden while their parent is off).
        // Laid out in the book's virtual pixels (launcher_book.h): a page is 1000 units tall, `u` pixels a unit.
        constexpr settings::Tab kBookSettingTabs[] = {settings::Tab::Display, settings::Tab::Graphics, settings::Tab::Interface,
                                                      settings::Tab::Gameplay, settings::Tab::Sound, settings::Tab::Controls,
                                                      settings::Tab::Mods};
        constexpr const char *kBookTabs[] = {"Presets", "Display", "Graphics", "Interface", "Gameplay", "Sound", "Controls", "Mods"};
        constexpr int kBookTabCount = static_cast<int>(std::size(kBookTabs));
        constexpr int kBookControlsTab = 6;
        constexpr int kBookModsTab = 7;
        constexpr const char *kControlsPages[] = {"Settings", "Controller", "Keys & Mouse", "Hotkeys"};
        constexpr int kControlsPageCount = static_cast<int>(std::size(kControlsPages));
        constexpr ImU32 kBookInk = IM_COL32(38, 26, 18, 238), kBookFaded = IM_COL32(38, 26, 18, 105);
        constexpr ImU32 kBookPencil = IM_COL32(164, 30, 22, 225), kBookPencilFaded = IM_COL32(164, 30, 22, 105);
        constexpr ImU32 kBookCream = IM_COL32(240, 222, 190, 240);
        constexpr float kBookLoopTime = 0.28f; // the bill's loop
        constexpr float kBookTextScale = 1.4f; // the pages' writing against the book's unit (K, Nov 18: big enough for a TV)
        constexpr float kHeadingScale = 1.3f;  // Wendyville is narrow: its capitals drawn bigger than asked (K's pick)
        // The slips' manila, a little different each (the open one lighter, in front).
        constexpr ImU32 kSlipTints[kBookTabCount] = {IM_COL32(244, 237, 219, 255), IM_COL32(240, 233, 217, 255), IM_COL32(243, 235, 222, 255),
                                                     IM_COL32(239, 234, 219, 255), IM_COL32(242, 236, 218, 255), IM_COL32(241, 233, 220, 255),
                                                     IM_COL32(243, 236, 219, 255), IM_COL32(240, 234, 218, 255)};
        constexpr ImU32 kSlipOpen = IM_COL32(255, 251, 240, 255);

        struct Book
        {
            const book::Space *space = nullptr;
            float u = 1.0f;
            ImDrawList *dl = nullptr;          // the main window's: slips, ribbon, the right page
            ImVec2 leftMin, leftMax;           // the left page's text area
            ImVec2 rightMin, rightMax;         // the right page's
            float top = 0.0f, bottom = 0.0f;   // the pages' edges (virtual y)
            float left = 0.0f, right = 0.0f;   // the spread's outer edges (virtual x)
        };

        // The headings' font (Wendyville) is set in capitals, as on the wanted posters (its lowercase m, n and y are
        // other shapes), and bigger: textSize and bookText do both for it.
        ImFont *g_headingFont = nullptr;
        std::string capsFor(ImFont *font, const char *text)
        {
            std::string t = text ? text : "";
            if (font == g_headingFont)
                for (char &c : t)
                    if (c >= 'a' && c <= 'z')
                        c = static_cast<char>(c - 'a' + 'A');
            return t;
        }

        ImVec2 textSize(ImFont *font, float size, const char *text, float wrap = 0.0f)
        {
            if (font == nullptr || text == nullptr)
                return ImVec2(0.0f, 0.0f);
            return font->CalcTextSizeA(font == g_headingFont ? size * kHeadingScale : size, FLT_MAX, wrap, capsFor(font, text).c_str());
        }

        void bookText(ImDrawList *dl, ImFont *font, float size, ImVec2 pos, ImU32 col, const char *text, float wrap = 0.0f)
        {
            if (font != nullptr && text != nullptr && *text)
                dl->AddText(font, font == g_headingFont ? size * kHeadingScale : size, pos, ImGui::GetColorU32(col), capsFor(font, text).c_str(), nullptr, wrap);
        }

        // Turns what was drawn since `start` (a vertex index) about `centre`.
        void rotateSince(ImDrawList *dl, int start, ImVec2 centre, float radians)
        {
            const float c = std::cos(radians), s = std::sin(radians);
            for (int i = start; i < dl->VtxBuffer.Size; ++i)
            {
                ImDrawVert &v = dl->VtxBuffer[i];
                const float dx = v.pos.x - centre.x, dy = v.pos.y - centre.y;
                v.pos = ImVec2(centre.x + dx * c - dy * s, centre.y + dx * s + dy * c);
            }
        }

        // A quad with a colour per corner (a, b, c, d around it).
        void quadColors(ImDrawList *dl, ImVec2 a, ImVec2 b, ImVec2 c, ImVec2 d, ImU32 ca, ImU32 cb, ImU32 cc, ImU32 cd)
        {
            const ImVec2 uv = dl->_Data->TexUvWhitePixel;
            const ImDrawIdx i = static_cast<ImDrawIdx>(dl->_VtxCurrentIdx);
            dl->PrimReserve(6, 4);
            dl->PrimWriteIdx(i);
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 1));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
            dl->PrimWriteIdx(i);
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 2));
            dl->PrimWriteIdx(static_cast<ImDrawIdx>(i + 3));
            dl->PrimWriteVtx(a, uv, ImGui::GetColorU32(ca));
            dl->PrimWriteVtx(b, uv, ImGui::GetColorU32(cb));
            dl->PrimWriteVtx(c, uv, ImGui::GetColorU32(cc));
            dl->PrimWriteVtx(d, uv, ImGui::GetColorU32(cd));
        }

        // A picture painted onto the page in 4x4 pieces (the homography bends it; one quad would kink on its diagonal).
        void bookImage(ImDrawList *dl, const Texture2D &texture, ImVec2 min, ImVec2 max, ImU32 tint = IM_COL32_WHITE)
        {
            if (texture.id == 0u)
                return;
            constexpr int kPieces = 4;
            for (int y = 0; y < kPieces; ++y)
                for (int x = 0; x < kPieces; ++x)
                {
                    const float u0 = static_cast<float>(x) / kPieces, u1 = static_cast<float>(x + 1) / kPieces;
                    const float v0 = static_cast<float>(y) / kPieces, v1 = static_cast<float>(y + 1) / kPieces;
                    const ImVec2 a(min.x + (max.x - min.x) * u0, min.y + (max.y - min.y) * v0);
                    const ImVec2 b(min.x + (max.x - min.x) * u1, min.y + (max.y - min.y) * v1);
                    dl->AddImage(ImTextureID(texture.id), a, b, ImVec2(u0, v0), ImVec2(u1, v1), ImGui::GetColorU32(tint));
                }
        }

        // A prompt (the pack's picture) with a faint inked edge, so the white ones read on the cream paper (K, Nov 18).
        void bookPrompt(ImDrawList *dl, const Texture2D &texture, ImVec2 centre, float size)
        {
            if (texture.id == 0u)
                return;
            const float h = size * 0.5f, w = h * static_cast<float>(texture.width) / std::max(1, texture.height);
            const ImVec2 min(centre.x - w, centre.y - h), max(centre.x + w, centre.y + h);
            const float e = std::max(1.0f, size * 0.03f);
            for (const ImVec2 &o : {ImVec2(-e, 0.0f), ImVec2(e, 0.0f), ImVec2(0.0f, -e), ImVec2(0.0f, e)})
                dl->AddImage(ImTextureID(texture.id), ImVec2(min.x + o.x, min.y + o.y), ImVec2(max.x + o.x, max.y + o.y), ImVec2(0, 0),
                             ImVec2(1, 1), ImGui::GetColorU32(IM_COL32(38, 26, 18, 70)));
            dl->AddImage(ImTextureID(texture.id), min, max, ImVec2(0, 0), ImVec2(1, 1), ImGui::GetColorU32(IM_COL32_WHITE));
        }

        // A book item: an invisible button (a keyboard / gamepad stop unless `nav` is off); the mouse focuses what it
        // moves over, as on the bill; a new focus starts its loop and the pen.
        bool bookItem(State &state, const char *id, ImVec2 min, ImVec2 max, bool nav = true)
        {
            ImGui::SetCursorScreenPos(min);
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, max.x - min.x), std::max(1.0f, max.y - min.y)),
                                                        nav ? ImGuiButtonFlags_EnableNav : ImGuiButtonFlags_None);
            ImGui::PopStyleColor();
            if (!nav)
            {
                ownSoundItem(); // the slips, the index tabs, the ribbon, USE THIS: their own sounds (K: no menu sound)
                return pressed;
            }
            const ImGuiIO &io = ImGui::GetIO();
            const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
            if (moved && ImGui::IsItemHovered() && !ImGui::IsItemFocused())
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            if (state.focusFirst && !state.optionsLeaving)
            {
                state.focusFirst = false;
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
            }
            if (ImGui::IsItemFocused() && ImGui::GetItemID() != state.bookFocusId)
            {
                state.bookFocusId = ImGui::GetItemID();
                state.bookFocusAge = 0.0f;
                state.bookFocusSeed = static_cast<uint32_t>(std::rand()) * 2654435761u + 1u;
                if (state.bookQuiet == 0)
                    penSound(state);
            }
            return pressed;
        }

        // The focus loop round [min, max] (drawn in over kBookLoopTime). It reaches out past its word, past the left
        // page's text area too (the scrolling child clips to that): while the word itself is in view, it is drawn
        // under the book's own edges instead (K, Nov 18: clipped loops).
        void bookLoop(State &state, const Book &b, ImDrawList *dl, ImVec2 min, ImVec2 max)
        {
            const ImVec4 clip = dl->_CmdHeader.ClipRect;
            const bool inView = min.x >= clip.x - 1.0f && min.y >= clip.y - 1.0f && max.x <= clip.z + 1.0f && max.y <= clip.w + 1.0f;
            if (inView)
                dl->PushClipRect(ImVec2(b.left, b.top), ImVec2(b.right, b.bottom), false);
            book::loop(dl, min, max, state.bookFocusAge / kBookLoopTime, state.bookFocusSeed, kBookPencil, 3.6f * b.u);
            if (inView)
                dl->PopClipRect();
        }

        void bookHelp(State &state, const char *title, const char *help, const char *note = nullptr)
        {
            state.bookTitle = title ? title : "";
            state.bookHelp = help ? help : "";
            state.bookNote = note ? note : "";
        }

        // A hand-ruled line (a few points, each a hair off).
        void bookRule(const Book &b, ImDrawList *dl, float x0, float x1, float y, ImU32 color, float width, uint32_t seed)
        {
            ImVec2 points[book::kMaxLinePoints];
            const int n = std::clamp(static_cast<int>((x1 - x0) / (40.0f * b.u)) + 2, 2, book::kMaxLinePoints);
            uint32_t r = seed * 2654435761u + 7u;
            for (int i = 0; i < n; ++i)
            {
                r = r * 1664525u + 1013904223u;
                const float jitter = (static_cast<float>(r >> 8) / 16777216.0f - 0.5f) * 0.9f * b.u;
                points[i] = ImVec2(x0 + (x1 - x0) * static_cast<float>(i) / (n - 1), y + (i == 0 || i == n - 1 ? 0.0f : jitter));
            }
            book::line(dl, points, n, width, color);
        }

        // ---- rows (the left page) ------------------------------------------------------------------------------------
        constexpr float kRowH = 44.0f, kCatH = 60.0f;

        struct Row
        {
            bool pressed = false, focused = false, active = false;
            ImVec2 min, max;
            float mid = 0.0f;       // the row's middle (y)
            float nameRight = 0.0f; // where its name ends (x)
        };

        void bookCategory(State &state, const Book &b, const char *title, bool first)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u, w = ImGui::GetContentRegionAvail().x;
            if (!first)
                ImGui::Dummy(ImVec2(w, 10.0f * u));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float size = 25.0f * u;
            bookText(dl, state.bookFonts.heading, size, ImVec2(p.x, p.y + 12.0f * u), kBookInk, title);
            const float tw = textSize(state.bookFonts.heading, size, title).x;
            if (p.x + tw + 12.0f * u < p.x + w)
                bookRule(b, dl, p.x + tw + 12.0f * u, p.x + w, p.y + 12.0f * u + size * 0.62f, kBookInk, 1.3f * u,
                         static_cast<uint32_t>(std::strlen(title)));
            ImGui::Dummy(ImVec2(w, kCatH * u - 10.0f * u));
        }

        Row bookRow(State &state, const Book &b, const char *id, const char *name, const char *help, const char *note, bool sub,
                    bool faded, bool silent = false)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u, w = ImGui::GetContentRegionAvail().x;
            Row r;
            r.min = ImGui::GetCursorScreenPos();
            r.max = ImVec2(r.min.x + w, r.min.y + kRowH * u);
            r.mid = r.min.y + kRowH * u * 0.5f;
            ImGui::BeginDisabled(faded);
            r.pressed = bookItem(state, id, r.min, r.max);
            ImGui::EndDisabled();
            if (silent)
                silentItem();
            else
                tickItem();
            r.focused = ImGui::IsItemFocused();
            r.active = ImGui::IsItemActive();
            const float x = r.min.x + (sub ? 26.0f * u : 0.0f), size = 26.0f * u;
            const ImU32 ink = faded ? kBookFaded : kBookInk;
            if (sub)
            {
                const ImVec2 elbow[3] = {ImVec2(r.min.x + 7.0f * u, r.min.y + 6.0f * u), ImVec2(r.min.x + 7.0f * u, r.mid + 1.0f * u),
                                         ImVec2(r.min.x + 19.0f * u, r.mid + 1.0f * u)};
                book::line(dl, elbow, 3, 1.2f * u, IM_COL32(38, 26, 18, 120));
            }
            const ImVec2 at(x, r.mid - size * 0.56f);
            bookText(dl, state.bookFonts.text, size, at, ink, name);
            r.nameRight = at.x + textSize(state.bookFonts.text, size, name).x;
            if (r.focused)
            {
                const ImVec2 ts = textSize(state.bookFonts.text, size, name);
                bookLoop(state, b, dl, ImVec2(at.x, at.y + size * 0.12f), ImVec2(at.x + ts.x, at.y + size * 1.02f));
                bookHelp(state, name, help, note);
            }
            ImGui::SetCursorScreenPos(ImVec2(r.min.x, r.max.y));
            ImGui::Dummy(ImVec2(w, 0.0f));
            return r;
        }

        void bookValue(State &state, const Book &b, const Row &r, const char *text, bool faded)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u, room = r.max.x - 20.0f * u - (r.nameRight + 22.0f * u);
            const float full = textSize(state.bookFonts.text, 26.0f * u, text).x;
            const float size = 26.0f * u * (full > room && room > 0.0f ? std::max(0.6f, room / full) : 1.0f); // a long value beside a long name
            const float tw = textSize(state.bookFonts.text, size, text).x;
            const float right = r.max.x - 20.0f * u;
            bookText(dl, state.bookFonts.text, size, ImVec2(right - tw, r.mid - size * 0.56f), faded ? kBookPencilFaded : kBookPencil, text);
            const ImVec2 caret[3] = {ImVec2(right + 5.0f * u, r.mid - 3.0f * u), ImVec2(right + 10.5f * u, r.mid + 3.0f * u),
                                     ImVec2(right + 16.0f * u, r.mid - 3.0f * u)};
            book::line(dl, caret, 3, 2.0f * u, faded ? kBookPencilFaded : kBookPencil);
        }

        // A dropdown's list: a paper slip under the row's value, a line per choice (the current one in red). The pick
        // (index) or -1.
        int bookList(State &state, const Book &b, const char *popup, const Row &r, const std::vector<std::string> &labels, int current)
        {
            const float u = b.u, size = 26.0f * u, line = 40.0f * u, padX = 22.0f * u, padY = 12.0f * u;
            float width = 150.0f * u;
            for (const std::string &l : labels)
                width = std::max(width, textSize(state.bookFonts.text, size, l.c_str()).x + 2.0f * padX);
            ImGui::SetNextWindowPos(ImVec2(r.max.x + 4.0f * u, r.max.y - 4.0f * u), ImGuiCond_Appearing, ImVec2(1.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
            const bool open = ImGui::BeginPopup(popup, ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleVar(3);
            ImGui::PopStyleColor(2);
            if (!open)
                return -1;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const float height = padY * 2.0f + line * static_cast<float>(labels.size());
            const ImVec2 quad[4] = {p, ImVec2(p.x + width, p.y), ImVec2(p.x + width, p.y + height), ImVec2(p.x, p.y + height)};
            book::paper(dl, quad, IM_COL32(255, 251, 242, 255), 8.0f * u);
            book::ink(dl);
            int picked = -1;
            for (int i = 0; i < static_cast<int>(labels.size()); ++i)
            {
                ImGui::PushID(i);
                const ImVec2 a(p.x, p.y + padY + line * static_cast<float>(i)), z(p.x + width, a.y + line);
                if (bookItem(state, "##pick", a, z))
                    picked = i;
                if (ImGui::IsWindowAppearing() && i == std::max(0, current))
                {
                    ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                    ImGui::SetNavCursorVisible(true);
                }
                const ImVec2 at(a.x + padX, a.y + (line - size) * 0.5f - size * 0.06f);
                bookText(dl, state.bookFonts.text, size, at, i == current ? kBookPencil : kBookInk, labels[static_cast<size_t>(i)].c_str());
                if (ImGui::IsItemFocused())
                {
                    const float tw = textSize(state.bookFonts.text, size, labels[static_cast<size_t>(i)].c_str()).x;
                    bookLoop(state, b, dl, ImVec2(at.x, at.y + size * 0.12f), ImVec2(at.x + tw, at.y + size * 1.02f));
                }
                ImGui::PopID();
            }
            ImGui::SetCursorScreenPos(p);
            ImGui::Dummy(ImVec2(width, height));
            if (picked >= 0)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
            return picked;
        }

        // A ruler with the red paperclip at the value (stop of stops) and the value written after it.
        void bookRuler(State &state, const Book &b, const Row &r, int stop, int stops, const char *text, bool faded,
                       float &x0, float &x1)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u;
            const float valueW = std::max(textSize(state.bookFonts.text, 26.0f * u, text).x, textSize(state.bookFonts.text, 26.0f * u, "150%").x);
            x1 = r.max.x - valueW - 16.0f * u;
            x0 = std::min(x1 - 50.0f * u, std::max(x1 - 150.0f * u, r.nameRight + 22.0f * u));
            const float y = r.mid + 5.0f * u;
            const ImU32 ink = faded ? kBookFaded : kBookInk;
            bookRule(b, dl, x0, x1, y, ink, 1.4f * u, 3u);
            for (int i = 0; i <= 10; ++i)
            {
                const float x = x0 + (x1 - x0) * static_cast<float>(i) / 10.0f, h = (i % 5 == 0 ? 9.0f : 5.0f) * u;
                const ImVec2 tick[2] = {ImVec2(x, y), ImVec2(x, y - h)};
                book::line(dl, tick, 2, 1.1f * u, ink);
            }
            const float px = x0 + (x1 - x0) * (stops > 1 ? static_cast<float>(stop) / (stops - 1) : 0.0f);
            const ImVec2 clip[6] = {ImVec2(px - 5.0f * u, y + 10.0f * u), ImVec2(px - 5.0f * u, y - 18.0f * u), ImVec2(px + 5.0f * u, y - 18.0f * u),
                                    ImVec2(px + 5.0f * u, y + 6.0f * u),  ImVec2(px - 1.0f * u, y + 6.0f * u),  ImVec2(px - 1.0f * u, y - 12.0f * u)};
            book::paint(dl);
            dl->AddPolyline(clip, 6, ImGui::GetColorU32(faded ? IM_COL32(150, 40, 40, 140) : IM_COL32(150, 26, 26, 255)), ImDrawFlags_None, 3.0f * u);
            book::ink(dl);
            const float size = 26.0f * u, tw = textSize(state.bookFonts.text, size, text).x;
            bookText(dl, state.bookFonts.text, size, ImVec2(r.max.x - tw, r.mid - size * 0.56f), faded ? kBookPencilFaded : kBookPencil, text);
        }

        // The step a ruler's row takes from the keys / pad this frame (-1, 0, +1) while it is focused.
        int rulerStep()
        {
            const auto pressed = [](ImGuiKey key) { return ImGui::IsKeyPressed(key, true); };
            if (pressed(ImGuiKey_LeftArrow) || pressed(ImGuiKey_GamepadDpadLeft) || pressed(ImGuiKey_GamepadLStickLeft))
                return -1;
            if (pressed(ImGuiKey_RightArrow) || pressed(ImGuiKey_GamepadDpadRight) || pressed(ImGuiKey_GamepadLStickRight))
                return 1;
            return 0;
        }

        // A setting from the table as a row: a tick box, a dropdown or a ruler; Display Mode, Character and the texture
        // packs as the launcher draws them by hand.
        void bookSetting(State &state, const Book &b, size_t index)
        {
            const settings::Setting &s = settings::kSettings[index];
            const bool allowed = settings::parentAllows(state.ini, s);
            const bool narrow = lower(state.ini.get("Video", "AspectRatio", "16:9")) == "4:3";
            const bool faded = !allowed || (s.wideOnly && narrow);
            const bool sub = s.parent != nullptr;
            ImGui::PushID(static_cast<int>(index));
            const std::string id = std::string("##") + s.id;
            const auto listOf = [&](const std::vector<std::string> &labels, int current, const Row &r) {
                if (r.pressed)
                    ImGui::OpenPopup("##list");
                return bookList(state, b, "##list", r, labels, current);
            };
            if (s.kind == settings::Kind::Toggle)
            {
                bool on = state.ini.getBool(s.section, s.key, std::strcmp(s.fallback, "1") == 0);
                const Row r = bookRow(state, b, id.c_str(), s.name, s.description, s.note, sub, faded);
                if (r.pressed)
                {
                    on = !on;
                    state.ini.setBool(s.section, s.key, on);
                }
                book::tickBox(ImGui::GetWindowDrawList(), ImVec2(r.max.x - 14.0f * b.u, r.mid), 22.0f * b.u, on, faded ? kBookFaded : kBookInk);
            }
            else if ((s.kind == settings::Kind::Choice && s.gameSlider) || s.kind == settings::Kind::Slider)
            {
                // a ruler: the choices' stops, or the slider's values
                const std::string value = settings::value(state.ini, s);
                const int stops = s.kind == settings::Kind::Slider ? s.valueCount : s.choiceCount;
                int stop = 0;
                if (s.kind == settings::Kind::Slider)
                {
                    const int number = std::atoi(value.c_str());
                    for (int i = 1; i < stops; ++i)
                        if (std::abs(s.values[i] - number) < std::abs(s.values[stop] - number))
                            stop = i;
                }
                else
                    stop = std::max(0, settings::choiceIndex(s, value));
                const auto valueAt = [&](int i) {
                    return s.kind == settings::Kind::Slider ? std::to_string(s.values[i]) : std::string(s.choices[i].value);
                };
                const auto labelAt = [&](int i) {
                    std::string text = s.kind == settings::Kind::Slider ? std::to_string(s.values[i])
                                                                        : std::string(s.choices[i].game ? s.choices[i].game : s.choices[i].value);
                    return text + s.unit;
                };
                const Row r = bookRow(state, b, id.c_str(), s.name, s.description, s.note, sub, faded, true);
                float x0 = 0.0f, x1 = 0.0f;
                bookRuler(state, b, r, stop, stops, labelAt(stop).c_str(), faded, x0, x1);
                int next = stop;
                if (!faded && r.focused)
                    next = std::clamp(stop + rulerStep(), 0, stops - 1);
                const ImGuiIO &io = ImGui::GetIO();
                if (!faded && r.active && io.MouseDown[0] && stops > 1 && io.MousePos.x >= x0 - 20.0f * b.u && io.MousePos.x <= x1 + 20.0f * b.u)
                    next = std::clamp(static_cast<int>(std::round((io.MousePos.x - x0) / (x1 - x0) * (stops - 1))), 0, stops - 1);
                if (next != stop)
                {
                    if (std::strcmp(s.key, "CameraPreset") == 0)
                        settings::writeCameraPreset(state.ini, valueAt(next));
                    else
                        state.ini.set(s.section, s.key, valueAt(next));
                }
            }
            else if (s.kind == settings::Kind::Choice)
            {
                std::string value = settings::value(state.ini, s);
                const int current = settings::choiceIndex(s, value);
                std::vector<std::string> labels;
                for (int i = 0; i < s.choiceCount; ++i)
                    labels.push_back(s.choices[i].label);
                const Row r = bookRow(state, b, id.c_str(), s.name, s.description, s.note, sub, faded);
                bookValue(state, b, r, current >= 0 ? s.choices[current].label : ("Custom: " + value).c_str(), faded);
                const int picked = listOf(labels, current, r);
                if (picked >= 0 && picked != current)
                {
                    if (std::strcmp(s.key, "CameraPreset") == 0)
                        settings::writeCameraPreset(state.ini, s.choices[picked].value);
                    else
                        state.ini.set(s.section, s.key, s.choices[picked].value);
                }
            }
            else if (std::strcmp(s.id, "DisplayMode") == 0)
            {
                const std::string mode = state.ini.getBool("Video", "StartFullscreen", true)    ? "fullscreen"
                                         : state.ini.getBool("Video", "StartMaximized", false) ? "maximized"
                                                                                               : "window";
                int current = 0;
                std::vector<std::string> labels;
                for (size_t i = 0; i < std::size(kWindowModes); ++i)
                {
                    labels.push_back(kWindowModes[i].label);
                    if (mode == kWindowModes[i].value)
                        current = static_cast<int>(i);
                }
                const Row r = bookRow(state, b, id.c_str(), s.name, s.description, s.note, sub, faded);
                bookValue(state, b, r, kWindowModes[current].label, faded);
                const int picked = listOf(labels, current, r);
                if (picked >= 0)
                {
                    state.ini.setBool("Video", "StartFullscreen", std::strcmp(kWindowModes[picked].value, "fullscreen") == 0);
                    state.ini.setBool("Video", "StartMaximized", std::strcmp(kWindowModes[picked].value, "maximized") == 0);
                }
            }
            else if (std::strcmp(s.id, "Character") == 0)
            {
                static std::vector<characters::Character> list;
                if (list.empty())
                    list = characters::list(state.context->exeDir);
                const std::string current = lower(state.ini.get("Mods", "Character", "gene"));
                int picked = 0;
                std::vector<std::string> names;
                for (size_t i = 0; i < list.size(); ++i)
                {
                    names.push_back(list[i].name);
                    if (lower(list[i].id) == current)
                        picked = static_cast<int>(i);
                }
                std::string help = s.description;
                if (!list.empty() && !list[static_cast<size_t>(picked)].credit.empty())
                    help += "\n" + list[static_cast<size_t>(picked)].credit;
                const Row r = bookRow(state, b, id.c_str(), s.name, help.c_str(), s.note, sub, faded);
                bookValue(state, b, r, names.empty() ? "Gene" : names[static_cast<size_t>(picked)].c_str(), faded);
                const int next = listOf(names, picked, r);
                if (next >= 0 && next < static_cast<int>(list.size()))
                    state.ini.set("Mods", "Character", list[static_cast<size_t>(next)].id);
            }
            else if (std::strcmp(s.id, "Costume") == 0)
            {
                const std::vector<characters::Costume> &costumes = costumesOf(state);
                const int picked = costumeIndex(state, costumes);
                std::vector<std::string> names;
                for (const characters::Costume &c : costumes)
                    names.push_back(c.name);
                const Row r = bookRow(state, b, id.c_str(), s.name, s.description, s.note, sub, faded);
                bookValue(state, b, r, names.empty() ? "" : names[static_cast<size_t>(picked)].c_str(), faded);
                const int next = listOf(names, picked, r);
                if (next >= 0 && next < static_cast<int>(costumes.size()))
                    state.ini.set("Mods", "Costume", costumes[static_cast<size_t>(next)].id);
            }
            else if (std::strcmp(s.id, "Textures") == 0)
            {
                bool on = state.ini.getBool("Runtime", "PS2X_PCSX2_TEXTURES", true);
                const Row r = bookRow(state, b, id.c_str(), s.name, s.description, s.note, sub, faded);
                if (r.pressed)
                {
                    on = !on;
                    state.ini.setBool("Runtime", "PS2X_PCSX2_TEXTURES", on);
                }
                book::tickBox(ImGui::GetWindowDrawList(), ImVec2(r.max.x - 14.0f * b.u, r.mid), 22.0f * b.u, on, kBookInk);
                const std::string folder = state.ini.get("Runtime", "PS2X_PCSX2_TEXTURE_DIR");
                const Row f = bookRow(state, b, "##folder", "Texture Pack Folder",
                                      "The folder that holds replacements/. Empty: textures/SLUS-21503 next to the game. "
                                      "Press to choose another.",
                                      nullptr, true, false);
                const std::string shown = folder.empty() ? std::string("Next to the game") : utf8(fromUtf8(folder).filename());
                bookValue(state, b, f, shown.c_str(), false);
                if (f.pressed)
                {
                    const fs::path pickedFolder = platform::pick(GetWindowHandle(), true);
                    if (!pickedFolder.empty())
                        state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", utf8(pickedFolder));
                }
                if (!folder.empty())
                {
                    const Row d = bookRow(state, b, "##default", "Use the Default Folder", "Back to textures/SLUS-21503 next to the game.",
                                          nullptr, true, false);
                    if (d.pressed)
                        state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", "");
                }
            }
            ImGui::PopID();
        }

        void bookModList(State &state, const Book &b, bool input); // the Mods tab's list (below)

        void bookSettings(State &state, const Book &b, settings::Tab tab)
        {
            const char *category = nullptr;
            for (size_t i = 0; i < settings::kSettingCount; ++i)
            {
                const settings::Setting &s = settings::kSettings[i];
                if (s.tab != tab)
                    continue;
                if (!settings::parentAllows(state.ini, s) && !s.greyed)
                    continue;
                if (std::strcmp(s.id, "Character") == 0 && !otherCharacters(state))
                    continue;
                if (std::strcmp(s.id, "Costume") == 0 && costumesOf(state).empty())
                    continue;
                if (!category || std::strcmp(category, s.category) != 0)
                {
                    bookCategory(state, b, s.category, category == nullptr);
                    category = s.category;
                }
                bookSetting(state, b, i);
            }
            if (tab == settings::Tab::Mods)
                bookModList(state, b, !state.optionsLeaving);
        }

        // ---- stamps and notes ----------------------------------------------------------------------------------------
        // A rubber stamp in red ink: a double border and the words (Holtwood One SC), turned a little; a button (a
        // keyboard / gamepad stop unless `nav` is off) or, with no id, just the mark.
        bool bookStamp(State &state, const Book &b, const char *id, const char *label, ImVec2 centre, float size, float degrees, bool nav = true)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u;
            const ImVec2 ts = textSize(state.bookFonts.stamp, size, label);
            const float hw = ts.x * 0.5f + 18.0f * u, hh = ts.y * 0.5f + 10.0f * u;
            bool pressed = false, focused = false;
            if (id != nullptr)
            {
                pressed = bookItem(state, id, ImVec2(centre.x - hw, centre.y - hh), ImVec2(centre.x + hw, centre.y + hh), nav);
                focused = nav && ImGui::IsItemFocused();
            }
            const float a = degrees * 3.14159265f / 180.0f, c = std::cos(a), s = std::sin(a);
            const auto turned = [&](float x, float y) { return ImVec2(centre.x + x * c - y * s, centre.y + x * s + y * c); };
            for (const float inset : {0.0f, 5.0f * u})
            {
                const ImVec2 box[5] = {turned(-hw + inset, -hh + inset), turned(hw - inset, -hh + inset), turned(hw - inset, hh - inset),
                                       turned(-hw + inset, hh - inset), turned(-hw + inset, -hh + inset - 1.0f * u)};
                book::line(dl, box, 5, (inset == 0.0f ? 2.6f : 1.2f) * u, kBookPencil);
            }
            const int start = dl->VtxBuffer.Size;
            bookText(dl, state.bookFonts.stamp, size, ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f + 1.0f * u), IM_COL32(164, 30, 22, 205), label);
            rotateSince(dl, start, centre, a);
            if (focused)
                bookLoop(state, b, dl, ImVec2(centre.x - hw - 4.0f * u, centre.y - hh - 4.0f * u), ImVec2(centre.x + hw + 4.0f * u, centre.y + hh + 4.0f * u));
            return pressed;
        }

        // A word on a note that is a button (the loop shows its focus).
        bool bookWord(State &state, const Book &b, const char *id, const char *word, ImVec2 at, bool first)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float size = 30.0f * b.u;
            const ImVec2 ts = textSize(state.bookFonts.text, size, word);
            const bool pressed = bookItem(state, id, ImVec2(at.x - 8.0f * b.u, at.y), ImVec2(at.x + ts.x + 8.0f * b.u, at.y + ts.y));
            ownSoundItem();
            if (first && ImGui::IsWindowAppearing())
            {
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                ImGui::SetNavCursorVisible(true);
            }
            bookText(dl, state.bookFonts.text, size, at, kBookInk, word);
            if (ImGui::IsItemFocused())
                bookLoop(state, b, dl, ImVec2(at.x, at.y + size * 0.12f), ImVec2(at.x + ts.x, at.y + size * 1.02f));
            return pressed;
        }

        // A paper note over the right page (the capture, the key clash, replacing the custom settings): a modal popup.
        bool bookNoteBegin(const Book &b, const char *id)
        {
            ImGui::SetNextWindowPos(ImVec2((b.rightMin.x + b.rightMax.x) * 0.5f, b.rightMin.y + (b.rightMax.y - b.rightMin.y) * 0.64f),
                                    ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            ImGui::PushStyleColor(ImGuiCol_PopupBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_Border, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleColor(ImGuiCol_ModalWindowDimBg, ImVec4(0, 0, 0, 0));
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(34.0f * b.u, 26.0f * b.u));
            ImGui::PushStyleVar(ImGuiStyleVar_PopupBorderSize, 0.0f);
            const bool open = ImGui::BeginPopupModal(id, nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar |
                                                                    ImGuiWindowFlags_NoScrollbar);
            ImGui::PopStyleVar(2);
            ImGui::PopStyleColor(3);
            if (!open)
                return false;
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const ImVec2 p = ImGui::GetWindowPos(), s = ImGui::GetWindowSize();
            // a note dropped on the page, a hair turned
            const float t = 0.012f * s.x;
            const ImVec2 quad[4] = {ImVec2(p.x, p.y + t), ImVec2(p.x + s.x, p.y), ImVec2(p.x + s.x, p.y + s.y - t), ImVec2(p.x, p.y + s.y)};
            book::paper(dl, quad, IM_COL32(255, 252, 244, 255), 10.0f * b.u);
            book::ink(dl);
            return true;
        }

        void bookNoteText(State &state, const Book &b, const char *text, bool dim = false, float width = 420.0f)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float size = 28.0f * b.u, wrap = width * b.u;
            const ImVec2 p = ImGui::GetCursorScreenPos();
            const ImVec2 ts = textSize(state.bookFonts.text, size, text, wrap);
            bookText(dl, state.bookFonts.text, size, p, dim ? IM_COL32(38, 26, 18, 160) : kBookInk, text, wrap);
            ImGui::Dummy(ImVec2(wrap, ts.y + 6.0f * b.u));
        }

        // The capture notes (a binding or a hotkey waiting for its key) and the key clash, as the panel has them.
        void bookNotes(State &state, const Book &b)
        {
            if (g_openCapture)
            {
                g_openCapture = false;
                ImGui::OpenPopup("##bookcapture");
            }
            if (bookNoteBegin(b, "##bookcapture"))
            {
                g_captureFrame = true;
                bookNoteText(state, b, rowName(g_capture.row).c_str());
                bookNoteText(state, b, g_capture.controller ? "Press a controller button or move a stick or trigger..." : "Press a key or click a mouse button...", true);
                const std::string left = "Esc cancels (" + std::to_string(std::max(0, 10 - static_cast<int>(GetTime() - g_capture.started))) + " s)";
                bookNoteText(state, b, left.c_str(), true);
                if (captureTick())
                {
                    g_capture.active = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (g_openHotkeyCapture)
            {
                g_openHotkeyCapture = false;
                ImGui::OpenPopup("##bookhotkey");
            }
            if (bookNoteBegin(b, "##bookhotkey"))
            {
                g_captureFrame = true;
                bookNoteText(state, b, hk::info(static_cast<hk::Action>(g_hotkeyCapture)).label);
                bookNoteText(state, b, "Press a key (Shift or Ctrl with it if you like)...", true);
                const std::string left = "Esc cancels (" + std::to_string(std::max(0, 10 - static_cast<int>(GetTime() - g_hotkeyStarted))) + " s)";
                bookNoteText(state, b, left.c_str(), true);
                if (hotkeyCaptureTick(state))
                {
                    g_hotkeyCapture = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            if (g_openClash)
            {
                g_openClash = false;
                ImGui::OpenPopup("##bookclash");
            }
            if (bookNoteBegin(b, "##bookclash"))
            {
                g_captureFrame = true; // its Esc / B must not also leave Options
                bookNoteText(state, b, g_clash.message.c_str());
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const bool cancel = bookWord(state, b, "##cancel", "Cancel", ImVec2(at.x + 20.0f * b.u, at.y + 8.0f * b.u), true);
                const bool assign = bookWord(state, b, "##assign", "Assign", ImVec2(at.x + 240.0f * b.u, at.y + 8.0f * b.u), false);
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + 54.0f * b.u));
                ImGui::Dummy(ImVec2(1.0f, 1.0f));
                bool close = false;
                if (assign)
                {
                    menuSound(state, kConfirm);
                    if (g_clash.game)
                        bindCell(g_clash.row, false, g_clash.column, g_clash.binding);
                    else
                    {
                        storeHotkey(state, g_clash.action, g_clash.key);
                        g_hotkeyStatus = std::string(hk::info(g_clash.action).label) + ": " + hk::text(g_clash.key);
                    }
                    close = true;
                }
                else if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                {
                    menuSound(state, kCancel);
                    (g_clash.game ? g_controlsStatus : g_hotkeyStatus) = "Not changed.";
                    close = true;
                }
                if (close)
                    ImGui::CloseCurrentPopup();
                ImGui::EndPopup();
            }
            if (state.presetOpenPopup)
            {
                state.presetOpenPopup = false;
                ImGui::OpenPopup("##bookreplace");
            }
            if (bookNoteBegin(b, "##bookreplace"))
            {
                g_captureFrame = true;
                bookNoteText(state, b, "Replace your custom settings?");
                if (state.presetAsk >= 0 && state.presetAsk < presets::kCount)
                {
                    const std::string line = std::string(presets::kText[state.presetAsk].name) + " sets them to its own.";
                    bookNoteText(state, b, line.c_str(), true);
                }
                const ImVec2 at = ImGui::GetCursorScreenPos();
                const bool keep = bookWord(state, b, "##keep", "No, keep mine", ImVec2(at.x + 10.0f * b.u, at.y + 8.0f * b.u), true);
                const bool replace = bookWord(state, b, "##replace", "Yes", ImVec2(at.x + 300.0f * b.u, at.y + 8.0f * b.u), false);
                ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + 54.0f * b.u));
                ImGui::Dummy(ImVec2(1.0f, 1.0f));
                if (replace && state.presetAsk >= 0 && state.presetAsk < presets::kCount)
                {
                    menuSound(state, kConfirm);
                    usePreset(state, state.presetAsk);
                    state.presetAsk = -1;
                    ImGui::CloseCurrentPopup();
                }
                else if (keep || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                {
                    menuSound(state, kCancel);
                    state.presetAsk = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // ---- the Mods tab's list of mods (claude/mod-support.md, step 1) ---------------------------------------------
        // The mods under mods/ in load order (mod_list.h), scanned when the tab shows and again when a folder comes or
        // goes. A tick, a move or a mod seen for the first time goes into GHR.ini [Mods] LoadOrder / Disabled at once
        // (saved with the rest when Options closes); a new mod stays marked NEW until the launcher closes.
        struct ModJob // Install from zip, on a thread of its own (the note shows its progress)
        {
            mod_zip::Plan plan;
            mod_zip::Progress progress;
            std::atomic<bool> finished{false};
            bool ok = false;
            std::string error;
        };
        struct ModsPage
        {
            bool scanned = false;
            std::vector<mod_list::Mod> mods;
            std::vector<mod_list::Overrides> overrides;
            std::vector<std::string> folders; // the folder names at the last scan
            std::vector<std::string> fresh;   // folders (lower case) new this session: marked NEW
            double nextCheck = 0.0;
            int focused = -1;                 // the mod row with the focus this frame (-1: none)
            std::string scrollTo;             // a mod just moved: kept in view
            enum class Note
            {
                None,
                Problem,
                Replace,
                Working,
                Done
            } note = Note::None;
            bool openNote = false;
            std::string noteText;
            mod_zip::Plan plan;
            std::shared_ptr<ModJob> job;
        };
        ModsPage g_modsPage;

        fs::path modsFolder(const State &state) { return state.context->exeDir / "mods"; }

        bool isFresh(const mod_list::Mod &m)
        {
            const std::vector<std::string> &f = g_modsPage.fresh;
            return std::find(f.begin(), f.end(), lower(m.folder)) != f.end();
        }

        // The list as it stands -> GHR.ini, and what the mods take from each other.
        void storeModList(State &state)
        {
            ModsPage &p = g_modsPage;
            const std::string order = mod_list::orderText(p.mods), off = mod_list::disabledText(p.mods);
            if (state.ini.get("Mods", "LoadOrder") != order)
                state.ini.set("Mods", "LoadOrder", order);
            if (state.ini.get("Mods", "Disabled") != off)
                state.ini.set("Mods", "Disabled", off);
            p.overrides = mod_list::overrides(p.mods);
        }

        void scanMods(State &state, bool force)
        {
            ModsPage &p = g_modsPage;
            const double now = GetTime();
            if (p.scanned && !force)
            {
                if (now < p.nextCheck)
                    return;
                p.nextCheck = now + 1.5;
                if (mod_list::folderNames(modsFolder(state)) == p.folders)
                    return;
            }
            p.scanned = true;
            p.nextCheck = now + 1.5;
            p.folders = mod_list::folderNames(modsFolder(state));
            p.mods = mod_list::scan(modsFolder(state), state.ini.get("Mods", "LoadOrder"), state.ini.get("Mods", "Disabled"), true);
            for (const mod_list::Mod &m : p.mods)
                if (m.isNew && !isFresh(m))
                    p.fresh.push_back(lower(m.folder));
            storeModList(state); // the new ones are seen now: listed (on, at the bottom)
        }

        void moveMod(State &state, size_t index, int by)
        {
            ModsPage &p = g_modsPage;
            const long to = static_cast<long>(index) + by;
            if (to < 0 || to >= static_cast<long>(p.mods.size()))
                return;
            std::swap(p.mods[index], p.mods[static_cast<size_t>(to)]);
            p.scrollTo = p.mods[static_cast<size_t>(to)].folder;
            storeModList(state);
            menuSound(state, kTick);
        }

        std::string countOf(size_t n, const char *what)
        {
            return std::to_string(n) + " " + what + (n == 1 ? "" : "s");
        }

        std::string joinWords(const std::vector<std::string> &parts)
        {
            std::string out;
            for (size_t i = 0; i < parts.size(); ++i)
                out += (i == 0 ? "" : i + 1 == parts.size() ? " and " : ", ") + parts[i];
            return out;
        }

        // "pl/pl00.dat and 2 textures"
        std::string conflictText(const mod_list::Conflict &c)
        {
            std::vector<std::string> parts;
            if (c.files.size() <= 2)
                parts.insert(parts.end(), c.files.begin(), c.files.end());
            else
                parts.push_back(countOf(c.files.size(), "game file"));
            if (c.textures != 0)
                parts.push_back(countOf(c.textures, "texture"));
            return joinWords(parts);
        }

        // The right page for a mod: who made it, what it is, what it holds, what it wins and loses to the others.
        std::string modHelp(const State &state, size_t i)
        {
            const ModsPage &p = g_modsPage;
            const mod_list::Mod &m = p.mods[i];
            std::string text;
            if (!m.author.empty() && !m.version.empty())
                text = "By " + m.author + ", version " + m.version + ".";
            else if (!m.author.empty())
                text = "By " + m.author + ".";
            else if (!m.version.empty())
                text = "Version " + m.version + ".";
            if (!m.description.empty())
                text += (text.empty() ? "" : "\n") + m.description;
            std::vector<std::string> parts;
            const size_t entries = static_cast<size_t>(
                std::count_if(m.files.begin(), m.files.end(), [](const std::string &f) { return !mod_list::archiveOf(f).empty(); }));
            if (m.files.size() > entries)
                parts.push_back(countOf(m.files.size() - entries, "game file"));
            if (entries != 0)
                parts.push_back(entries == 1 ? std::string("1 archive entry") : std::to_string(entries) + " archive entries");
            if (m.textureCount != 0)
                parts.push_back(countOf(m.textureCount, "texture"));
            if (!m.patches.empty())
                parts.push_back(countOf(m.patches.size(), "patch file"));
            text += std::string(text.empty() ? "" : "\n\n") +
                    (parts.empty() ? "No game files, textures or patches in it." : "It replaces " + joinWords(parts) + ".");
            if (m.hasCharacters)
                text += " Its characters/ come in a later version of GHR.";
            if (i < p.overrides.size())
            {
                const mod_list::Overrides &o = p.overrides[i];
                int lines = 0, more = 0;
                for (const mod_list::Conflict &c : o.wins)
                {
                    if (lines++ < 2)
                        text += "\nWins over " + p.mods[c.other].name + ": " + conflictText(c) + ".";
                    else
                        ++more;
                }
                for (const mod_list::Conflict &c : o.loses)
                {
                    if (lines++ < 3)
                        text += "\nLoses to " + p.mods[c.other].name + ", lower in the list: " + conflictText(c) + ".";
                    else
                        ++more;
                }
                if (more != 0)
                    text += "\n... and " + countOf(static_cast<size_t>(more), "more mod") + ".";
            }
            if (!m.enabled)
                text += "\n\nOff: the game leaves it out.";
            else if (isFresh(m))
                text += "\n\nNew: GHR put it on, at the bottom of the list.";
            if (state.lastDevice == 0)
                text += "\nThe marks by its number move it up or down.";
            return text;
        }

        // A mod's row: its place (the number; the up / down marks, for the mouse), its name (NEW in red pencil, a note
        // when it wins or loses files to another mod), its tick (a press of the row turns it on or off). The focused row
        // moves with LT / RT or Z / C (the Controls tab's sub-tab keys; free here).
        void bookModRow(State &state, const Book &b, size_t i, bool input, long &moveFrom, int &moveBy)
        {
            ModsPage &p = g_modsPage;
            const mod_list::Mod &m = p.mods[i];
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u, w = ImGui::GetContentRegionAvail().x;
            const ImVec2 min = ImGui::GetCursorScreenPos(), max(min.x + w, min.y + kRowH * u);
            const float mid = min.y + kRowH * u * 0.5f, size = 26.0f * u;
            const float numberRight = min.x + 30.0f * u, upX = min.x + 50.0f * u, downX = min.x + 76.0f * u, nameX = min.x + 98.0f * u;
            ImGui::PushID(m.folder.c_str());
            const std::string number = std::to_string(i + 1);
            const float ns = 22.0f * u;
            bookText(dl, state.bookFonts.text, ns, ImVec2(numberRight - textSize(state.bookFonts.text, ns, number.c_str()).x, mid - ns * 0.56f),
                     kBookFaded, number.c_str());
            const auto mark = [&](const char *id, float x, bool up, bool usable) {
                bool pressed = false, hovered = false;
                if (usable)
                {
                    pressed = bookItem(state, id, ImVec2(x - 12.0f * u, min.y + 2.0f * u), ImVec2(x + 12.0f * u, max.y - 2.0f * u), false);
                    hovered = ImGui::IsItemHovered();
                }
                const float h = 5.5f * u, half = 7.5f * u;
                const float tip = up ? mid - h : mid + h, base = up ? mid + h : mid - h;
                const ImVec2 caret[3] = {ImVec2(x - half, base), ImVec2(x, tip), ImVec2(x + half, base)};
                book::line(dl, caret, 3, (hovered ? 2.4f : 1.7f) * u, !usable ? IM_COL32(38, 26, 18, 40) : hovered ? kBookPencil : kBookFaded);
                return pressed && input;
            };
            if (mark("##up", upX, true, i > 0))
            {
                moveFrom = static_cast<long>(i);
                moveBy = -1;
            }
            if (mark("##down", downX, false, i + 1 < p.mods.size()))
            {
                moveFrom = static_cast<long>(i);
                moveBy = 1;
            }
            const bool pressed = bookItem(state, "##mod", ImVec2(nameX - 8.0f * u, min.y), max);
            tickItem();
            const bool focused = ImGui::IsItemFocused();
            if (p.scrollTo == m.folder)
            {
                ImGui::ScrollToItem(ImGuiScrollFlags_KeepVisibleEdgeY);
                p.scrollTo.clear();
            }
            if (pressed && input)
            {
                p.mods[i].enabled = !p.mods[i].enabled;
                storeModList(state);
            }
            // the name, shrunk to leave room for its notes and the tick
            const bool fresh = isFresh(m);
            std::string tag;
            if (i < p.overrides.size() && m.enabled)
            {
                const mod_list::Overrides &o = p.overrides[i];
                if (!o.wins.empty())
                    tag = o.wins.size() == 1 ? "overrides " + p.mods[o.wins[0].other].name : "overrides " + countOf(o.wins.size(), "mod");
                else if (!o.loses.empty())
                    tag = "partly overridden";
            }
            const float tagSize = 19.0f * u, gap = 10.0f * u;
            const float newW = fresh ? textSize(state.bookFonts.text, tagSize, "new").x + gap : 0.0f;
            float tagW = tag.empty() ? 0.0f : textSize(state.bookFonts.text, tagSize, tag.c_str()).x + gap;
            const float room = max.x - 40.0f * u - nameX;
            if (tagW > room * 0.45f)
            {
                tag = !p.overrides[i].wins.empty() ? "overrides " + countOf(p.overrides[i].wins.size(), "mod") : tag;
                tagW = textSize(state.bookFonts.text, tagSize, tag.c_str()).x + gap;
            }
            const float full = textSize(state.bookFonts.text, size, m.name.c_str()).x;
            const float nameRoom = std::max(room * 0.4f, room - newW - tagW);
            const float nameSize = size * (full > nameRoom ? std::max(0.62f, nameRoom / full) : 1.0f);
            const ImVec2 at(nameX, mid - nameSize * 0.56f);
            bookText(dl, state.bookFonts.text, nameSize, at, m.enabled ? kBookInk : kBookFaded, m.name.c_str());
            const ImVec2 ts = textSize(state.bookFonts.text, nameSize, m.name.c_str());
            float x = at.x + std::min(ts.x, nameRoom) + gap;
            if (fresh)
            {
                bookText(dl, state.bookFonts.text, tagSize, ImVec2(x, mid - tagSize * 0.62f), kBookPencil, "new");
                x += newW;
            }
            if (!tag.empty())
                bookText(dl, state.bookFonts.text, tagSize, ImVec2(x, mid - tagSize * 0.62f), m.enabled ? kBookFaded : kBookPencilFaded,
                         tag.c_str());
            book::tickBox(dl, ImVec2(max.x - 14.0f * u, mid), 22.0f * u, m.enabled, kBookInk);
            if (focused)
            {
                p.focused = static_cast<int>(i);
                bookLoop(state, b, dl, ImVec2(at.x, at.y + nameSize * 0.12f), ImVec2(at.x + ts.x, at.y + nameSize * 1.02f));
                bookHelp(state, m.name.c_str(), modHelp(state, i).c_str());
                if (input && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId))
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false) || ImGui::IsKeyPressed(ImGuiKey_Z, false))
                        moveFrom = static_cast<long>(i), moveBy = -1;
                    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false) || ImGui::IsKeyPressed(ImGuiKey_C, false))
                        moveFrom = static_cast<long>(i), moveBy = 1;
                }
            }
            ImGui::PopID();
            ImGui::SetCursorScreenPos(ImVec2(min.x, max.y));
            ImGui::Dummy(ImVec2(w, 0.0f));
        }

        // The Mods tab's MODS heading and list (after Character and Texture Packs).
        void bookModList(State &state, const Book &b, bool input)
        {
            ModsPage &p = g_modsPage;
            scanMods(state, false);
            p.focused = -1;
            bookCategory(state, b, "Mods", false);
            if (p.mods.empty())
            {
                ImDrawList *dl = ImGui::GetWindowDrawList();
                const float u = b.u, w = ImGui::GetContentRegionAvail().x, size = 24.0f * u;
                const char *text = "None yet. A mod is a folder in mods next to GHR.exe: install one from its zip or open the "
                                   "folder (on the right).";
                const ImVec2 at = ImGui::GetCursorScreenPos();
                bookText(dl, state.bookFonts.text, size, ImVec2(at.x, at.y + 6.0f * u), kBookFaded, text, w);
                ImGui::Dummy(ImVec2(w, textSize(state.bookFonts.text, size, text, w).y + 12.0f * u));
                return;
            }
            long moveFrom = -1;
            int moveBy = 0;
            for (size_t i = 0; i < p.mods.size(); ++i)
                bookModRow(state, b, i, input, moveFrom, moveBy);
            if (moveFrom >= 0)
                moveMod(state, static_cast<size_t>(moveFrom), moveBy);
        }

        // ---- Install from zip (the Mods tab's stamp) ------------------------------------------------------------------
        void startModInstall(State &state)
        {
            ModsPage &p = g_modsPage;
            auto job = std::make_shared<ModJob>();
            job->plan = p.plan;
            const fs::path folder = modsFolder(state);
            p.job = job;
            p.note = ModsPage::Note::Working;
            std::thread([job, folder] {
                job->ok = mod_zip::install(job->plan, folder, job->error, &job->progress);
                job->finished = true;
            }).detach();
        }

        std::string modNames(const mod_zip::Plan &plan)
        {
            std::vector<std::string> names;
            for (const mod_zip::Found &m : plan.mods)
                names.push_back(m.name + (m.version.empty() ? "" : " " + m.version));
            return joinWords(names);
        }

        void installModZip(State &state)
        {
            ModsPage &p = g_modsPage;
            const fs::path zip = platform::pickZip(GetWindowHandle());
            if (zip.empty())
                return;
            p.plan = mod_zip::inspect(zip, modsFolder(state));
            p.openNote = true;
            if (!p.plan.error.empty())
            {
                p.note = ModsPage::Note::Problem;
                p.noteText = utf8(zip.filename()) + " " + p.plan.error + ".";
                KM_WARN(Boot, "launcher: install from zip: " << utf8(zip) << " " << p.plan.error);
                return;
            }
            std::vector<std::string> replaced;
            bool same = true;
            for (const mod_zip::Found &m : p.plan.mods)
                if (m.exists)
                {
                    replaced.push_back(m.name + (m.oldVersion.empty() ? "" : " " + m.oldVersion));
                    same = same && m.oldVersion == m.version;
                }
            if (!replaced.empty())
            {
                p.note = ModsPage::Note::Replace;
                p.noteText = same ? "You have " + joinWords(replaced) + " already. Put it in again? It keeps its place in the list."
                                  : "Replace the " + joinWords(replaced) + " you have with " + modNames(p.plan) +
                                        "? It keeps its place in the list.";
                return;
            }
            startModInstall(state);
        }

        // The zip's note: a problem, the replace question, the progress, the result.
        void bookModNote(State &state, const Book &b)
        {
            ModsPage &p = g_modsPage;
            if (p.openNote)
            {
                p.openNote = false;
                ImGui::OpenPopup("##bookmods");
            }
            if (!bookNoteBegin(b, "##bookmods"))
                return;
            g_captureFrame = true; // its Esc / B must not also leave Options
            if (p.note == ModsPage::Note::Working && p.job && p.job->finished)
            {
                const std::string names = modNames(p.job->plan);
                if (p.job->ok)
                {
                    p.note = ModsPage::Note::Done;
                    p.noteText = "Installed " + names + ".";
                    KM_INFO(Boot, "launcher: installed " << names << " from " << utf8(p.job->plan.zip));
                }
                else
                {
                    p.note = ModsPage::Note::Problem;
                    p.noteText = "Not installed: " + p.job->error + ".";
                    KM_WARN(Boot, "launcher: install from " << utf8(p.job->plan.zip) << " failed: " << p.job->error);
                }
                p.job.reset();
                scanMods(state, true);
            }
            if (p.note == ModsPage::Note::Working)
            {
                const uint64_t total = p.job ? p.job->progress.total.load() : 0u, done = p.job ? p.job->progress.done.load() : 0u;
                const std::string text = "Installing " + modNames(p.plan) + "... " +
                                         std::to_string(total ? static_cast<int>(done * 100u / total) : 0) + "%";
                bookNoteText(state, b, text.c_str());
                ImGui::EndPopup();
                return;
            }
            bookNoteText(state, b, p.noteText.c_str());
            const ImVec2 at = ImGui::GetCursorScreenPos();
            bool close = false;
            if (p.note == ModsPage::Note::Replace)
            {
                const bool cancel = bookWord(state, b, "##cancel", "Cancel", ImVec2(at.x + 20.0f * b.u, at.y + 8.0f * b.u), true);
                const bool replace = bookWord(state, b, "##replace", "Replace", ImVec2(at.x + 240.0f * b.u, at.y + 8.0f * b.u), false);
                if (replace)
                {
                    menuSound(state, kConfirm);
                    startModInstall(state);
                }
                else if (cancel || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                {
                    menuSound(state, kCancel);
                    close = true;
                }
            }
            else
            {
                const bool ok = bookWord(state, b, "##ok", "OK", ImVec2(at.x + 20.0f * b.u, at.y + 8.0f * b.u), true);
                if (ok || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
                {
                    menuSound(state, ok ? kConfirm : kCancel);
                    close = true;
                }
            }
            ImGui::SetCursorScreenPos(ImVec2(at.x, at.y + 54.0f * b.u));
            ImGui::Dummy(ImVec2(1.0f, 1.0f));
            if (close)
            {
                p.note = ModsPage::Note::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // The right page's stamps on the Mods tab, and for a focused mod the keys that move it.
        void bookModsRight(State &state, const Book &b, bool input)
        {
            ModsPage &p = g_modsPage;
            const float u = b.u;
            if (p.focused >= 0 && state.lastDevice != 0 && !state.bookHelp.empty())
            {
                // under the help text: "Move it" and the sub-tab prompts (LT / RT, Z / C)
                const float w = b.rightMax.x - b.rightMin.x, hs = 38.0f * u;
                const float th = textSize(state.bookFonts.heading, hs, state.bookTitle.c_str(), w).y;
                const float y = b.rightMin.y + th + 50.0f * u + textSize(state.bookFonts.text, state.bookHelpSize * u, state.bookHelp.c_str(), w).y + 18.0f * u;
                const float size = 26.0f * u;
                const float upW = textSize(state.bookFonts.text, size, "Up").x, downAt = b.rightMin.x + upW + 76.0f * u;
                const float downW = textSize(state.bookFonts.text, size, "Down").x;
                bookText(b.dl, state.bookFonts.text, size, ImVec2(b.rightMin.x, y), kBookInk, "Up");
                bookText(b.dl, state.bookFonts.text, size, ImVec2(downAt, y), kBookInk, "Down");
                book::paint(b.dl); // the prompts in their own colours (as the index tabs' and the slips')
                bookPrompt(b.dl, state.prompt[2], ImVec2(b.rightMin.x + upW + 32.0f * u, y + size * 0.55f), 38.0f * u);
                bookPrompt(b.dl, state.prompt[3], ImVec2(downAt + downW + 32.0f * u, y + size * 0.55f), 38.0f * u);
                book::ink(b.dl);
            }
            if (!input)
                return;
            const ImVec2 at((b.rightMin.x + b.rightMax.x) * 0.5f, b.rightMax.y - 130.0f * u);
            if (bookStamp(state, b, "##zip", "INSTALL FROM ZIP", at, 20.0f * u, -1.5f))
                installModZip(state);
            if (bookStamp(state, b, "##modsfolder", "OPEN MODS FOLDER", ImVec2(at.x, at.y + 70.0f * u), 20.0f * u, 1.0f))
            {
                std::error_code error;
                fs::create_directories(modsFolder(state), error);
                platform::open(modsFolder(state));
            }
        }

        // The plain Options panel's version (when the saloon can't load): a box, Up / Down per mod; the two buttons.
        void drawModList(State &state)
        {
            ModsPage &p = g_modsPage;
            scanMods(state, false);
            if (!beginCategory("Mods"))
                return;
            long moveFrom = -1;
            int moveBy = 0;
            for (size_t i = 0; i < p.mods.size(); ++i)
            {
                mod_list::Mod &m = p.mods[i];
                ImGui::PushID(m.folder.c_str());
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::TextUnformatted((std::to_string(i + 1) + ". " + m.name + (isFresh(m) ? "  (new)" : "")).c_str());
                if (ImGui::IsItemHovered() && !m.description.empty())
                    ImGui::SetTooltip("%s", m.description.c_str());
                ImGui::TableNextColumn();
                bool on = m.enabled;
                if (ImGui::Checkbox("On", &on))
                {
                    m.enabled = on;
                    storeModList(state);
                }
                tickItem();
                ImGui::SameLine();
                ImGui::BeginDisabled(i == 0);
                if (ImGui::SmallButton("Up"))
                    moveFrom = static_cast<long>(i), moveBy = -1;
                ImGui::EndDisabled();
                ImGui::SameLine();
                ImGui::BeginDisabled(i + 1 == p.mods.size());
                if (ImGui::SmallButton("Down"))
                    moveFrom = static_cast<long>(i), moveBy = 1;
                ImGui::EndDisabled();
                ImGui::PopID();
            }
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (ImGui::Button("Install from zip..."))
            {
                const fs::path zip = platform::pickZip(GetWindowHandle());
                if (!zip.empty())
                {
                    p.plan = mod_zip::inspect(zip, modsFolder(state));
                    std::string error = p.plan.error;
                    if (error.empty() && !mod_zip::install(p.plan, modsFolder(state), error))
                        p.noteText = "Not installed: " + error + ".";
                    else
                        p.noteText = error.empty() ? "Installed " + modNames(p.plan) + "." : utf8(zip.filename()) + " " + error + ".";
                    scanMods(state, true);
                }
            }
            ImGui::SameLine();
            if (ImGui::Button("Open mods folder"))
            {
                std::error_code error;
                fs::create_directories(modsFolder(state), error);
                platform::open(modsFolder(state));
            }
            if (!p.noteText.empty())
                ImGui::TextWrapped("%s", p.noteText.c_str());
            ImGui::EndTable();
            if (moveFrom >= 0)
                moveMod(state, static_cast<size_t>(moveFrom), moveBy);
        }

        // ---- the pages ---------------------------------------------------------------------------------------------
        void bookPresetsLeft(State &state, const Book &b, int now, int &focusedPreset)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u, w = ImGui::GetContentRegionAvail().x;
            if (now == presets::kCustom)
            {
                const ImVec2 p = ImGui::GetCursorScreenPos();
                bookText(dl, state.bookFonts.heading, 36.0f * u, ImVec2(p.x, p.y + 12.0f * u), kBookInk, presets::kCustomText.name);
                const std::string line = std::string(presets::kCustomText.tagline) + " " + presets::kCustomText.band;
                bookText(dl, state.bookFonts.text, 26.0f * u, ImVec2(p.x, p.y + 58.0f * u), kBookInk, line.c_str(), w);
                bookStamp(state, b, nullptr, "IN USE", ImVec2(p.x + w - 74.0f * u, p.y + 30.0f * u), 22.0f * u, 7.0f);
                ImGui::Dummy(ImVec2(w, 120.0f * u));
                bookRule(b, dl, p.x, p.x + w, p.y + 112.0f * u, IM_COL32(38, 26, 18, 90), 1.0f * u, 99u);
            }
            for (int i = 0; i < presets::kCount; ++i)
            {
                const presets::Text &t = presets::kText[i];
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float tagH = textSize(state.bookFonts.text, 26.0f * u, t.tagline, w - 20.0f * u).y;
                const float rowH = std::max(140.0f * u, 76.0f * u + tagH + 18.0f * u);
                ImGui::PushID(i);
                const bool pressed = bookItem(state, "##preset", p, ImVec2(p.x + w, p.y + rowH));
                ImGui::PopID();
                const float nameSize = 36.0f * u;
                const ImVec2 at(p.x, p.y + 24.0f * u);
                bookText(dl, state.bookFonts.heading, nameSize, at, kBookInk, t.name);
                bookText(dl, state.bookFonts.text, 26.0f * u, ImVec2(p.x, p.y + 76.0f * u), kBookInk, t.tagline, w - 20.0f * u);
                if (i == now)
                    bookStamp(state, b, nullptr, "IN USE", ImVec2(p.x + w - 74.0f * u, p.y + 44.0f * u), 22.0f * u, 7.0f);
                if (ImGui::IsItemFocused())
                {
                    const ImVec2 ts = textSize(state.bookFonts.heading, nameSize, t.name);
                    bookLoop(state, b, dl, ImVec2(at.x, at.y + nameSize * 0.08f), ImVec2(at.x + ts.x, at.y + nameSize * 1.0f));
                    focusedPreset = i;
                    bookHelp(state, t.name, t.band);
                }
                if (pressed && i != now)
                {
                    if (now == presets::kCustom)
                    {
                        state.presetAsk = i;
                        state.presetOpenPopup = true;
                    }
                    else
                        usePreset(state, i);
                }
                if (i + 1 < presets::kCount)
                    bookRule(b, dl, p.x, p.x + w, p.y + rowH, IM_COL32(38, 26, 18, 90), 1.0f * u, static_cast<uint32_t>(i));
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + rowH + 10.0f * u));
                ImGui::Dummy(ImVec2(w, 0.0f));
            }
        }

        void bookPresetsRight(State &state, const Book &b, int now, int shown)
        {
            ImDrawList *dl = b.dl;
            const float u = b.u;
            const ImVec2 m = b.rightMin;
            const float w = b.rightMax.x - b.rightMin.x;
            // the photo (the pause menu's, from the player's disc) in black photo corners
            const float photoW = w - 40.0f * u, photoH = photoW / presets::kPhotoAspect;
            const ImVec2 p0(m.x + 20.0f * u, m.y + 6.0f * u), p1(p0.x + photoW, p0.y + photoH);
            book::paint(dl);
            if (shown >= 0 && shown < presets::kCount && state.presetPhotos[shown].id != 0u)
                bookImage(dl, state.presetPhotos[shown], p0, p1);
            else
                dl->AddRectFilled(p0, p1, ImGui::GetColorU32(IM_COL32(92, 86, 80, 255)));
            const float c = 46.0f * u, o = 8.0f * u;
            const ImU32 black = ImGui::GetColorU32(IM_COL32(28, 24, 22, 245));
            dl->AddTriangleFilled(ImVec2(p0.x - o, p0.y - o), ImVec2(p0.x + c, p0.y - o), ImVec2(p0.x - o, p0.y + c), black);
            dl->AddTriangleFilled(ImVec2(p1.x + o, p0.y - o), ImVec2(p1.x + o, p0.y + c), ImVec2(p1.x - c, p0.y - o), black);
            dl->AddTriangleFilled(ImVec2(p1.x + o, p1.y + o), ImVec2(p1.x - c, p1.y + o), ImVec2(p1.x + o, p1.y - c), black);
            dl->AddTriangleFilled(ImVec2(p0.x - o, p1.y + o), ImVec2(p0.x - o, p1.y - c), ImVec2(p0.x + c, p1.y + o), black);
            book::ink(dl);
            if (shown < 0 || shown >= presets::kCount)
                return;
            const presets::Text &t = presets::kText[shown];
            float y = p1.y + 44.0f * u;
            bookText(dl, state.bookFonts.text, 34.0f * u, ImVec2(m.x, y), kBookInk, t.tagline, w); // (as on the posters)
            y += textSize(state.bookFonts.text, 34.0f * u, t.tagline, w).y + 8.0f * u;
            bookRule(b, dl, m.x, m.x + w, y, kBookInk, 1.6f * u, 9u);
            bookText(dl, state.bookFonts.text, 28.0f * u, ImVec2(m.x, y + 26.0f * u), kBookInk, t.band, w);
            const ImVec2 stampAt((b.rightMin.x + b.rightMax.x) * 0.5f, b.rightMax.y - 70.0f * u);
            if (shown == now)
                bookStamp(state, b, nullptr, "IN USE", stampAt, 30.0f * u, -2.0f);
            else if (bookStamp(state, b, "##use", "USE THIS", stampAt, 30.0f * u, -2.0f, false))
            {
                if (now == presets::kCustom)
                {
                    state.presetAsk = shown;
                    state.presetOpenPopup = true;
                }
                else
                {
                    menuSound(state, kConfirm);
                    usePreset(state, shown);
                }
            }
        }

        // A binding's pictures from the pack (K, Nov 18: the Controls pages show the prompts, not names): one file per
        // element of the chord (resources/prompts/xelu/<set>, the sticks' side views from resources/options/mapping as
        // in game), "" where the pack has none (that element is written out); a stick's direction (1 up, 2 down,
        // 3 left, 4 right) is inked beside its picture.
        std::string padFolder(const State &state); // (the prompts' art set: below)
        struct ArtPiece
        {
            std::string file; // relative to the exe's folder
            int arrow = 0;
        };
        std::vector<ArtPiece> bindingArt(const State &state, const std::string &binding)
        {
            std::vector<ArtPiece> pieces;
            const std::string set = padFolder(state);
            struct SetNames
            {
                const char *folder, *prefix, *face[4], *shoulder[2], *trigger[2], *start, *back;
            };
            static const SetNames kSets[] = {
                {"xbox360", "360_", {"A", "B", "X", "Y"}, {"LB", "RB"}, {"LT", "RT"}, "Start_Alt", "Back_Alt"},
                {"ps3", "PS3_", {"Cross", "Circle", "Square", "Triangle"}, {"L1", "R1"}, {"L2", "R2"}, "Start", "Select"},
                {"switch", "Switch_", {"B", "A", "Y", "X"}, {"LB", "RB"}, {"LT", "RT"}, "Plus", "Minus"}, // by place
                {"steamdeck", "SteamDeck_", {"A", "B", "X", "Y"}, {"L1", "R1"}, {"L2", "R2"}, "Menu", "Inventory"},
            };
            const SetNames *names = &kSets[0];
            for (const SetNames &s : kSets)
                if (set == s.folder)
                    names = &s;
            const auto padFile = [&](const std::string &name) { return std::string("resources/prompts/xelu/") + names->folder + "/" + names->prefix + name + ".png"; };
            const auto keyFile = [](const std::string &name) { return "resources/prompts/xelu/keyboard/" + name + "_Key_Light.png"; };
            size_t start = 0;
            while (start <= binding.size())
            {
                size_t end = binding.find('&', start);
                if (end == std::string::npos)
                    end = binding.size();
                std::string element = binding.substr(start, end - start);
                element.erase(0, element.find_first_not_of(' '));
                element.erase(element.find_last_not_of(' ') + 1);
                start = end + 1;
                if (element.empty())
                    continue;
                ArtPiece piece;
                const size_t slash = element.find('/');
                const std::string device = element.substr(0, slash), name = slash == std::string::npos ? std::string() : element.substr(slash + 1);
                if (device.rfind("SDL-", 0) == 0)
                {
                    static const char *const kFaces[] = {"FaceSouth", "FaceEast", "FaceWest", "FaceNorth"};
                    for (int i = 0; i < 4; ++i)
                        if (name == kFaces[i])
                            piece.file = padFile(names->face[i]);
                    if (name == "LeftShoulder" || name == "RightShoulder")
                        piece.file = padFile(names->shoulder[name[0] == 'R']);
                    else if (name == "+LeftTrigger" || name == "+RightTrigger")
                        piece.file = padFile(names->trigger[name[1] == 'R']);
                    else if (name == "LeftStick" || name == "RightStick")
                        piece.file = padFile(name == "LeftStick" ? "Left_Stick_Click" : "Right_Stick_Click");
                    else if (name == "Start")
                        piece.file = padFile(names->start);
                    else if (name == "Back")
                        piece.file = padFile(names->back);
                    else if (name.rfind("DPad", 0) == 0)
                        piece.file = padFile("Dpad_" + name.substr(4));
                    else if (name.size() == 6 && (name.compare(1, 4, "Left") == 0 || name.compare(1, 4, "Righ") == 0))
                        ; // (not a stick axis: below)
                    if (piece.file.empty() && name.size() >= 6 && (name[0] == '+' || name[0] == '-') &&
                        (name.compare(1, 5, "LeftX") == 0 || name.compare(1, 5, "LeftY") == 0 || name.compare(1, 6, "RightX") == 0 ||
                         name.compare(1, 6, "RightY") == 0))
                    {
                        const bool left = name[1] == 'L', vertical = name.back() == 'Y', negative = name[0] == '-';
                        piece.file = std::string("resources/options/mapping/stick_") + names->folder + (left ? "_left.png" : "_right.png");
                        piece.arrow = vertical ? (negative ? 1 : 2) : (negative ? 3 : 4);
                    }
                    static const char *const kPaddles[] = {"Paddle1", "Paddle2", "Paddle3", "Paddle4"};
                    static const char *const kPaddleFiles[] = {"paddle_r4", "paddle_l4", "paddle_r5", "paddle_l5"}; // SDL: upper right first
                    for (int i = 0; i < 4; ++i)
                        if (name == kPaddles[i])
                            piece.file = std::string("resources/options/mapping/") + kPaddleFiles[i] + ".png";
                }
                else if (device == "Keyboard")
                {
                    static const std::pair<const char *, const char *> kKeys[] = {
                        {"Space", "Space"}, {"Return", "Enter"}, {"Keypad+Enter", "Enter"}, {"Escape", "Esc"}, {"Tab", "Tab"},
                        {"Backspace", "Backspace"}, {"LeftShift", "Shift"}, {"RightShift", "Shift"}, {"LeftControl", "Ctrl"},
                        {"RightControl", "Ctrl"}, {"LeftAlt", "Alt"}, {"RightAlt", "Alt"}, {"Up", "Arrow_Up"}, {"Down", "Arrow_Down"},
                        {"Left", "Arrow_Left"}, {"Right", "Arrow_Right"}, {"Insert", "Insert"}, {"Delete", "Del"}, {"Home", "Home"},
                        {"End", "End"}, {"PageUp", "Page_Up"}, {"PageDown", "Page_Down"}, {"CapsLock", "Caps_Lock"}, {"Minus", "Minus"},
                        {"Slash", "Slash"}, {"Semicolon", "Semicolon"}, {"BracketLeft", "Bracket_Left"}, {"BracketRight", "Bracket_Right"},
                        {"Apostrophe", "Quote"}, {"QuoteLeft", "Tilda"}, {"NumLock", "Num_Lock"}, {"Print", "Print_Screen"}};
                    for (const auto &[from, to] : kKeys)
                        if (name == from)
                            piece.file = keyFile(to);
                    if (piece.file.empty() && name.size() == 1 && std::isalnum(static_cast<unsigned char>(name[0])))
                        piece.file = keyFile(std::string(1, static_cast<char>(std::toupper(static_cast<unsigned char>(name[0])))));
                    else if (piece.file.empty() && name.size() >= 2 && name.size() <= 3 && name[0] == 'F' && std::isdigit(static_cast<unsigned char>(name[1])))
                        piece.file = keyFile(name);
                    else if (piece.file.empty() && name.size() == 7 && name.rfind("Keypad", 0) == 0 && std::isdigit(static_cast<unsigned char>(name[6])))
                        piece.file = keyFile(name.substr(6));
                }
                else if (device == "Pointer-0")
                {
                    if (name == "LeftButton")
                        piece.file = keyFile("Mouse_Left");
                    else if (name == "RightButton")
                        piece.file = keyFile("Mouse_Right");
                    else if (name == "MiddleButton")
                        piece.file = keyFile("Mouse_Middle");
                }
                pieces.push_back(piece);
            }
            return pieces;
        }

        const Texture2D &artTexture(State &state, const std::string &file)
        {
            auto it = state.promptArt.find(file);
            if (it != state.promptArt.end())
                return it->second;
            Texture2D &texture = state.promptArt[file];
            std::error_code error;
            const fs::path path = kamite::paths::programDir() / fromUtf8(file); // "resources/..." (the program folder)
            if (fs::is_regular_file(path, error))
            {
                texture = LoadTexture(utf8(path).c_str());
                if (texture.id != 0u)
                {
                    GenTextureMipmaps(&texture);
                    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
                }
            }
            return texture;
        }

        // A binding drawn as its pictures (+ between a chord's), from `at` (left, the row's middle); false when one of
        // them has no picture (the caller writes the name instead). The width drawn in `width`.
        bool bookBindingArt(State &state, const Book &b, ImDrawList *dl, const std::string &binding, ImVec2 at, float maxWidth, float &width)
        {
            const std::vector<ArtPiece> pieces = bindingArt(state, binding);
            if (pieces.empty())
                return false;
            for (const ArtPiece &piece : pieces)
                if (piece.file.empty() || artTexture(state, piece.file).id == 0u)
                    return false;
            const float u = b.u, h = 40.0f * u, plus = 18.0f * u, arrow = 16.0f * u;
            float total = 0.0f;
            for (size_t i = 0; i < pieces.size(); ++i)
            {
                const Texture2D &tex = artTexture(state, pieces[i].file);
                total += h * static_cast<float>(tex.width) / std::max(1, tex.height) + (pieces[i].arrow ? arrow : 0.0f) + (i ? plus : 0.0f);
            }
            const float scale = total > maxWidth && maxWidth > 0.0f ? maxWidth / total : 1.0f;
            float x = at.x;
            for (size_t i = 0; i < pieces.size(); ++i)
            {
                if (i)
                {
                    const float c = x + plus * scale * 0.5f, r = 5.0f * u * scale;
                    const ImVec2 hbar[2] = {ImVec2(c - r, at.y), ImVec2(c + r, at.y)}, vbar[2] = {ImVec2(c, at.y - r), ImVec2(c, at.y + r)};
                    book::line(dl, hbar, 2, 2.0f * u, kBookInk);
                    book::line(dl, vbar, 2, 2.0f * u, kBookInk);
                    x += plus * scale;
                }
                const Texture2D &tex = artTexture(state, pieces[i].file);
                const float ih = h * scale, iw = ih * static_cast<float>(tex.width) / std::max(1, tex.height);
                book::paint(dl);
                bookPrompt(dl, tex, ImVec2(x + iw * 0.5f, at.y), ih); // with its faint inked edge (white art on cream)
                book::ink(dl);
                x += iw;
                if (pieces[i].arrow)
                {
                    // the stick's way, a small inked arrow
                    const float a = arrow * scale, c = x + a * 0.5f, len = a * 0.42f, head = a * 0.22f;
                    const int way = pieces[i].arrow;
                    const ImVec2 dir = way == 1 ? ImVec2(0, -1) : way == 2 ? ImVec2(0, 1) : way == 3 ? ImVec2(-1, 0) : ImVec2(1, 0);
                    const ImVec2 tip(c + dir.x * len, at.y + dir.y * len), tail(c - dir.x * len, at.y - dir.y * len);
                    const ImVec2 shaft[2] = {tail, tip};
                    const ImVec2 wing[3] = {ImVec2(tip.x - dir.x * head - dir.y * head, tip.y - dir.y * head + dir.x * head), tip,
                                            ImVec2(tip.x - dir.x * head + dir.y * head, tip.y - dir.y * head - dir.x * head)};
                    book::line(dl, shaft, 2, 2.2f * u, kBookInk);
                    book::line(dl, wing, 3, 2.2f * u, kBookInk);
                    x += a;
                }
            }
            width = x - at.x;
            return true;
        }

        // Controller / Keys & Mouse: a row per PS2 input; its binding(s) in red pencil, A / a click to bind, X /
        // Delete / a right-click to clear.
        void bookBindings(State &state, const Book &b, bool controller)
        {
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u, w = ImGui::GetContentRegionAvail().x, size = 26.0f * u;
            const ImVec2 head = ImGui::GetCursorScreenPos();
            bookText(dl, state.bookFonts.heading, 24.0f * u, ImVec2(head.x, head.y + 8.0f * u), kBookInk, "Action");
            const float cell = controller ? w * 0.42f : w * 0.27f;
            const float c0 = head.x + w - (controller ? cell : 2.0f * cell);
            bookText(dl, state.bookFonts.heading, 24.0f * u, ImVec2(c0, head.y + 8.0f * u), kBookInk, controller ? "Button" : "Key 1");
            if (!controller)
                bookText(dl, state.bookFonts.heading, 24.0f * u, ImVec2(c0 + cell, head.y + 8.0f * u), kBookInk, "Key 2");
            bookRule(b, dl, head.x, head.x + w, head.y + 42.0f * u, kBookInk, 1.3f * u, 5u);
            ImGui::Dummy(ImVec2(w, 56.0f * u));
            const char *help = controller ? "Press A (or click) on it, then the button, stick or trigger it should be. "
                                            "X (or a right-click) clears it."
                                          : "Press A (or click) on a box, then the key or mouse button it should be. A right-click "
                                            "(or Delete) clears it. F-keys stay the port's shortcuts; menus always take the arrows / "
                                            "WASD, Enter / Space, Esc / Backspace and Q / E.";
            for (int row = 0; row < kControlRowCount; ++row)
            {
                if (controller && kControlRows[row].keysOnly)
                    continue;
                const Split split = splitRow(row);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float mid = p.y + kRowH * u * 0.5f;
                const char *name = *kControlRows[row].action ? kControlRows[row].action : kControlRows[row].button;
                bookText(dl, state.bookFonts.text, size, ImVec2(p.x, mid - size * 0.56f), kBookInk, name);
                const int columns = controller ? 1 : kKeyColumns;
                for (int column = 0; column < columns; ++column)
                {
                    const std::string binding = controller ? (split.pads.empty() ? std::string() : split.pads[0])
                                                           : (column < static_cast<int>(split.keys.size()) ? split.keys[static_cast<size_t>(column)] : std::string());
                    const std::string text = binding.empty() ? std::string("\xE2\x80\x94") : pc::displayName(binding);
                    const float x = c0 + cell * static_cast<float>(column);
                    ImGui::PushID(row * 8 + (controller ? 7 : column));
                    const bool pressed = bookItem(state, "##cell", ImVec2(x - 6.0f * u, p.y), ImVec2(x + cell - 4.0f * u, p.y + kRowH * u));
                    const bool focused = ImGui::IsItemFocused();
                    if (pressed)
                        startCapture(row, controller, column);
                    if ((ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ||
                        (focused && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))))
                        clearCell(row, controller, column);
                    ImGui::PopID();
                    const ImVec2 at(x, mid - size * 0.56f);
                    std::string fitted = text;
                    float artWidth = 0.0f;
                    const bool art = !binding.empty() && bookBindingArt(state, b, dl, binding, ImVec2(x, mid), cell - 14.0f * u, artWidth);
                    if (!art)
                    {
                        while (fitted.size() > 2 && textSize(state.bookFonts.text, size, fitted.c_str()).x > cell - 14.0f * u)
                            fitted = fitted.substr(0, fitted.size() - 2) + "\xE2\x80\xA6";
                        bookText(dl, state.bookFonts.text, size, at, kBookPencil, fitted.c_str());
                    }
                    if (focused)
                    {
                        // the loop goes round the action's name (K); with two keys a row, a pencil stroke under the
                        // one in hand
                        const ImVec2 nameAt(p.x, mid - size * 0.56f), ns = textSize(state.bookFonts.text, size, name);
                        bookLoop(state, b, dl, ImVec2(nameAt.x, nameAt.y + size * 0.12f), ImVec2(nameAt.x + ns.x, nameAt.y + size * 1.02f));
                        if (!controller && state.bookFocusAge > 0.02f)
                        {
                            const float used = art ? artWidth : textSize(state.bookFonts.text, size, fitted.c_str()).x;
                            const float drawn = std::clamp(state.bookFocusAge / kBookLoopTime, 0.0f, 1.0f);
                            bookRule(b, dl, x - 2.0f * u, x - 2.0f * u + (used + 4.0f * u) * drawn, mid + 21.0f * u, kBookPencil, 2.6f * u,
                                     state.bookFocusSeed);
                        }
                        std::string title = name;
                        if (!controller)
                            title += column == 0 ? " (key 1)" : " (key 2)";
                        const std::string body = std::string("The PS2's ") + kControlRows[row].button + ". Now: " + text + ".\n\n" + help;
                        bookHelp(state, title.c_str(), body.c_str(), g_controlsStatus.c_str());
                    }
                }
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + kRowH * u));
                ImGui::Dummy(ImVec2(w, 0.0f));
            }
        }

        void bookHotkeys(State &state, const Book &b)
        {
            static bool loaded = false;
            if (!loaded)
            {
                loaded = true;
                loadHotkeys(state);
            }
            ImDrawList *dl = ImGui::GetWindowDrawList();
            const float u = b.u, w = ImGui::GetContentRegionAvail().x, size = 26.0f * u;
            const float cell = w * 0.36f, c0 = ImGui::GetCursorScreenPos().x + w - cell;
            for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
            {
                const hk::Action action = static_cast<hk::Action>(i);
                if (!hk::available(action))
                    continue; // the dev keys in a release build
                const hk::Info &info = hk::info(action);
                const hk::Binding binding = hk::binding(action);
                const ImVec2 p = ImGui::GetCursorScreenPos();
                const float mid = p.y + kRowH * u * 0.5f;
                ImGui::PushID(1000 + i);
                const bool pressed = bookItem(state, "##hk", p, ImVec2(p.x + w, p.y + kRowH * u));
                const bool focused = ImGui::IsItemFocused();
                if (pressed)
                {
                    g_hotkeyCapture = i;
                    g_hotkeyStarted = GetTime();
                    g_openHotkeyCapture = true;
                    g_hotkeyWaitRelease = true;
                }
                if ((ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) ||
                    (focused && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false))))
                {
                    storeHotkey(state, action, hk::Binding{});
                    g_hotkeyStatus = std::string(info.label) + ": no key.";
                }
                ImGui::PopID();
                const ImVec2 at(p.x, mid - size * 0.56f);
                bookText(dl, state.bookFonts.text, size, at, kBookInk, info.label);
                const std::string key = binding.bound() ? hk::text(binding) : std::string("\xE2\x80\x94");
                bookText(dl, state.bookFonts.text, size, ImVec2(c0, at.y), kBookPencil, key.c_str());
                if (focused)
                {
                    const ImVec2 ts = textSize(state.bookFonts.text, size, info.label);
                    bookLoop(state, b, dl, ImVec2(at.x, at.y + size * 0.12f), ImVec2(at.x + ts.x, at.y + size * 1.02f));
                    bookHelp(state, info.label,
                             "The port's own key for it. Press A (or click), then the key (with Shift or Ctrl held if you want). "
                             "A right-click (or Delete) clears it. The F2 menu's Controls tab edits the same list.",
                             g_hotkeyStatus.c_str());
                }
                ImGui::SetCursorScreenPos(ImVec2(p.x, p.y + kRowH * u));
                ImGui::Dummy(ImVec2(w, 0.0f));
            }
        }

        // The right page: the focused row's name, its help, its note at the foot in red pencil.
        void bookHelpPage(State &state, const Book &b)
        {
            ImDrawList *dl = b.dl;
            const float u = b.u, w = b.rightMax.x - b.rightMin.x;
            const ImVec2 m = b.rightMin;
            if (state.bookTitle.empty())
                return;
            const float hs = 38.0f * u;
            bookText(dl, state.bookFonts.heading, hs, ImVec2(m.x, m.y + 6.0f * u), kBookInk, state.bookTitle.c_str(), w);
            const float th = textSize(state.bookFonts.heading, hs, state.bookTitle.c_str(), w).y;
            bookRule(b, dl, m.x, m.x + w, m.y + 6.0f * u + th + 6.0f * u, kBookInk, 1.8f * u, 3u);
            float size = 28.0f * u;
            if (state.bookHelpRoom > 0.0f) // a long help (a mod's) shrinks to leave its page's stamps clear
            {
                const float room = state.bookHelpRoom * u - (th + 50.0f * u), h = textSize(state.bookFonts.text, size, state.bookHelp.c_str(), w).y;
                if (h > room && room > 0.0f)
                    size *= std::max(0.7f, room / h);
            }
            state.bookHelpSize = size / u;
            bookText(dl, state.bookFonts.text, size, ImVec2(m.x, m.y + th + 50.0f * u), kBookInk, state.bookHelp.c_str(), w);
            if (!state.bookNote.empty())
            {
                const float ns = 25.0f * u;
                const float nh = textSize(state.bookFonts.text, ns, state.bookNote.c_str(), w).y;
                bookText(dl, state.bookFonts.text, ns, ImVec2(m.x, b.rightMax.y - nh - 14.0f * u), kBookPencil, state.bookNote.c_str(), w);
            }
        }

        // ---- the slips, Controls' index tabs, the ribbon ---------------------------------------------------------------
        // The prompts' art set for the device in use ([Game] ButtonPrompts; auto: the controller's name) and loading it.
        std::string padFolder(const State &state);
        std::string promptFolder(const State &state) { return state.lastDevice == 1 ? std::string("keyboard") : padFolder(state); }
        std::string padFolder(const State &state)
        {
            std::string choice = lower(state.ini.get("Game", "ButtonPrompts", "auto"));
            if (choice == "auto" && IsGamepadAvailable(0))
            {
                const std::string name = lower(GetGamepadName(0) ? GetGamepadName(0) : "");
                const auto has = [&](const char *word) { return name.find(word) != std::string::npos; };
                choice = has("playstation") || has("ps4") || has("ps5") || has("dualsense") || has("dualshock") || has("sony") ||
                                 has("wireless controller")
                             ? "playstation"
                         : has("nintendo") || has("switch") || has("pro controller") || has("joy-con") ? "nintendo"
                         : has("steam")                                                                ? "steam"
                                                                                                       : "xbox";
            }
            if (choice == "playstation")
                return "ps3";
            if (choice == "nintendo")
                return "switch";
            if (choice == "steam")
                return "steamdeck";
            return "xbox360";
        }

        void loadPrompts(State &state)
        {
            const std::string folder = promptFolder(state);
            if (folder == state.promptSet)
                return;
            for (Texture2D &t : state.prompt)
                if (t.id != 0u)
                {
                    UnloadTexture(t);
                    t = Texture2D{};
                }
            state.promptSet = folder;
            // tab left, tab right, sub-tab left, sub-tab right, back (the east button: Nintendo's A)
            static const std::map<std::string, std::array<const char *, 5>> kFiles = {
                {"xbox360", {"360_LB", "360_RB", "360_LT", "360_RT", "360_B"}},
                {"ps3", {"PS3_L1", "PS3_R1", "PS3_L2", "PS3_R2", "PS3_Circle"}},
                {"switch", {"Switch_LB", "Switch_RB", "Switch_LT", "Switch_RT", "Switch_A"}},
                {"steamdeck", {"SteamDeck_L1", "SteamDeck_R1", "SteamDeck_L2", "SteamDeck_R2", "SteamDeck_B"}},
                {"keyboard", {"Q_Key_Light", "E_Key_Light", "Z_Key_Light", "C_Key_Light", "Esc_Key_Light"}},
            };
            const auto files = kFiles.find(folder);
            if (files == kFiles.end())
                return;
            std::error_code error;
            for (size_t i = 0; i < 5; ++i)
            {
                const fs::path path = kamite::paths::resources() / "prompts" / "xelu" / folder / (std::string(files->second[i]) + ".png");
                if (!fs::is_regular_file(path, error))
                    continue;
                state.prompt[i] = LoadTexture(utf8(path).c_str());
                if (state.prompt[i].id != 0u)
                {
                    GenTextureMipmaps(&state.prompt[i]);
                    SetTextureFilter(state.prompt[i], TEXTURE_FILTER_TRILINEAR);
                }
            }
        }

        // The last device used, by deliberate input (never mouse movement alone): which prompts show (none: the mouse).
        void trackDevice(State &state)
        {
            for (int key = KEY_SPACE; key <= KEY_KB_MENU; ++key) // (not GetKeyPressed: the hotkey capture reads its queue)
                if (IsKeyPressed(key))
                {
                    state.lastDevice = 1;
                    break;
                }
            for (int pad = 0; pad < 4; ++pad)
            {
                if (!IsGamepadAvailable(pad))
                    continue;
                if (GetGamepadButtonPressed() != GAMEPAD_BUTTON_UNKNOWN)
                    state.lastDevice = 2;
                for (int axis = 0; axis < 4; ++axis)
                    if (std::fabs(GetGamepadAxisMovement(pad, axis)) > 0.5f)
                        state.lastDevice = 2;
            }
            if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT) || IsMouseButtonPressed(MOUSE_BUTTON_RIGHT) || GetMouseWheelMove() != 0.0f)
                state.lastDevice = 0;
        }

        void switchBookTab(State &state, int tab)
        {
            tab = (tab + kBookTabCount) % kBookTabCount;
            if (tab == state.bookTab)
                return;
            state.bookTab = tab;
            state.bookTabChanged = true;
            state.focusFirst = true;
            state.bookQuiet = 2;
            state.bookTitle.clear();
            state.bookHelp.clear();
            state.bookNote.clear();
            playEffect(state, state.rustle, state.hasRustle);
            state.soundThisFrame = true;
            state.quietFrames = 3; // the cursor's move to the page's first row: no tick (K)
        }

        void switchControlsPage(State &state, int page)
        {
            page = (page + kControlsPageCount) % kControlsPageCount;
            if (page == state.bookControls)
                return;
            state.bookControls = page;
            state.bookTabChanged = true;
            state.focusFirst = true;
            state.bookQuiet = 2;
            state.bookTitle.clear();
            state.bookHelp.clear();
            state.bookNote.clear();
            playEffect(state, state.rustle, state.hasRustle);
            state.soundThisFrame = true;
            state.quietFrames = 3; // the cursor's move to the page's first row: no tick (K)
        }

        void bookSlips(State &state, const Book &b, bool input)
        {
            ImDrawList *dl = b.dl;
            const float u = b.u;
            const float x0 = b.left + 12.0f * u, x1 = b.right - 12.0f * u, gap = 8.0f * u;
            const float w = (x1 - x0 - gap * (kBookTabCount - 1)) / kBookTabCount;
            // behind first, the open one last (in front)
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < kBookTabCount; ++i)
                {
                    const bool open = i == state.bookTab;
                    if (open != (pass == 1))
                        continue;
                    const float a = x0 + static_cast<float>(i) * (w + gap), h = (open ? 86.0f : 64.0f) * u;
                    const ImVec2 quad[4] = {ImVec2(a, b.top - h), ImVec2(a + w, b.top - h), ImVec2(a + w, b.top + 2.0f * u), ImVec2(a, b.top + 2.0f * u)};
                    book::paper(dl, quad, open ? kSlipOpen : kSlipTints[i], 5.0f * u);
                    book::ink(dl);
                    const float size = 29.0f * u;
                    const ImVec2 ts = textSize(state.bookFonts.heading, size, kBookTabs[i]);
                    const float scale = ts.x > w - 10.0f * u ? (w - 10.0f * u) / ts.x : 1.0f;
                    bookText(dl, state.bookFonts.heading, size * scale, ImVec2(a + (w - ts.x * scale) * 0.5f, b.top - h * 0.5f - ts.y * scale * 0.55f),
                             open ? kBookInk : IM_COL32(38, 26, 18, 200), kBookTabs[i]);
                    if (input)
                    {
                        ImGui::PushID(i);
                        if (bookItem(state, "##slip", quad[0], ImVec2(quad[1].x, b.top), false))
                            switchBookTab(state, i);
                        ImGui::PopID();
                    }
                }
            if (state.lastDevice != 0)
            {
                book::paint(dl);
                bookPrompt(dl, state.prompt[0], ImVec2(x0 - 40.0f * u, b.top - 34.0f * u), 50.0f * u);
                bookPrompt(dl, state.prompt[1], ImVec2(x1 + 40.0f * u, b.top - 34.0f * u), 50.0f * u);
                book::ink(dl);
            }
        }

        void bookIndexTabs(State &state, const Book &b, bool input)
        {
            ImDrawList *dl = b.dl;
            const float u = b.u;
            const float top = b.top + 36.0f * u, bottom = b.bottom - 34.0f * u, gap = 10.0f * u;
            const float h = (bottom - top - gap * (kControlsPageCount - 1)) / kControlsPageCount;
            for (int pass = 0; pass < 2; ++pass)
                for (int i = 0; i < kControlsPageCount; ++i)
                {
                    const bool open = i == state.bookControls;
                    if (open != (pass == 1))
                        continue;
                    const float a = top + static_cast<float>(i) * (h + gap), out = (open ? 74.0f : 56.0f) * u;
                    const ImVec2 quad[4] = {ImVec2(b.right - 4.0f * u, a), ImVec2(b.right + out, a + 6.0f * u), ImVec2(b.right + out, a + h - 6.0f * u),
                                            ImVec2(b.right - 4.0f * u, a + h)};
                    book::paper(dl, quad, open ? kSlipOpen : kSlipTints[(i + 2) % kBookTabCount], 5.0f * u);
                    book::ink(dl);
                    const float size = 27.0f * u;
                    const ImVec2 ts = textSize(state.bookFonts.heading, size, kControlsPages[i]);
                    const ImVec2 centre(b.right + out * 0.5f, a + h * 0.5f);
                    const int start = dl->VtxBuffer.Size;
                    bookText(dl, state.bookFonts.heading, size, ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f),
                             open ? kBookInk : IM_COL32(38, 26, 18, 200), kControlsPages[i]);
                    rotateSince(dl, start, centre, 1.5707963f);
                    if (input)
                    {
                        ImGui::PushID(100 + i);
                        if (bookItem(state, "##index", ImVec2(b.right, a), ImVec2(b.right + out, a + h), false))
                            switchControlsPage(state, i);
                        ImGui::PopID();
                    }
                }
            if (state.lastDevice != 0)
            {
                book::paint(dl);
                bookPrompt(dl, state.prompt[2], ImVec2(b.right + 112.0f * u, top + h * 0.5f), 48.0f * u);
                bookPrompt(dl, state.prompt[3], ImVec2(b.right + 112.0f * u, bottom - h * 0.5f), 48.0f * u);
                book::ink(dl);
            }
        }

        // The ribbon bookmark from under the book's foot at the spine (its top cut along the book's edge, so it comes
        // out from under it; the book's shadow on it), "Back" in cream along it.
        bool bookRibbon(State &state, const Book &b, bool input)
        {
            ImDrawList *dl = b.dl;
            const float u = b.u;
            const float angle = 0.6632f; // 38 degrees
            const ImVec2 d(std::cos(angle), std::sin(angle));
            const float width = 42.0f * u, length = 300.0f * u, across = width / std::sin(angle);
            const float y = b.bottom - 14.0f * u, cx = b.space->spine + 12.0f * u; // its top at the pages' visible edge (the quad's is a little lower)
            const ImVec2 tl(cx - across * 0.5f, y), tr(cx + across * 0.5f, y);
            const ImVec2 br(tr.x + d.x * length, tr.y + d.y * length), bl(tl.x + d.x * length, tl.y + d.y * length);
            book::paint(dl);
            // its shadow on the cloth, then the satin, a sheen along it, the book's shadow across its top
            const ImVec2 s(5.0f * u, 7.0f * u);
            dl->AddQuadFilled(ImVec2(tl.x + s.x, tl.y + s.y), ImVec2(tr.x + s.x, tr.y + s.y), ImVec2(br.x + s.x, br.y + s.y), ImVec2(bl.x + s.x, bl.y + s.y),
                              ImGui::GetColorU32(IM_COL32(0, 0, 0, 70)));
            dl->AddQuadFilled(tl, tr, br, bl, ImGui::GetColorU32(IM_COL32(118, 20, 28, 255)));
            quadColors(dl, ImVec2(tl.x + (tr.x - tl.x) * 0.38f, y), ImVec2(tl.x + (tr.x - tl.x) * 0.5f, y),
                       ImVec2(bl.x + (br.x - bl.x) * 0.5f, bl.y), ImVec2(bl.x + (br.x - bl.x) * 0.38f, bl.y), IM_COL32(190, 70, 76, 10),
                       IM_COL32(200, 80, 86, 60), IM_COL32(200, 80, 86, 60), IM_COL32(190, 70, 76, 10));
            const float shade = 20.0f * u;
            quadColors(dl, tl, tr, ImVec2(tr.x + d.x * shade / d.y, y + shade), ImVec2(tl.x + d.x * shade / d.y, y + shade), IM_COL32(20, 6, 6, 200),
                       IM_COL32(20, 6, 6, 200), IM_COL32(0, 0, 0, 0), IM_COL32(0, 0, 0, 0));
            // "Back" along it
            const float size = 26.0f * u;
            const ImVec2 ts = textSize(state.bookFonts.heading, size, "Back");
            const ImVec2 centre(cx + d.x * 74.0f * u, y + d.y * 74.0f * u);
            const int start = dl->VtxBuffer.Size;
            bookText(dl, state.bookFonts.heading, size, ImVec2(centre.x - ts.x * 0.5f, centre.y - ts.y * 0.5f), kBookCream, "Back");
            rotateSince(dl, start, centre, angle);
            if (state.lastDevice != 0)
                bookPrompt(dl, state.prompt[4], ImVec2(cx - 52.0f * u, y + 54.0f * u), 44.0f * u);
            book::ink(dl);
            if (!input)
                return false;
            return bookItem(state, "##ribbon", ImVec2(std::min(tl.x, bl.x), y), ImVec2(std::max(tr.x, br.x), y + d.y * 170.0f * u), false);
        }

        // ---- the book ------------------------------------------------------------------------------------------------
        void drawBook(State &state, const book::Space &space)
        {
            g_captureFrame = false;
            g_settingRows = 0;
            trackDevice(state);
            book::setPaperTexture(state.scene.paper());
            loadPrompts(state);
            Book b;
            b.space = &space;
            b.u = space.unit();
            b.dl = ImGui::GetWindowDrawList();
            const float u = b.u;
            b.left = space.rect.x;
            b.right = space.rect.x + space.rect.width;
            b.top = space.rect.y;
            b.bottom = space.rect.y + space.rect.height;
            b.leftMin = ImVec2(b.left + 56.0f * u, b.top + 64.0f * u);
            b.leftMax = ImVec2(space.spine - 50.0f * u, b.bottom - 50.0f * u);
            b.rightMin = ImVec2(space.spine + 50.0f * u, b.top + 64.0f * u);
            b.rightMax = ImVec2(b.right - 56.0f * u, b.bottom - 50.0f * u);
            // what is written on the pages in a size that reads on a TV across the room (K: scrolling is fine); the
            // slips, tabs and ribbon keep the book's own unit
            Book pg = b;
            pg.u = u * kBookTextScale;
            // the whole book draws outside the window's own clip (the slips stand above the screen's top edge at times)
            const ImVec2 display = ImGui::GetIO().DisplaySize;
            ImGui::PushClipRect(ImVec2(-0.25f * display.x, -0.25f * display.y), ImVec2(1.25f * display.x, 1.25f * display.y), false);
            const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            const bool input = !state.optionsLeaving;
            if (input && !popup)
            {
                if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false))
                    switchBookTab(state, state.bookTab - 1);
                else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false))
                    switchBookTab(state, state.bookTab + 1);
                if (state.bookTab == kBookControlsTab)
                {
                    if (ImGui::IsKeyPressed(ImGuiKey_GamepadL2, false) || ImGui::IsKeyPressed(ImGuiKey_Z, false))
                        switchControlsPage(state, state.bookControls - 1);
                    else if (ImGui::IsKeyPressed(ImGuiKey_GamepadR2, false) || ImGui::IsKeyPressed(ImGuiKey_C, false))
                        switchControlsPage(state, state.bookControls + 1);
                }
            }
            bookSlips(state, b, input && !popup);
            if (state.bookTab == kBookControlsTab)
                bookIndexTabs(state, b, input && !popup);
            const bool ribbon = bookRibbon(state, b, input && !popup);

            // the left page: the tab's rows (scrolling)
            const int now = state.bookTab == 0 ? presets::current(state.ini) : presets::kCustom;
            static int focusedPreset = -1;
            if (state.bookTab == 0)
                readPresetPhotos(state);
            ImGui::SetCursorScreenPos(b.leftMin);
            ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(0.0f, 0.0f));
            ImGui::PushStyleColor(ImGuiCol_ChildBg, ImVec4(0, 0, 0, 0));
            if (state.bookTabChanged)
                ImGui::SetNextWindowScroll(ImVec2(0.0f, 0.0f));
            ImGui::PushID(state.bookTab * 10 + (state.bookTab == kBookControlsTab ? state.bookControls : 0));
            const ImVec2 pageSize(b.leftMax.x - b.leftMin.x, b.leftMax.y - b.leftMin.y);
            float scrollY = 0.0f, scrollMax = 0.0f;
            if (ImGui::BeginChild("##page", pageSize, ImGuiChildFlags_NavFlattened, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoBackground))
            {
                if (!input)
                    ImGui::BeginDisabled();
                if (state.bookTab == 0)
                    bookPresetsLeft(state, pg, now, focusedPreset);
                else if (state.bookTab == kBookControlsTab && state.bookControls == 1)
                    bookBindings(state, pg, true);
                else if (state.bookTab == kBookControlsTab && state.bookControls == 2)
                    bookBindings(state, pg, false);
                else if (state.bookTab == kBookControlsTab && state.bookControls == 3)
                    bookHotkeys(state, pg);
                else
                    bookSettings(state, pg, kBookSettingTabs[state.bookTab - 1]);
                ImGui::Dummy(ImVec2(1.0f, 20.0f * u));
                if (!input)
                    ImGui::EndDisabled();
                scrollY = ImGui::GetScrollY();
                scrollMax = ImGui::GetScrollMaxY();
            }
            ImGui::EndChild();
            ImGui::PopID();
            ImGui::PopStyleColor();
            ImGui::PopStyleVar(2);
            // the pencil scroll bar in the left page's outer margin
            if (scrollMax > 0.5f)
            {
                const float x = b.left + 28.0f * u, y0 = b.leftMin.y, y1 = b.leftMax.y, total = scrollMax + pageSize.y;
                const ImVec2 track[2] = {ImVec2(x, y0), ImVec2(x, y1)};
                book::line(b.dl, track, 2, 1.1f * u, IM_COL32(60, 58, 56, 110));
                const float t0 = y0 + (y1 - y0) * scrollY / total, t1 = y0 + (y1 - y0) * (scrollY + pageSize.y) / total;
                for (int k = 0; k < 4; ++k)
                {
                    const ImVec2 thumb[2] = {ImVec2(x - 1.6f * u + k * 1.1f * u, t0), ImVec2(x - 1.6f * u + k * 1.1f * u, t1)};
                    book::line(b.dl, thumb, 2, 1.5f * u, IM_COL32(55, 53, 50, 110), 0.1f);
                }
            }

            // the right page
            if (state.bookTab == 0)
                bookPresetsRight(state, pg, now, focusedPreset >= 0 ? focusedPreset : std::max(0, now));
            else
            {
                // the Mods tab: the help stops above its stamps and the line of keys that move a mod
                state.bookHelpRoom = state.bookTab == kBookModsTab ? (b.rightMax.y - b.rightMin.y) / pg.u - 215.0f : 0.0f;
                bookHelpPage(state, pg);
                if (state.bookTab == kBookModsTab)
                    bookModsRight(state, pg, input && !popup);
                if (input && !popup && state.bookTab == kBookControlsTab && (state.bookControls == 1 || state.bookControls == 2))
                {
                    const ImVec2 at((b.rightMin.x + b.rightMax.x) * 0.5f, b.rightMax.y - 150.0f * pg.u);
                    if (bookStamp(state, pg, "##reset", g_resetArmed ? "AGAIN TO RESET ALL" : "RESET ALL BINDINGS", at, 20.0f * pg.u, -1.5f))
                    {
                        if (g_resetArmed)
                        {
                            pc::resetDefaults(kPlayer);
                            g_controlsStatus = "Default bindings restored.";
                        }
                        g_resetArmed = !g_resetArmed;
                    }
                    if (bookStamp(state, pg, "##ini", "OPEN CONTROLS.INI", ImVec2(at.x, at.y + 70.0f * pg.u), 20.0f * pg.u, 1.0f))
                    {
                        std::string message;
                        pc::save(message);
                        platform::open(state.context->exeDir / "controls.ini");
                    }
                }
                if (input && !popup && state.bookTab == kBookControlsTab && state.bookControls == 3)
                {
                    const ImVec2 at((b.rightMin.x + b.rightMax.x) * 0.5f, b.rightMax.y - 120.0f * pg.u);
                    if (bookStamp(state, pg, "##defaults", "DEFAULT HOTKEYS", at, 20.0f * pg.u, -1.5f))
                    {
                        for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
                            if (hk::available(static_cast<hk::Action>(i)))
                                storeHotkey(state, static_cast<hk::Action>(i), hk::info(static_cast<hk::Action>(i)).def);
                        g_hotkeyStatus = "Default hotkeys restored.";
                    }
                }
            }
            bookNotes(state, pg);
            bookModNote(state, pg);
            ImGui::PopClipRect();
            state.bookTabChanged = false;
            if (state.bookQuiet > 0)
                --state.bookQuiet;

            // Back: B / Esc (not while a note or a list is up), or the ribbon
            // (a list or a note closed by this very press was open last frame: that press is theirs)
            const bool cancel = !g_captureFrame && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) && !state.popupWasOpen &&
                                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false) ||
                                 ImGui::IsKeyPressed(ImGuiKey_Backspace, false));
            if ((ribbon || cancel) && !state.optionsLeaving && !popup)
            {
                bookSound(state, false);
                state.ini.save();
                saveControls();
                g_resetArmed = false;
                state.optionsLeaving = true;
            }
        }

        void drawOptions(State &state)
        {
            g_captureFrame = false;
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 30.0f)));
            pushSize(state, 46.0f);
            centeredText("Options", kHeading);
            ImGui::PopFont();
            const float width = std::min(ImGui::GetWindowWidth() - fontSize(state, 60.0f), fontSize(state, 1040.0f));
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 21.0f);
            const float footer = ImGui::GetFrameHeightWithSpacing() + fontSize(state, 36.0f);
            ImGui::BeginChild("options", ImVec2(width, ImGui::GetContentRegionAvail().y - footer),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            g_categories = 0;
            if (ImGui::BeginTabBar("tabs"))
            {
                // Presets first (K, Nov 16).
                if (ImGui::BeginTabItem("Presets"))
                {
                    drawPresets(state);
                    ImGui::EndTabItem();
                }
                // The launcher's own order (K, Nov 13: Controls after Sound; the game keeps Controls first).
                constexpr settings::Tab kLauncherTabs[] = {settings::Tab::Display,  settings::Tab::Graphics,
                                                           settings::Tab::Interface, settings::Tab::Gameplay,
                                                           settings::Tab::Sound,    settings::Tab::Controls,
                                                           settings::Tab::Mods};
                for (const settings::Tab tab : kLauncherTabs)
                {
                    if (!ImGui::BeginTabItem(settings::tabName(tab)))
                        continue;
                    drawTab(state, tab);
                    if (tab == settings::Tab::Controls)
                        drawControls(state); // the bindings and hotkeys
                    if (tab == settings::Tab::Mods)
                        drawModList(state); // the mods under mods/
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            const float backWidth = fontSize(state, 200.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - backWidth) * 0.5f);
            const bool back = ImGui::Button("Back", ImVec2(backWidth, 0.0f));
            ownSoundItem();
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            const bool cancel = !g_captureFrame && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
            ImGui::PopFont();
            if ((back || cancel) && !state.optionsLeaving)
            {
                if (state.saloonOn)
                    bookSound(state, false); // back out of the book
                else
                    menuSound(state, kCancel);
                state.ini.save();
                saveControls();
                g_resetArmed = false;
                if (state.saloonOn) // the panel fades out, then the zoom plays backwards (the main loop)
                {
                    state.optionsLeaving = true;
                    return;
                }
                state.screen = Screen::Home;
                state.focusFirst = true;
                return;
            }
            state.focusFirst = false;
        }
    }

    bool wanted(bool haveImage)
    {
        if (!haveImage || platform::shiftHeld())
            return true;
        const char *value = std::getenv("KAMITE_LAUNCHER");
        return !(value && *value == '0');
    }

    Result run(const Context &context)
    {
        State state;
        state.context = &context;
        if (!state.ini.load(context.iniPath))
            KM_WARN(Boot, "launcher: cannot read " << utf8(context.iniPath));
        // [Video] CameraDistance (100 = Gene keeps his size) became CameraZoomOut (0 = that) on Oct 30: carry it over.
        if (state.ini.get("Video", "CameraZoomOut").empty() && !state.ini.get("Video", "CameraDistance").empty())
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", 100.0 - std::atof(state.ini.get("Video", "CameraDistance").c_str()));
            state.ini.set("Video", "CameraZoomOut", text);
        }
        std::error_code error;
        // A new install (K, Nov 16): no boot ELF checked out yet and no preset picked: the wanted posters once.
        state.firstRunPending = !state.ini.getBool("Launcher", "PresetPicked", false) &&
                                !fs::exists(context.cacheDir / kBootElfPath, error);
        state.installed = !context.iso.empty() && fs::is_regular_file(context.iso, error) && context.message.empty();
        if (!context.iso.empty())
            std::snprintf(state.isoText, sizeof(state.isoText), "%s", utf8(context.iso).c_str());
        if (!context.message.empty())
        {
            state.installMessage = context.message;
            state.installError = true;
            state.screen = Screen::Install;
        }

        ps2_host_display::openHostWindow(kWindowTitle);
        // Without vsync: the monitor's own rate (a 60 cap on a faster screen judders Play's animation; K Nov 10).
        if (!ps2_host_display::vsyncEnabled())
        {
            const int rate = GetMonitorRefreshRate(GetCurrentMonitor());
            SetTargetFPS(rate >= 60 && rate <= 360 ? rate : 60);
        }
        rlImGuiSetup(true);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        const fs::path fontPath = kamite::paths::resources() / "fonts" / "Roboto-Regular.ttf";
        if (fs::is_regular_file(fontPath, error))
            state.font = io.Fonts->AddFontFromFileTTF(utf8(fontPath).c_str(), 20.0f);
        {
            // the Options book's (any size: ImGui 1.92 bakes each as it is used); room for them from the start (a growing
            // atlas re-packs as new sizes come in)
            io.Fonts->TexMinWidth = 2048;
            io.Fonts->TexMinHeight = 1024;
            io.Fonts->TexGlyphPadding = 2; // the book grows each glyph's quad a texel into it (launcher_book.cpp)
            const fs::path fonts = kamite::paths::resources() / "fonts";
            const auto add = [&](const char *name) -> ImFont * {
                return fs::is_regular_file(fonts / name, error) ? io.Fonts->AddFontFromFileTTF(utf8(fonts / name).c_str(), 26.0f) : nullptr;
            };
            // K, Nov 18: the posters' fonts, not the bill's old ones (Wendyville titles, Dominican for the rest)
            state.bookFonts.heading = add("Wendyville.ttf");
            g_headingFont = state.bookFonts.heading;
            state.bookFonts.text = addAtlasFont(io, kamite::paths::resources() / "splash" / "splash.fnt", fonts / "EBGaramond-Regular.ttf");
            if (!state.bookFonts.text)
                state.bookFonts.text = add("EBGaramond-Regular.ttf");
            state.bookFonts.stamp = add("HoltwoodOneSC-Regular.ttf");
            if (!state.bookFonts.stamp)
                state.bookFonts.stamp = state.bookFonts.heading;
        }
        applyStyle();
        const auto loadImage = [&](Texture2D &texture, const char *name)
        {
            const fs::path path = kamite::paths::resources() / "images" / name;
            if (!fs::is_regular_file(path, error))
                return;
            texture = LoadTexture(utf8(path).c_str());
            if (texture.id != 0u)
            {
                GenTextureMipmaps(&texture);
                SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
            }
        };
        loadImage(state.logo, "logo.png");
        loadImage(state.kana, "kana.png");
        loadImage(state.rearmed, "rearmed.png");
        loadImage(state.rearmedText, "rearmed_text.png");
        loadImage(state.rearmedGlow, "rearmed_glow.png");
        loadImage(state.bill.steamMark, "steam_mark.png"); // the bill's Add to Steam mark (Flaticon's sketch, ringed)
        if (state.kana.id != 0u && state.logo.id != 0u)
            state.scene.setKana(state.kana, kLogoContent, static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width));
        const fs::path burstPath = kamite::paths::resources() / "images" / "burst.png";
        if (fs::is_regular_file(burstPath, error))
        {
            state.burst = LoadTexture(utf8(burstPath).c_str());
            if (state.burst.id != 0u)
            {
                GenTextureMipmaps(&state.burst);
                SetTextureFilter(state.burst, TEXTURE_FILTER_TRILINEAR);
            }
        }
        loadSplash(state, kamite::paths::resources() / "splash");
        // The saloon (resources/saloon): its images decode on worker threads while the launcher starts.
        state.saloonOn = state.scene.open(kamite::paths::resources() / "saloon");
        if (state.saloonOn)
        {
            if (!saloon::loadBillFonts(state.billFonts, kamite::paths::resources() / "fonts", state.splash))
            {
                KM_WARN(Boot, "launcher: the bill's fonts are missing from resources/fonts, plain launcher");
                state.scene.close();
                state.saloonOn = false;
            }
            else if (!(state.paperOn = state.paper.load(kamite::paths::resources() / "fonts", &state.billFonts.text)))
                KM_WARN(Boot, "launcher: the newspaper's fonts are missing from resources/fonts, the corner slip instead");
        }
        InitAudioDevice();
        state.audio = IsAudioDeviceReady();
        const fs::path musicPath = kamite::paths::resources() / "music" / "launcher.ogg";
        state.musicPath = musicPath;
        if (state.audio && fs::is_regular_file(musicPath, error))
        {
            state.music = LoadMusicStream(utf8(musicPath).c_str());
            state.hasMusic = IsMusicValid(state.music);
            if (state.hasMusic)
            {
                state.music.looping = true; // the file is cut to loop seamlessly
                SetMusicVolume(state.music, kMusicVolume);
                PlayMusicStream(state.music);
            }
        }
        // The saloon's sounds (K's, resources/sfx/launcher): the bar's murmur (a loop under the music, saloon only), the
        // pen's scribble round a loop, the smoothie's last sip.
        if (state.audio)
        {
            const fs::path sfx = kamite::paths::resources() / "sfx" / "launcher";
            if (fs::is_regular_file(sfx / "bar_ambience.ogg", error))
            {
                state.ambience = LoadMusicStream(utf8(sfx / "bar_ambience.ogg").c_str());
                state.hasAmbience = IsMusicValid(state.ambience);
                if (state.hasAmbience)
                {
                    state.ambience.looping = true;
                    SetMusicVolume(state.ambience, 0.0f);
                    PlayMusicStream(state.ambience);
                }
            }
            const auto effect = [&](const char *name, Sound &sound) {
                if (!fs::is_regular_file(sfx / name, error))
                    return false;
                sound = LoadSound(utf8(sfx / name).c_str());
                return sound.frameCount > 0;
            };
            state.hasPen = effect("pen_circle.ogg", state.pen);
            state.hasSlurp = effect("smoothie_drink.ogg", state.slurp);
            state.hasBite = effect("eat_fruit.ogg", state.bite);
            state.hasBookOpen = effect("book_open.ogg", state.bookOpen);
            state.hasBookClose = effect("book_close.ogg", state.bookClose);
            // a slip turned in the book: a paper rustle cut from the book opening (K, Nov 18: for now), faded at both ends
            if (fs::is_regular_file(sfx / "book_open.ogg", error))
            {
                Wave wave = LoadWave(utf8(sfx / "book_open.ogg").c_str());
                if (wave.frameCount > 0 && wave.sampleRate > 0)
                {
                    WaveFormat(&wave, static_cast<int>(wave.sampleRate), 16, static_cast<int>(wave.channels));
                    const int from = static_cast<int>(0.14f * wave.sampleRate), to = std::min(static_cast<int>(wave.frameCount), static_cast<int>(0.30f * wave.sampleRate));
                    if (to > from + 64)
                    {
                        WaveCrop(&wave, from, to);
                        int16_t *pcm = static_cast<int16_t *>(wave.data);
                        const int frames = static_cast<int>(wave.frameCount), fade = std::min(frames / 3, static_cast<int>(0.02f * wave.sampleRate));
                        for (int i = 0; i < frames; ++i)
                        {
                            const float g = std::min({1.0f, static_cast<float>(i) / std::max(1, fade), static_cast<float>(frames - 1 - i) / std::max(1, fade * 3)});
                            for (unsigned int c = 0; c < wave.channels; ++c)
                                pcm[static_cast<size_t>(i) * wave.channels + c] = static_cast<int16_t>(pcm[static_cast<size_t>(i) * wave.channels + c] * g);
                        }
                        state.rustle = LoadSoundFromWave(wave);
                        state.hasRustle = state.rustle.frameCount > 0;
                    }
                }
                UnloadWave(wave);
            }
        }
        state.volume = std::clamp(static_cast<float>(std::atof(state.ini.get("Launcher", "Volume", "0.8").c_str())), 0.0f, 1.0f);
        state.exePath = platform::thisExe();
        update::cleanup(kamite::paths::programDir());
        if (g_updateCheck.state.load() == static_cast<int>(update::CheckState::Off))
        {
            std::string channel = state.ini.get("Launcher", "Channel", "");
            for (char &ch : channel)
                ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
            // UpdateTest: a dev build's test switch; Channel=tester: the tester zip's (builds of main).
            update::startCheck(g_updateCheck, state.ini.getBool("Launcher", "UpdateTest", false), channel == "tester");
        }
        KM_INFO(Boot, "launcher: " << update::versionLabel());
#if KAMITE_LAUNCHER_STEAM
        state.steam = steam::probe();
        KM_INFO(Boot, "launcher: steam " << (state.steam.available ? (state.steam.added ? "found, entry there" : "found") : "not found"));
#endif
        state.muted = state.ini.getBool("Launcher", "Mute", false);
        if (state.installed)
        {
            loadMenuSounds(state, context.iso);
            loadSnackVoices(state, context.iso);
            startBark(state, context.iso);
        }
        KM_INFO(Boot, "launcher: shown (" << (state.installed ? "installed" : "not installed") << ")");
        loadHotkeys(state); // F11 (Full screen) as GHR.ini binds it

        // The window follows Display Mode at once (K, Nov 21): the Options row, and F11 as in the game (saved to the
        // row's two keys).
        const auto windowMode = []
        { return IsWindowState(FLAG_BORDERLESS_WINDOWED_MODE) ? 2 : IsWindowMaximized() ? 1 : 0; };
        int appliedMode = windowMode();
        // Into a plain window: one that came back small (the size raylib had before fullscreen: 960 wide) is set to
        // 16:9 at 3/4 of the monitor's height, centred.
        const auto sizeWindow = [](int mode)
        {
            if (mode != 0)
                return;
            const int monitor = GetCurrentMonitor();
            const int mw = GetMonitorWidth(monitor), mh = GetMonitorHeight(monitor);
            if (mw <= 0 || mh <= 0 || GetScreenHeight() >= mh * 2 / 3)
                return;
            int h = mh * 3 / 4, w = h * 16 / 9;
            if (w > mw * 9 / 10)
                w = mw * 9 / 10, h = w * 9 / 16;
            const Vector2 origin = GetMonitorPosition(monitor);
            SetWindowSize(w, h);
            SetWindowPosition(static_cast<int>(origin.x) + (mw - w) / 2, static_cast<int>(origin.y) + (mh - h) / 2);
        };
        while (!state.done)
        {
            if (WindowShouldClose())
            {
                state.result = Result::Quit;
                break;
            }
            {
                const int wanted = state.ini.getBool("Video", "StartFullscreen", true)    ? 2
                                   : state.ini.getBool("Video", "StartMaximized", false) ? 1
                                                                                         : 0;
                if (wanted != appliedMode)
                {
                    ps2_host_display::applyMode(wanted);
                    appliedMode = wanted;
                    sizeWindow(wanted);
                }
                if (!g_capture.active && g_hotkeyCapture < 0 && hk::pressed(hk::Action::Fullscreen))
                {
                    appliedMode = ps2_host_display::toggleFullscreen();
                    state.ini.setBool("Video", "StartFullscreen", appliedMode == 2);
                    state.ini.setBool("Video", "StartMaximized", appliedMode == 1);
                    sizeWindow(appliedMode);
                }
            }
            state.unit = std::clamp(static_cast<float>(GetScreenHeight()) / 900.0f, 0.55f, 3.0f);
            feedGamepads(io);
            updateMotion(state, state.screen);
            state.logoThisFrame = false;
            state.soundThisFrame = false;
            state.penThisFrame = false;
            g_tickItems.clear();
            g_ownSoundItems.clear();
            g_silentItems.clear();
            if (state.playTime >= 0.0f)
            {
                if (state.playTime == 0.0f)
                    playBark(state);
                state.playTime += std::min(GetFrameTime(), 0.05f);
                if (state.playTime >= kPlayLength)
                {
                    state.result = Result::Play;
                    state.done = true;
                }
            }
            if (state.hasMusic && !state.musicHandedOff)
            {
                const float fade = state.playTime >= 0.0f ? std::clamp(state.playTime / (kPlayLength - 0.02f), 0.0f, 1.0f) : 0.0f;
                SetMusicVolume(state.music, kMusicVolume * userVolume(state) * (1.0f - fade));
                UpdateMusicStream(state.music);
            }
            if (state.hasAmbience) // out with the music on Play; silent without the saloon
            {
                const float fade = state.playTime >= 0.0f ? std::clamp(state.playTime / (kPlayLength - 0.02f), 0.0f, 1.0f) : 0.0f;
                SetMusicVolume(state.ambience, state.saloonOn ? kEffectGain * state.scene.tuning().ambience * userVolume(state) * (1.0f - fade) : 0.0f);
                UpdateMusicStream(state.ambience);
            }
            if (state.voiceAt >= 0.0f && GetTime() >= state.voiceAt)
            {
                state.voiceAt = -1.0f;
                if (state.screen == Screen::Home && state.playTime < 0.0f)
                    snackVoice(state);
            }
            if (state.saloonOn && state.scene.failed())
            {
                KM_WARN(Boot, "launcher: the saloon failed to load, plain launcher");
                state.scene.close();
                state.saloonOn = false;
            }
            if (state.saloonOn)
            {
                const float dt = std::min(GetFrameTime(), 0.1f);
                saloon::Scene::Frame frame;
                frame.dt = dt;
                frame.parallax = Vector2{state.parallax.x, state.parallax.y};
                frame.volume = userVolume(state); // the curry's steam (muted: cold curry)
                frame.bookLit = state.screen == Screen::Home && state.bill.focus == static_cast<int>(kBillOptions);
                if (!state.posterPhotosSet && state.installed && state.scene.ready())
                {
                    // the wanted posters on the wall show the presets' photos from the player's disc (K, Nov 18)
                    readPresetPhotos(state);
                    state.scene.setPosterPhotos(state.presetPhotos, presets::kCount);
                    state.posterPhotosSet = true;
                }
                state.scene.update(frame);
                state.bill.focusAge += dt;
                state.bookFocusAge += dt;
                if (state.zoomPending && state.screen == Screen::Home && (state.scene.zoomReady() || state.scene.zoomBroken()))
                {
                    state.zoomPending = false;
                    openOptions(state, false); // (its sound played at the press)
                }
                // The zoom landed: Options' panel fades in over its last frame. Back: it fades out, then the zoom
                // plays backwards (the bill's loop back on Options).
                if (state.screen == Screen::Home && state.scene.landed())
                {
                    state.screen = Screen::Options;
                    state.focusFirst = true;
                    state.optionsShown = 0.0f;
                    state.optionsLeaving = false;
                }
                if (state.screen == Screen::Options)
                {
                    const float target = state.optionsLeaving ? 0.0f : 1.0f;
                    state.optionsShown += std::clamp(target - state.optionsShown, -dt / 0.12f, dt / 0.15f);
                    if (state.optionsLeaving && state.optionsShown <= 0.0f)
                    {
                        state.optionsLeaving = false;
                        state.screen = Screen::Home;
                        state.focusOptions = true;
                        state.scene.zoomOut();
                    }
                }
            }
            const bool playing = state.playTime >= 0.0f;
            // The newspaper slides up when opened and back down when closed (0.45 s); away from Home it is put away.
            if (state.screen != Screen::Home)
                state.paperOpen = false;
            {
                const float step = std::min(GetFrameTime(), 0.1f) / 0.45f;
                state.paperT = state.paperOpen ? std::min(1.0f, state.paperT + step) : std::max(0.0f, state.paperT - step);
                if (state.screen != Screen::Home)
                    state.paperT = 0.0f;
            }
            BeginDrawing();
            if (state.saloonOn)
            {
                // Install and the first run's posters over Home darkened about halfway (K); Options without its zoom too.
                const float dim = state.screen == Screen::Install || state.screen == Screen::FirstRun                ? 0.5f
                                  : state.screen == Screen::Options && state.scene.zoomFrame() == 0 ? 0.65f
                                                                                                         : 0.0f;
                state.scene.draw(dim);
                // The logo in the doorway's glare (not on Options or the posters), riding the zoom's frames.
                Rectangle at{};
                const bool shown = state.logo.id != 0u && state.screen != Screen::Options && state.screen != Screen::FirstRun &&
                                   state.scene.ready() &&
                                   state.scene.logo(kLogoContent, static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width), at);
                state.logoPlaced = shown;
                state.logoThisFrame = shown;
                if (shown)
                {
                    state.logoMin = ImVec2(at.x, at.y);
                    state.logoMax = ImVec2(at.x + at.width, at.y + at.height);
                }
            }
            else
                drawBackground();
            drawBurst(state);
            if (state.saloonOn)
                drawSaloonInk(state);
            // A popup (Add to Steam's) must sit over the logo: while one is open on Home the logo goes under ImGui
            // (only on Home: the other screens draw no logo).
            const bool logoUnder = state.popupWasOpen && !playing && state.screen == Screen::Home;
            if (logoUnder)
            {
                drawLogo(state, true);
                if (!state.saloonOn)
                    drawSplash(state);
            }
            // Options in the open book (its last zoom frame): ImGui laid out in the book's own pixels, drawn onto its pages
            book::Space bookSpace;
            if (state.saloonOn && state.screen == Screen::Options && !playing)
            {
                saloon::Quad left, right;
                if (state.scene.spread(left, right))
                    bookSpace = book::spaceFrom(left, right);
            }
            if (bookSpace.valid)
                book::beginFrame(bookSpace);
            else
                rlImGuiBegin();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            if (bookSpace.valid) // no frame round the window in the book (it would land on the table)
                ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.0f);
            const bool launcherWindow = ImGui::Begin("##launcher", nullptr,
                                                     ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                                         ImGuiWindowFlags_NoBringToFrontOnFocus | (bookSpace.valid ? ImGuiWindowFlags_NoBackground : 0));
            if (bookSpace.valid)
                ImGui::PopStyleVar();
            if (launcherWindow)
            {
                // While Play's animation runs nothing takes input (and nothing greys out: it is fading anyway).
                if (playing)
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
                    ImGui::BeginDisabled();
                }
                state.slipFocus = kSlipNone;
                switch (state.screen)
                {
                case Screen::Home:
                    drawHome(state);
                    break;
                case Screen::Install:
                    drawInstall(state);
                    break;
                case Screen::Options:
                    if (state.saloonOn) // over the zoom's last frame, fading in once it lands and out on Back
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(state.optionsShown, 0.0f, 1.0f));
                        const bool leaving = state.optionsLeaving; // Back sets it inside
                        if (leaving)
                            ImGui::BeginDisabled();
                        if (bookSpace.valid)
                            drawBook(state, bookSpace);
                        else
                            drawOptions(state);
                        if (leaving)
                            ImGui::EndDisabled();
                        ImGui::PopStyleVar();
                    }
                    else
                        drawOptions(state);
                    break;
                case Screen::FirstRun:
                    drawFirstRun(state);
                    break;
                }
                if (state.screen != Screen::Options)
                    drawVolume(state);
                if (state.screen == Screen::Home)
                    drawUpdate(state);
#if KAMITE_LAUNCHER_STEAM
                if (state.screen == Screen::Home)
                    drawSteam(state);
#endif
                if (playing)
                {
                    ImGui::EndDisabled();
                    ImGui::PopStyleVar();
                }
            }
            ImGui::End();
            if (state.slipFocus != state.lastSlipFocus && state.slipFocus != kSlipNone)
                penSound(state); // the slip's loop
            state.lastSlipFocus = state.slipFocus;
            frameSounds(state);
            if (bookSpace.valid)
                book::endFrame(bookSpace);
            else
                rlImGuiEnd();
            if (!logoUnder)
            {
                drawLogo(state);
                if (!state.saloonOn)
                    drawSplash(state);
            }
            drawPosters(state);
            if (state.paperOn && state.paperT > 0.0f)
                state.paper.draw(paperContent(state), state.paperT, 1.0f); // sliding up or open: over the logo too
            if (playing)
            {
                DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                              Color{0, 0, 0, alphaOf(state.playTime / (kPlayLength - 0.02f))});
                if (state.logoThisFrame)
                    drawPlate(state, state.playTime);
            }
            EndDrawing();
        }
        state.ini.save();
        saveControls();
        if (state.logo.id != 0u)
            UnloadTexture(state.logo);
        for (Texture2D *texture : {&state.kana, &state.rearmed, &state.rearmedText, &state.rearmedGlow})
            if (texture->id != 0u)
                UnloadTexture(*texture);
        if (state.burst.id != 0u)
            UnloadTexture(state.burst);
        for (Texture2D &photo : state.presetPhotos)
            if (photo.id != 0u)
                UnloadTexture(photo);
        for (int n = 0; n < presets::kCount; ++n)
            for (Texture2D *texture : {&state.poster[n], &state.posterTape[n], &state.posterShadow[n]})
                if (texture->id != 0u)
                    UnloadTexture(*texture);
        if (state.stamp.id != 0u)
            UnloadTexture(state.stamp);
        for (Texture2D &prompt : state.prompt)
            if (prompt.id != 0u)
                UnloadTexture(prompt);
        for (auto &[file, art] : state.promptArt)
            if (art.id != 0u)
                UnloadTexture(art);
        saloon::unloadBillFonts(state.billFonts);
        state.paper.unload();
        if (state.bill.steamMark.id != 0u)
            UnloadTexture(state.bill.steamMark);
        state.scene.close(); // its threads and textures, before the game has the window
        if (state.splashFont.texture.id != 0u)
            UnloadFont(state.splashFont);
        if (state.barkWorker.joinable())
            state.barkWorker.join();
#if KAMITE_LAUNCHER_STEAM
        if (state.steamWorker.joinable())
            state.steamWorker.join();
#endif
        if (state.hasMusic)
            UnloadMusicStream(state.music);
        if (state.hasAmbience) // (faded out by Play; stopped before the device goes to the game, or it would repeat)
        {
            StopMusicStream(state.ambience);
            UnloadMusicStream(state.ambience);
        }
        // A check still running is left to finish on its own (the game's start does not wait for GitHub).
        if (g_updateCheck.worker.joinable())
            g_updateCheck.worker.detach();
        if (g_updater.worker.joinable())
            g_updater.worker.detach();
        if (state.result == Result::Play && state.audio)
        {
            // The game takes the audio device over (the runtime opens it only when it is not open yet), so Play's
            // sound rings on while the game boots; the sound's few samples are left to the device until then.
            KM_INFO(Boot, "launcher: audio device handed to the game");
        }
        else
        {
            for (int i = 0; i < 4; ++i)
                if (state.hasMenuSound[i])
                    UnloadSound(state.menuSounds[i]);
            for (Sound &voice : state.snackVoices)
                UnloadSound(voice);
            for (const auto &[loaded, sound] : {std::pair{state.hasPen, &state.pen}, std::pair{state.hasSlurp, &state.slurp},
                                                std::pair{state.hasBite, &state.bite}, std::pair{state.hasBookOpen, &state.bookOpen},
                                                std::pair{state.hasBookClose, &state.bookClose}, std::pair{state.hasRustle, &state.rustle}})
                if (loaded)
                    UnloadSound(*sound);
            if (state.audio)
                CloseAudioDevice();
        }
        rlImGuiShutdown(); // the F12 debugger sets ImGui up again later
        // One empty frame, so the window does not keep showing the launcher until the game's first picture.
        BeginDrawing();
        ClearBackground(BLACK);
        EndDrawing();
        KM_INFO(Boot, "launcher: " << (state.result == Result::Play ? "play" : "quit"));
        return state.result;
    }
}
