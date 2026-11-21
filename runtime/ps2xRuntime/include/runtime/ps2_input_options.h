#pragma once

#include <cstdint>
#include <string>
#include <utility>
#include <vector>

namespace ps2_host_input
{
    constexpr uint32_t kInvertRightX = 1u;
    constexpr uint32_t kInvertRightY = 2u;

    struct MouseCameraInput
    {
        float x = 0.0f;
        float y = 0.0f;
        bool active = false;
    };

    void setRightStickInvertDefault(uint32_t axes);
    // The sticks' deadzone for every controller, in percent of the stick's reach (0 = none; stick drift): stick 0 the
    // left, 1 the right. Until set, PS2X_LEFT_STICK_DEADZONE / PS2X_RIGHT_STICK_DEADZONE (GHR.ini [Game]
    // LeftStickDeadzone / RightStickDeadzone). A controls.ini [PadN] Deadzone above it wins. Any thread.
    void setStickDeadzone(int stick, int percent);
    // The controller's sticks swapped (left-handed play; K, Nov 21): the right stick does the left's job and the left
    // the right's, in play and menus. Keys and the mouse keep theirs; the deadzones stay with the stick they name; the
    // stick presses are the Button Mapping page's. Until set, PS2X_SWAP_STICKS (GHR.ini [Game] SwapSticks). Any thread.
    void setSwapSticks(bool on);
    void setMouseCameraEnabled(bool enabled);
    void setMouseStickEnabled(bool enabled);
    [[nodiscard]] MouseCameraInput consumeMouseCameraInput();

    // Controller pad (0-based) settings when controls.ini has no [Pad<pad+1>] section: PCSX2's [PadN] keys in file
    // order, a key repeated once per binding. urban.exe writes controls.ini with them.
    [[nodiscard]] std::vector<std::pair<std::string, std::string>> defaultPadSettings(uint32_t pad);

    // The controls file ([Pad1]-[Pad8] in PCSX2's syntax) the Controls screen (F1) edits; call before the run loop.
    // Missing: written from `legacy` (the PS2X_PAD<n>=1 / PS2X_PAD<n>_<KEY> settings an older GHR.ini carried),
    // else from the defaults. Reloaded while the game runs when it changes. A pad with PS2X_PAD<n> in the environment
    // keeps using those variables. An empty path: no file (the old behaviour).
    void setControlsFile(const std::string &path, const std::vector<std::pair<std::string, std::string>> &legacy);
}
