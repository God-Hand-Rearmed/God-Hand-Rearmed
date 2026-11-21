// Keeping the display awake on Linux (runtime/keep_awake.h, Kernel/host_keep_awake_linux.cpp; issue #12).
//
//   keep_awake_test            the calls' order and fallback, on a fake bus (no D-Bus, no display)
//   keep_awake_test bus        the real path: openSessionBus + inhibitIdle on $DBUS_SESSION_BUS_ADDRESS; prints
//                              "held <service> <cookie>", "none <errors>" or "nobus <error>", then waits for a line on
//                              stdin ("again": asks once more, printing "again <service>") and prints "alive" (test/keep_awake_bus.py
//                              runs it against fake desktop services, kills the bus in between, and checks the game
//                              would go on)
//   keep_awake_test entry      ps2xHostKeepDisplayAwake() twice, as the game calls it, then 2 s for its thread: the
//                              driver checks the desktop was asked once, with kApp and kReason, and the log lines
//   keep_awake_test missing-python   what ctest runs when CMake found no python3 for keep_awake_bus.py: skipped (77),
//                              or a failure under KAMITE_REQUIRE_ALL_TESTS=1
//   keep_awake_test x11        on $DISPLAY: the X server's idle time (MIT-SCREEN-SAVER, libXss) goes back to ~0 once
//                              startX11Reset runs, and again after each period; exit 77 (skipped) without a display or
//                              libXss (1 instead when KAMITE_REQUIRE_ALL_TESTS=1)
//   keep_awake_test wayland [s]  startWaylandInhibit on $WAYLAND_DISPLAY: "held" or "failed <why>", then [s] seconds
//                              holding it (test/keep_awake_wayland.sh: a headless sway, swayidle judging)
#include "runtime/keep_awake.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <dlfcn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <unistd.h>
#include <iostream>
#include <string>
#include <thread>

namespace
{
    int g_checks = 0, g_failures = 0;
#define CHECK(cond)                                                                    \
    do                                                                                 \
    {                                                                                  \
        ++g_checks;                                                                    \
        if (!(cond))                                                                   \
        {                                                                              \
            ++g_failures;                                                              \
            std::fprintf(stderr, "%s:%d: CHECK failed: %s\n", __FILE__, __LINE__, #cond); \
        }                                                                              \
    } while (0)

    using namespace ps2x::keepawake;

    struct FakeBus final : Bus
    {
        // Which inhibitors answer, and with what.
        bool freedesktop = false, gnome = false;
        std::vector<Inhibitor> asked;
        bool call(const Call &call, const char *app, const char *reason, uint32_t &cookie, std::string &error) override
        {
            asked.push_back(call.inhibitor);
            CHECK(std::strcmp(app, "GHR") == 0);
            CHECK(std::strcmp(reason, "why") == 0);
            CHECK(std::strcmp(call.method, "Inhibit") == 0);
            const bool answers = call.inhibitor == Inhibitor::FreedesktopScreenSaver ? freedesktop : gnome;
            if (!answers)
            {
                error = std::string("org.freedesktop.DBus.Error.ServiceUnknown (") + call.service + ")";
                return false;
            }
            cookie = call.inhibitor == Inhibitor::FreedesktopScreenSaver ? 11u : 22u;
            return true;
        }
    };

    void testOrder()
    {
        const std::vector<Call> list = calls();
        CHECK(list.size() == 2u);
        CHECK(list[0].inhibitor == Inhibitor::FreedesktopScreenSaver);
        CHECK(std::strcmp(list[0].service, "org.freedesktop.ScreenSaver") == 0);
        CHECK(std::strcmp(list[0].path, "/org/freedesktop/ScreenSaver") == 0);
        CHECK(std::strcmp(list[0].interface, "org.freedesktop.ScreenSaver") == 0);
        CHECK(list[1].inhibitor == Inhibitor::GnomeSessionManager);
        CHECK(std::strcmp(list[1].service, "org.gnome.SessionManager") == 0);
        CHECK(std::strcmp(list[1].path, "/org/gnome/SessionManager") == 0);
        CHECK(kGnomeInhibitIdle == 8u);
        CHECK(kX11ResetPeriodMs == 30000); // what the CHANGELOG says
    }

    // startX11Reset with no display: false and why, and a second call answers as the first did (once per process).
    void testX11ResetOnce()
    {
        ::unsetenv("DISPLAY");
        std::string first, second;
        CHECK(!startX11Reset(1000, first));
        CHECK(!first.empty());
        CHECK(!startX11Reset(1000, second));
        CHECK(second == first);

        FakeBus both;
        both.freedesktop = both.gnome = true;
        Result r = inhibitIdle(both, "GHR", "why");
        CHECK(r.by == Inhibitor::FreedesktopScreenSaver && r.cookie == 11u && r.error.empty());
        CHECK(both.asked.size() == 1u); // the first that holds ends it: never two inhibits

        FakeBus gnomeOnly;
        gnomeOnly.gnome = true;
        r = inhibitIdle(gnomeOnly, "GHR", "why");
        CHECK(r.by == Inhibitor::GnomeSessionManager && r.cookie == 22u);
        CHECK(gnomeOnly.asked.size() == 2u);

        FakeBus none;
        r = inhibitIdle(none, "GHR", "why");
        CHECK(r.by == Inhibitor::None && r.cookie == 0u);
        CHECK(r.error.find("org.freedesktop.ScreenSaver: org.freedesktop.DBus.Error.ServiceUnknown") != std::string::npos);
        CHECK(r.error.find("; org.gnome.SessionManager: ") != std::string::npos);
        CHECK(std::strcmp(name(Inhibitor::None), "none") == 0);
    }

    // startWaylandInhibit with no compositor named: false and why, without trying libwayland's "wayland-0" default,
    // and a second call answers as the first did.
    void testWaylandNone()
    {
        ::unsetenv("WAYLAND_DISPLAY");
        ::unsetenv("WAYLAND_SOCKET");
        std::string first, second;
        CHECK(!startWaylandInhibit(first));
        CHECK(first == "no Wayland compositor ($WAYLAND_DISPLAY unset)");
        ::setenv("WAYLAND_DISPLAY", "wayland-ghr-none", 1);
        CHECK(!startWaylandInhibit(second));
        CHECK(second == first); // once per process: not tried again
        CHECK(std::strcmp(kWaylandNamespace, "ghr-keep-awake") == 0); // what keep_awake_wayland.sh looks for
    }

    // "wayland [seconds]": startWaylandInhibit on $WAYLAND_DISPLAY, "held" or "failed <why>", then [seconds] (default 4)
    // holding it, then "alive" (test/keep_awake_wayland.sh runs it under a headless sway with swayidle watching).
    int runWayland(int seconds)
    {
        std::string error;
        if (!startWaylandInhibit(error))
        {
            std::printf("failed %s\n", error.c_str());
            return 1;
        }
        std::printf("held\n");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(seconds));
        std::printf("alive\n");
        return 0;
    }

    int runBus()
    {
        std::string error;
        Bus *bus = openSessionBus(error);
        if (!bus)
            std::printf("nobus %s\n", error.c_str());
        else
        {
            const Result held = inhibitIdle(*bus, "God Hand: Rearmed", "Playing");
            if (held.by == Inhibitor::None)
                std::printf("none %s\n", held.error.c_str());
            else
                std::printf("held %s %u\n", name(held.by), held.cookie);
        }
        std::fflush(stdout);
        std::string line;
        std::getline(std::cin, line); // the driver may kill the bus meanwhile: libdbus must not end the process
        if (bus && line == "again")
        {
            // A call on a dead bus must come back (an error, "again none"), not hang or crash. GHR makes no second
            // call today.
            const Result again = inhibitIdle(*bus, "God Hand: Rearmed", "Playing");
            std::printf("again %s\n", name(again.by));
        }
        std::printf("alive\n");
        return 0;
    }

    // 77 (ctest: skipped) without the display or libraries, unless KAMITE_REQUIRE_ALL_TESTS=1: then a failure.
    int skipped()
    {
        const char *require = std::getenv("KAMITE_REQUIRE_ALL_TESTS");
        return require && std::strcmp(require, "1") == 0 ? 1 : 77;
    }

    // The X server's idle time in ms (MIT-SCREEN-SAVER through libXss), or -1 (and a skip) when it can't be read.
    long idleNow()
    {
        struct Display;
        struct Info // XScreenSaverInfo (scrnsaver.h)
        {
            unsigned long window;
            int state, kind;
            unsigned long til_or_since, idle, eventMask;
        };
        static Display *display = nullptr;
        static Info *info = nullptr;
        static int (*query)(Display *, unsigned long, Info *) = nullptr;
        static unsigned long (*root)(Display *) = nullptr;
        if (!display)
        {
            void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
            void *xss = x11 ? dlopen("libXss.so.1", RTLD_NOW | RTLD_LOCAL) : nullptr;
            if (!xss)
                return -1;
            auto open = reinterpret_cast<Display *(*)(const char *)>(dlsym(x11, "XOpenDisplay"));
            root = reinterpret_cast<unsigned long (*)(Display *)>(dlsym(x11, "XDefaultRootWindow"));
            auto alloc = reinterpret_cast<Info *(*)()>(dlsym(xss, "XScreenSaverAllocInfo"));
            query = reinterpret_cast<int (*)(Display *, unsigned long, Info *)>(dlsym(xss, "XScreenSaverQueryInfo"));
            display = open && root && alloc && query ? open(nullptr) : nullptr;
            if (!display)
                return -1;
            info = alloc();
        }
        query(display, root(display), info);
        return static_cast<long>(info->idle);
    }

    int runEntryX11()
    {
        if (idleNow() < 0)
        {
            std::fprintf(stderr, "skipped: no X display or no libXss\n");
            return skipped();
        }
        if (!dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL)) // without it nothing could hang: no test of the order
        {
            std::fprintf(stderr, "skipped: libdbus-1.so.3 missing\n");
            return skipped();
        }
        // A session bus that takes the connection and never answers (a listening socket nobody accepts on): libdbus's
        // connect waits on it without end, and the X11 reset must not wait with it.
        const char *tmp = std::getenv("TMPDIR");
        const std::string hung = std::string(tmp && *tmp ? tmp : "/tmp") + "/ghr-keep-awake-hung-" +
                                 std::to_string(::getpid());
        const int listener = ::socket(AF_UNIX, SOCK_STREAM, 0);
        sockaddr_un where{};
        where.sun_family = AF_UNIX;
        std::snprintf(where.sun_path, sizeof(where.sun_path), "%s", hung.c_str());
        ::unlink(hung.c_str());
        CHECK(listener >= 0 && ::bind(listener, reinterpret_cast<sockaddr *>(&where), sizeof(where)) == 0 &&
              ::listen(listener, 4) == 0);
        ::setenv("DBUS_SESSION_BUS_ADDRESS", ("unix:path=" + hung).c_str(), 1);
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const long before = idleNow();
        CHECK(before >= 1200);
        ps2xHostKeepDisplayAwake();
        long after = before;
        for (int i = 0; i < 30 && after >= 600; ++i) // the background thread: no bus here, so at once
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            after = idleNow();
        }
        CHECK(after < 600);
        std::fprintf(stderr, "entry-x11 (session bus hung): idle %ld ms before, %ld ms after\n", before, after);
        ::unlink(hung.c_str());
        return 0;
    }

    int runX11()
    {
        struct Display;
        struct Info // XScreenSaverInfo (scrnsaver.h)
        {
            unsigned long window;
            int state, kind;
            unsigned long til_or_since, idle, eventMask;
        };
        void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_GLOBAL);
        void *xss = x11 ? dlopen("libXss.so.1", RTLD_NOW | RTLD_LOCAL) : nullptr;
        if (!x11 || !xss)
        {
            const char *why = dlerror();
            std::fprintf(stderr, "skipped: libX11.so.6 or libXss.so.1 did not load (%s)\n", why ? why : "?");
            return skipped();
        }
        auto open = reinterpret_cast<Display *(*)(const char *)>(dlsym(x11, "XOpenDisplay"));
        auto root = reinterpret_cast<unsigned long (*)(Display *)>(dlsym(x11, "XDefaultRootWindow"));
        auto alloc = reinterpret_cast<Info *(*)()>(dlsym(xss, "XScreenSaverAllocInfo"));
        auto query = reinterpret_cast<int (*)(Display *, unsigned long, Info *)>(dlsym(xss, "XScreenSaverQueryInfo"));
        Display *display = open ? open(nullptr) : nullptr;
        if (!display || !root || !alloc || !query)
        {
            std::fprintf(stderr, "skipped: no X display or no MIT-SCREEN-SAVER\n");
            return skipped();
        }
        Info *info = alloc();
        auto idleMs = [&]
        {
            query(display, root(display), info);
            return info->idle;
        };
        std::this_thread::sleep_for(std::chrono::milliseconds(1500));
        const unsigned long before = idleMs();
        CHECK(before >= 1200); // nothing has touched the server: idle grows
        std::string error;
        const bool started = startX11Reset(1000, error);
        if (!started)
            std::fprintf(stderr, "startX11Reset: %s\n", error.c_str());
        CHECK(started);
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
        const unsigned long reset = idleMs();
        CHECK(reset < 600); // the first reset is at once
        unsigned long worst = 0;
        for (int i = 0; i < 25; ++i) // 2.5 s: two more periods
        {
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
            const unsigned long idle = idleMs();
            worst = idle > worst ? idle : worst;
        }
        CHECK(worst < 1600); // never much past one period
        std::fprintf(stderr, "idle: %lu ms before, %lu ms after the first reset, at most %lu ms over 2.5 s\n", before,
                     reset, worst);
        return 0;
    }
}

int main(int argc, char **argv)
{
    if (argc > 1 && std::strcmp(argv[1], "bus") == 0)
        return runBus();
    if (argc > 1 && std::strcmp(argv[1], "entry") == 0)
    {
        // entry [seconds]: the two calls must come back at once whatever the bus does (a stalled desktop included):
        // "entry-returned-ms N" says how long they took; then [seconds] (default 4) for the background thread.
        const auto start = std::chrono::steady_clock::now();
        ps2xHostKeepDisplayAwake();
        ps2xHostKeepDisplayAwake(); // once only: the second call asks nothing
        const auto took = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start);
        std::printf("entry-returned-ms %lld\n", static_cast<long long>(took.count()));
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::seconds(argc > 2 ? std::atoi(argv[2]) : 4));
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "entry-x11") == 0)
    {
        // The game's call on $DISPLAY (no session bus): its X11 half must reset the X server's idle time.
        const int code = runEntryX11();
        if (code != 0)
            return code;
        std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failures);
        return g_failures == 0 ? 0 : 1;
    }
    if (argc > 1 && std::strcmp(argv[1], "x11-lost") == 0)
    {
        // test/keep_awake_x11_lost.sh kills the X server after "ready"; the reset thread's next call finds the
        // connection gone. libX11's default would exit() here; the process must print "alive" and end with 0.
        void *x11 = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!x11 || !dlsym(x11, "XSetIOErrorExitHandler"))
        {
            std::printf("no-exit-handler\n");
            return 0;
        }
        std::string error;
        if (!startX11Reset(200, error))
        {
            std::printf("failed %s\n", error.c_str());
            return 1;
        }
        // "x11-lost other": a connection of the test's own too, standing for GLFW's. Its loss must still end the
        // process as libX11 always has (only the reset's own connection is spared).
        struct Display;
        Display *other = nullptr;
        const bool withOther = argc > 2 && std::strcmp(argv[2], "other") == 0;
        // A second start must change nothing. Run again, it would open a second connection and take over the IO
        // handler's "ours" (the first connection's loss then ends the process), or chain the handler to itself.
        if (!startX11Reset(200, error))
            return 1;
        if (withOther)
            other = reinterpret_cast<Display *(*)(const char *)>(dlsym(x11, "XOpenDisplay"))(nullptr);
        std::printf("ready\n");
        std::fflush(stdout);
        std::this_thread::sleep_for(std::chrono::milliseconds(2500)); // ~10 resets after the server is gone
        if (other)
        {
            reinterpret_cast<int (*)(Display *, int)>(dlsym(x11, "XSync"))(other, 0); // ends the process here
            std::printf("other-survived\n");
            return 3;
        }
        std::printf("alive\n");
        return 0;
    }
    if (argc > 1 && std::strcmp(argv[1], "wayland") == 0)
        return runWayland(argc > 2 ? std::atoi(argv[2]) : 4);
    if (argc > 1 && std::strcmp(argv[1], "missing-python") == 0)
    {
        std::fprintf(stderr, "skipped: CMake found no python3 for keep_awake_bus.py\n");
        return skipped();
    }
    if (argc > 1 && std::strcmp(argv[1], "x11") == 0)
    {
        const int code = runX11();
        if (code != 0)
            return code;
    }
    else
    {
        testOrder();
        testX11ResetOnce();
        testWaylandNone();
    }
    std::fprintf(stderr, "%d checks, %d failed\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
