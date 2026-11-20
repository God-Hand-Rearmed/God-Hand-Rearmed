#pragma once

// The launcher's operating-system pieces (kept out of launcher.cpp: <windows.h> and raylib clash).
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kamite::launcher::platform
{
    // A file (folder = false: disc images) or folder picker over the window. Empty if cancelled or not available.
    // Linux: zenity, else kdialog (separate programs, run without a shell); neither installed: empty.
    std::filesystem::path pick(void *window, bool folder);
    // A mod's zip (Install from zip on the Mods tab). Empty if cancelled or not available.
    std::filesystem::path pickZip(void *window);
    // Opens a file or folder with the system's handler (the controls file in a text editor). Linux: xdg-open.
    void open(const std::filesystem::path &path);
    // Shift held right now (the launcher shows even when hidden). Linux: only once the window is open (X11 and
    // Wayland give a program the keyboard through its window); false before.
    bool shiftHeld();
    // An HTTPS GET into `out` (redirects followed): true on status 200 with at most maxBytes. `received` (if given)
    // counts the bytes as they come. Blocks: run it on a thread. False on platforms without it. Linux: the system's
    // libcurl (libcurl.so.4, else libcurl-gnutls.so.4) opened at run time; without one, false.
    bool httpGet(const std::string &url, std::vector<uint8_t> &out, size_t maxBytes, const char *accept = nullptr,
                 std::atomic<uint64_t> *received = nullptr);
    // Starts the program again (the given executable, from its folder, no arguments); the caller then quits.
    bool relaunch(const std::filesystem::path &exe);
    // The running program's own executable. Linux: the AppImage file ($APPIMAGE) when it runs as one (its mounted
    // copy is gone once it quits), else /proc/self/exe.
    std::filesystem::path thisExe();
#if !defined(_WIN32)
    // Starts `program` (looked up in PATH unless it holds a '/') with `args`, detached, without a shell. False when
    // it is not installed or could not start.
    bool spawn(const std::string &program, const std::vector<std::string> &args);
#endif
}
