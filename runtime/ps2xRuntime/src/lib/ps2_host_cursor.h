#pragma once

#include <string>

// The window's mouse pointer as an image (God Hand: Rearmed: K's hand, resources/images/cursor_hand.png; K, Nov 28):
// a GLFW cursor made from the picture, so it is the OS pointer itself (no lag, the launcher and every screen of the
// game). Sized to a share of the window's height, made again when that changes. Where nothing sets an image, the OS
// arrow stays.
namespace ps2_host_cursor
{
    // Any thread: the image (a PNG), its hot spot as fractions of its width / height, and its height as a share of the
    // window's. Loaded on the window thread at the next apply().
    void setImage(const std::string &path, float hotU, float hotV, float heightShare);
    // Window thread, every frame (the game's input sample, the launcher's frame): sets the pointer on the window (the
    // window with the current GL context: raylib's). ours = false leaves the frame's own shape (the launcher's ImGui
    // text and resize pointers).
    void apply(bool ours = true);
}
