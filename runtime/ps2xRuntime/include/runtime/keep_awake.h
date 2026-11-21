#pragma once

// Keeping the display awake on Linux while the game runs (issue #12; Windows: Kernel/host_window_win32.cpp,
// SetThreadExecutionState). A controller's input doesn't count as the user being active, so a desktop's "blank the
// screen after N minutes" turned the monitor off mid-game. Two things handled here blank a Linux screen:
//
//   - the desktop (KDE, so the Steam Deck's Desktop Mode, GNOME, and others with either service): asked over the
//     session D-Bus not to,
//     org.freedesktop.ScreenSaver.Inhibit first (the freedesktop interface, KDE's and most others'), then GNOME's
//     org.gnome.SessionManager.Inhibit with the "idle" flag. The inhibit holds while our D-Bus connection is open: the
//     connection is kept for the life of the process, and the desktop drops the inhibit when the process ends, even
//     after a crash;
//   - the X server's own screensaver timer (a bare window manager, i3 and the like, sets none of its own; the X server
//     restarts its DPMS timer from the same reset, which the tests don't check):
//     XResetScreenSaver every 30 s, as SDL does, on a connection of our own.
//
// libdbus-1 and libX11 are opened at run time (dlopen): nothing is linked or shipped for this, and a system without one
// just skips that part (with a log line). The Steam Deck's Game Mode needs neither: Steam keeps the screen on while a
// game runs.
//
// Not covered: a Wayland compositor with neither D-Bus service (sway, river, niri with swayidle and the like). The game
// window is X11 (GLFW is put on X11 for the renderer: gs_pcsx2_backend.cpp), so it runs under Xwayland there, and
// neither XResetScreenSaver on Xwayland nor any D-Bus call reaches the compositor's idle timer; Wayland's idle-inhibit
// protocol needs a Wayland surface of the game's own. There the screen can still blank (swayidle's own inhibit rules,
// e.g. for fullscreen windows, apply).
#include <cstdint>
#include <string>
#include <vector>

namespace ps2x::keepawake
{
    enum class Inhibitor
    {
        None,
        FreedesktopScreenSaver, // org.freedesktop.ScreenSaver /org/freedesktop/ScreenSaver Inhibit(s app, s reason) -> u
        GnomeSessionManager,    // org.gnome.SessionManager /org/gnome/SessionManager Inhibit(s, u xid, s, u flags) -> u
    };
    const char *name(Inhibitor inhibitor);

    // GNOME's flag for "don't mark the session idle" (gnome-session's GsmInhibitorFlag: 8), which is what blanks it.
    constexpr uint32_t kGnomeInhibitIdle = 8u;

    // One method call on the session bus whose reply is a uint32 (the inhibit's cookie).
    struct Call
    {
        Inhibitor inhibitor = Inhibitor::None;
        const char *service = nullptr, *path = nullptr, *interface = nullptr, *method = nullptr;
    };
    // The calls tried, in order.
    std::vector<Call> calls();

    // The session bus, as inhibitIdle needs it: the real one (libdbus, below) or a test's.
    struct Bus
    {
        virtual ~Bus() = default;
        // Sends the call with GHR's arguments (app, reason; GNOME's also a toplevel XID of 0 and kGnomeInhibitIdle).
        // True with the reply's cookie, else false with the error (the D-Bus error's name and message).
        virtual bool call(const Call &call, const char *app, const char *reason, uint32_t &cookie,
                          std::string &error) = 0;
    };

    struct Result
    {
        Inhibitor by = Inhibitor::None;
        uint32_t cookie = 0;
        std::string error; // every failed call's error, "; " between them (when by == None: why nothing held)
    };
    // The first call in calls() the bus answers; None with each error when none does.
    Result inhibitIdle(Bus &bus, const char *app, const char *reason);

    // What GHR tells the desktop (shown in KDE's battery applet, GNOME's inhibitor list).
    constexpr const char *kApp = "God Hand: Rearmed";
    constexpr const char *kReason = "Playing (a controller doesn't count as activity)";

    // The real session bus through libdbus-1.so.3, or nullptr (and `error`) when the library or the bus is missing.
    // No bus is $DBUS_SESSION_BUS_ADDRESS unset and no $XDG_RUNTIME_DIR/bus socket: libdbus would otherwise start a
    // dbus-launch of its own (X11 autolaunch). Never closed: an inhibit lasts while the connection does. The
    // connection is never dispatched, so libdbus's exit-on-disconnect (an _exit() when it handles the Disconnected
    // message) can't happen today; it is turned off anyway, in case a later change dispatches it.
    Bus *openSessionBus(std::string &error);

    // The game's X11 reset period (SDL resets as often).
    constexpr int kX11ResetPeriodMs = 30000;

    // XResetScreenSaver on $DISPLAY now and every periodMs (a thread of its own, its own X connection, for the life of
    // the process); once per process, later calls answering as the first did. False (and `error`) when libX11 or the
    // display is missing. Losing the connection ends the thread, not the game, where libX11 has
    // XSetIOErrorExitHandler (1.7, 2020); tested on libX11 1.8.14 only (the Nix check's). 1.8.x fixed what happens
    // after an exit handler returns (writes on the dead connection); on 1.7.x (Debian 11, the Steam Runtime's sniper)
    // that path is untested, though this thread makes no call after its connection is found gone. An older libX11's
    // exit() stays (the game's own window is gone with that X server anyway).
    bool startX11Reset(int periodMs, std::string &error);
}

// Once, from the game window's thread (ps2_runtime.cpp): the X11 reset, then the D-Bus inhibit, on a background thread
// so a slow or hung session bus never holds up the window, nor the X11 reset (a hung bus holds only that thread).
// Linux: this file's implementation (Kernel/host_keep_awake_linux.cpp); Windows: host_window_win32.cpp.
void ps2xHostKeepDisplayAwake() noexcept;
