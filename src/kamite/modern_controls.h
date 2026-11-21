#pragma once

#include <cstdint>

// Control Style (modern_controls.cpp): Classic, or Modern (a free camera on the right stick): Toggle (R3 switches to
// the original controls and back, the choice kept across rooms; with Auto contact switches to them; L1 is always the
// game's quick turn) or, the controller only, Hold (the original controls while L1 is held, a double tap the quick
// turn; Auto's brawl mode ends with the fight). The input side runs on the window thread (a pad filter), the camera side in the camera hooks
// (camera.cpp, game thread); they meet in the atomics behind these calls.
namespace kamite::modern
{
    // [Game] ControlStyle, ModernAuto (and the camera speeds), read at install; registers the pad filter
    // and the F2 menu's Controls tab.
    void install();

    // Camera side (game thread, once per player camera update). battle: the battle (or a boss) preset camera is on
    // (+0x51C > 0); gameCamera: the game has taken the camera (a close-up preset, the second update path).
    void noteCamera(const uint8_t *rdram, bool battle, bool gameCamera);

    // The free camera: false = the original camera (behind Gene). While free, yaw is the camera's direction round
    // Gene in world radians and pitch the extra pitch to add. Advances the camera by the right stick (dt seconds),
    // and turns Gene toward the left stick's direction (rdram: the player object) while he walks or stands.
    // gameYaw: where the original camera would be this frame, which the free camera eases back to while the original
    // controls are in effect; geneYaw: Gene's own yaw.
    struct FreeCamera
    {
        bool free = false;
        float yaw = 0.0f;
        float pitch = 0.0f;
    };
    FreeCamera updateCamera(uint8_t *rdram, float gameYaw, float geneYaw, float dt);
    // The lowest extra pitch that keeps the camera above the ground at its current distance (camera.cpp, every update).
    void setPitchFloor(float floor);
    // setCamUpdate (a cut, a new room): back behind Gene.
    void resetCamera();

    // Modern is the style of the device used last (whatever is in effect now).
    bool enabled();
    // Modern's Auto is on (F2 or [Game] ModernAuto). Any thread.
    bool autoEnabled();
    // The in-game OPTIONS (any thread): Control Style, Modern: Auto and Modern: Mouse Camera, as F2 > Controls sets them.
    void setModern(bool on);
    // The controller's Modern is Hold ([Game] ControlStyle=hold; setModern(true) too). Any thread.
    void setHold(bool on);
    // Hold's mode: Hold to Cam ([Game] HoldMode=cam: brawl mode unless L1 is held) or Hold to Brawl. Any thread.
    void setHoldCam(bool on);
    // Modern: Hold is in effect now (the controller is the device in use). Any thread.
    bool holdEnabled();
    // ... and it is Hold to Cam.
    bool holdCamEnabled();
    void setAuto(bool on);
    void setMouseCamera(bool on);
    // The free camera's up / down, stick and mouse ([Game] CameraInvertY; the Options' Invert Camera Y-Axis).
    void setInvertY(bool on);
    // The keyboard & mouse's own Control Style and Auto ([Game] KbmControlStyle / KbmModernAuto; K, Nov 16). enabled()
    // and autoEnabled() answer for the device used last.
    void setKbmModern(bool on);
    void setKbmAuto(bool on);
    // What a test case records as KAMITE_CONTROL_STYLE / KAMITE_MODERN_AUTO: the style in effect when it starts.
    struct CaseStyle
    {
        bool modern = false;
        bool autoOn = false;
        bool hold = false; // the controller's Modern: Hold (recorded as KAMITE_CONTROL_STYLE=hold)
        bool holdCam = false; // ... Hold to Cam (KAMITE_HOLD_MODE=cam)
    };
    CaseStyle caseStyle();
    // A hit between Gene and an enemy landed (either way; game thread, the damage steps): with Auto, brawl mode.
    void noteContact();

    // Modern is on and the free camera is in effect now (not the original controls by R3 / L1 / Auto, and the game not
    // holding the camera). For the radar's glow (radar_glow.h). Any thread.
    bool freeCameraNow();
}
