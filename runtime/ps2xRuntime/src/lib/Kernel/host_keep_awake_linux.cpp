// Keeping the display awake on Linux (issue #12): runtime/keep_awake.h says what blanks a Linux screen and how each is
// stopped. Windows' counterpart is host_window_win32.cpp (SetThreadExecutionState).
#if defined(__linux__) && !defined(__ANDROID__)
#include "runtime/keep_awake.h"

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <dlfcn.h>
#include <sys/stat.h>
#include <mutex>
#include <thread>
#include <type_traits>

namespace ps2x::keepawake
{
    const char *name(Inhibitor inhibitor)
    {
        switch (inhibitor)
        {
        case Inhibitor::FreedesktopScreenSaver:
            return "org.freedesktop.ScreenSaver";
        case Inhibitor::GnomeSessionManager:
            return "org.gnome.SessionManager";
        case Inhibitor::None:
            break;
        }
        return "none";
    }

    std::vector<Call> calls()
    {
        return {
            {Inhibitor::FreedesktopScreenSaver, "org.freedesktop.ScreenSaver", "/org/freedesktop/ScreenSaver",
             "org.freedesktop.ScreenSaver", "Inhibit"},
            {Inhibitor::GnomeSessionManager, "org.gnome.SessionManager", "/org/gnome/SessionManager",
             "org.gnome.SessionManager", "Inhibit"},
        };
    }

    Result inhibitIdle(Bus &bus, const char *app, const char *reason)
    {
        Result result;
        for (const Call &call : calls())
        {
            uint32_t cookie = 0;
            std::string error;
            if (bus.call(call, app, reason, cookie, error))
            {
                result.by = call.inhibitor;
                result.cookie = cookie;
                return result;
            }
            if (!result.error.empty())
                result.error += "; ";
            result.error += std::string(name(call.inhibitor)) + ": " + (error.empty() ? "no reply" : error);
        }
        return result;
    }

    namespace
    {
        // libdbus-1's C API, the few calls used (dbus/dbus-*.h; the ABI has been stable since 1.0).
        struct DBusConnection;
        struct DBusMessage;
        struct DBusError // dbus-errors.h
        {
            const char *name;
            const char *message;
            unsigned int dummy1 : 1, dummy2 : 1, dummy3 : 1, dummy4 : 1, dummy5 : 1;
            void *padding1;
        };
        using dbus_bool_t = uint32_t;
        constexpr int kBusSession = 0;          // DBUS_BUS_SESSION
        constexpr int kTypeInvalid = 0;         // DBUS_TYPE_INVALID
        constexpr int kTypeString = 's';        // DBUS_TYPE_STRING
        constexpr int kTypeUint32 = 'u';        // DBUS_TYPE_UINT32
        constexpr int kReplyTimeoutMs = 3000;

        struct LibDbus
        {
            void (*error_init)(DBusError *) = nullptr;
            void (*error_free)(DBusError *) = nullptr;
            dbus_bool_t (*error_is_set)(const DBusError *) = nullptr;
            DBusConnection *(*bus_get_private)(int, DBusError *) = nullptr;
            void (*set_exit_on_disconnect)(DBusConnection *, dbus_bool_t) = nullptr;
            DBusMessage *(*new_method_call)(const char *, const char *, const char *, const char *) = nullptr;
            dbus_bool_t (*append_args)(DBusMessage *, int, ...) = nullptr;
            DBusMessage *(*send_with_reply_and_block)(DBusConnection *, DBusMessage *, int, DBusError *) = nullptr;
            dbus_bool_t (*get_args)(DBusMessage *, DBusError *, int, ...) = nullptr;
            void (*message_unref)(DBusMessage *) = nullptr;

            bool load(std::string &error)
            {
                void *lib = dlopen("libdbus-1.so.3", RTLD_NOW | RTLD_LOCAL);
                if (!lib)
                {
                    const char *why = dlerror();
                    error = std::string("libdbus-1.so.3 did not load (") + (why ? why : "?") + ")";
                    return false;
                }
                bool all = true;
                auto get = [&](auto &fn, const char *symbol)
                {
                    fn = reinterpret_cast<std::remove_reference_t<decltype(fn)>>(dlsym(lib, symbol));
                    all = all && fn != nullptr;
                };
                get(error_init, "dbus_error_init");
                get(error_free, "dbus_error_free");
                get(error_is_set, "dbus_error_is_set");
                get(bus_get_private, "dbus_bus_get_private");
                get(set_exit_on_disconnect, "dbus_connection_set_exit_on_disconnect");
                get(new_method_call, "dbus_message_new_method_call");
                get(append_args, "dbus_message_append_args");
                get(send_with_reply_and_block, "dbus_connection_send_with_reply_and_block");
                get(get_args, "dbus_message_get_args");
                get(message_unref, "dbus_message_unref");
                if (!all)
                    error = "libdbus-1.so.3 lacks a function it needs";
                return all;
            }
        };

        std::string describe(const DBusError &e)
        {
            return std::string(e.name ? e.name : "?") + (e.message ? std::string(" (") + e.message + ")" : "");
        }

        class SessionBus final : public Bus
        {
        public:
            SessionBus(const LibDbus &dbus, DBusConnection *connection) : m_dbus(dbus), m_connection(connection) {}

            bool call(const Call &call, const char *app, const char *reason, uint32_t &cookie,
                      std::string &error) override
            {
                DBusMessage *message = m_dbus.new_method_call(call.service, call.path, call.interface, call.method);
                if (!message)
                {
                    error = "out of memory";
                    return false;
                }
                bool built = false;
                if (call.inhibitor == Inhibitor::GnomeSessionManager)
                {
                    const uint32_t xid = 0, flags = kGnomeInhibitIdle;
                    built = m_dbus.append_args(message, kTypeString, &app, kTypeUint32, &xid, kTypeString, &reason,
                                               kTypeUint32, &flags, kTypeInvalid);
                }
                else
                    built = m_dbus.append_args(message, kTypeString, &app, kTypeString, &reason, kTypeInvalid);
                if (!built)
                {
                    m_dbus.message_unref(message);
                    error = "out of memory";
                    return false;
                }
                DBusError e;
                m_dbus.error_init(&e);
                DBusMessage *reply = m_dbus.send_with_reply_and_block(m_connection, message, kReplyTimeoutMs, &e);
                m_dbus.message_unref(message);
                if (!reply)
                {
                    error = m_dbus.error_is_set(&e) ? describe(e) : "no reply";
                    m_dbus.error_free(&e);
                    return false;
                }
                uint32_t value = 0;
                const bool read = m_dbus.get_args(reply, &e, kTypeUint32, &value, kTypeInvalid);
                m_dbus.message_unref(reply);
                if (!read)
                {
                    error = "the reply is not a cookie (" + (m_dbus.error_is_set(&e) ? describe(e) : "?") + ")";
                    m_dbus.error_free(&e);
                    return false;
                }
                cookie = value;
                return true;
            }

        private:
            const LibDbus &m_dbus;
            DBusConnection *m_connection;
        };
    }

    Bus *openSessionBus(std::string &error)
    {
        static LibDbus dbus;
        static std::string loadError;
        static const bool loaded = dbus.load(loadError);
        if (!loaded)
        {
            error = loadError;
            return nullptr;
        }
        const char *address = std::getenv("DBUS_SESSION_BUS_ADDRESS");
        if (!address || !*address)
        {
            // libdbus's own default when the address is unset ($XDG_RUNTIME_DIR/bus, systemd's user bus); without
            // that socket it would try X11 autolaunch (dbus-launch: the X display's bus if it has one, else one of
            // its own that no desktop hears).
            const char *runtime = std::getenv("XDG_RUNTIME_DIR");
            struct stat st{};
            if (!runtime || !*runtime || ::stat((std::string(runtime) + "/bus").c_str(), &st) != 0)
            {
                error = "no session bus ($DBUS_SESSION_BUS_ADDRESS unset, no $XDG_RUNTIME_DIR/bus)";
                return nullptr;
            }
        }
        DBusError e;
        dbus.error_init(&e);
        // A private connection: GHR's own, so no other library sharing the process's session connection (SDL's) can
        // close it, and its exit-on-disconnect is ours to turn off (keep_awake.h: never reached today). Untested: no
        // other library in the tests shares the process's bus connection.
        DBusConnection *connection = dbus.bus_get_private(kBusSession, &e);
        if (!connection)
        {
            error = "no session bus (" + (dbus.error_is_set(&e) ? describe(e) : std::string("?")) + ")";
            dbus.error_free(&e);
            return nullptr;
        }
        dbus.set_exit_on_disconnect(connection, 0u);
        return new SessionBus(dbus, connection); // kept, as the connection is: the inhibit lasts as long
    }

    namespace
    {
        bool startX11ResetOnce(int periodMs, std::string &error);
    }

    bool startX11Reset(int periodMs, std::string &error)
    {
        // Once per process: a second start would install the IO handler over itself (then calling itself forever).
        // Later calls give the first one's answer.
        static std::mutex mutex;
        static int state = 0; // 0 not tried, 1 running, 2 failed
        static std::string firstError;
        std::lock_guard<std::mutex> lock(mutex);
        if (state == 0)
        {
            state = startX11ResetOnce(periodMs, firstError) ? 1 : 2;
        }
        if (state == 2)
            error = firstError;
        return state == 1;
    }

    namespace
    {
    bool startX11ResetOnce(int periodMs, std::string &error)
    {
        struct Display;
        using OpenDisplay = Display *(*)(const char *);
        using DisplayCall = int (*)(Display *);
        void *lib = dlopen("libX11.so.6", RTLD_NOW | RTLD_LOCAL);
        if (!lib)
        {
            const char *why = dlerror();
            error = std::string("libX11.so.6 did not load (") + (why ? why : "?") + ")";
            return false;
        }
        const auto open = reinterpret_cast<OpenDisplay>(dlsym(lib, "XOpenDisplay"));
        const auto reset = reinterpret_cast<DisplayCall>(dlsym(lib, "XResetScreenSaver"));
        const auto flush = reinterpret_cast<DisplayCall>(dlsym(lib, "XFlush"));
        if (!open || !reset || !flush)
        {
            error = "libX11.so.6 lacks XOpenDisplay / XResetScreenSaver / XFlush";
            return false;
        }
        // A connection of our own, used by this thread alone (GLFW's and the renderer's stay theirs).
        Display *display = open(nullptr);
        if (!display)
        {
            error = "no X display ($DISPLAY)";
            return false;
        }
        // A lost connection (the X server or Xwayland gone): libX11's default ends the process from this thread. Its
        // per-connection exit handler (1.7+) lets the thread stop instead; the calls on a dead connection return at once.
        // Two steps, as libX11 runs them: the process-wide IO error handler first (its default prints "XIO: fatal IO
        // error" and exits), then the connection's exit handler. Ours returns for this connection only and hands every
        // other one (GLFW's, the renderer's) to the handler that was there before, which still ends the process.
        using ExitHandler = void (*)(Display *, void *);
        using SetExitHandler = void (*)(Display *, ExitHandler, void *);
        using IOHandler = int (*)(Display *);
        using SetIOHandler = IOHandler (*)(IOHandler);
        static std::atomic<bool> lost{false};
        static std::atomic<Display *> ours{nullptr};
        static std::atomic<IOHandler> previous{nullptr};
        const auto setExit = reinterpret_cast<SetExitHandler>(dlsym(lib, "XSetIOErrorExitHandler"));
        const auto setIO = reinterpret_cast<SetIOHandler>(dlsym(lib, "XSetIOErrorHandler"));
        if (setExit && setIO)
        {
            ours.store(display);
            setExit(display, [](Display *, void *) { lost.store(true); }, nullptr);
            // The handler there now, kept before ours goes in (an IO error on another thread meanwhile finds it). The
            // swap through nullptr leaves libX11's default (print and exit) in place for that instant: what any X
            // connection's loss did before, though a custom handler set earlier would miss that one instant.
            previous.store(setIO(nullptr));
            const IOHandler between = setIO([](Display *d) -> int
                  {
                      if (d == ours.load())
                      {
                          std::fprintf(stderr, "[window] the X11 screensaver reset's X connection is gone\n");
                          return 0; // on to the exit handler above: the thread stops
                      }
                      const IOHandler before = previous.load();
                      return before ? before(d) : 0;
                  });
            if (between) // libX11's default, or one another thread put in during the swap: chained either way
                previous.store(between);
        }
        reset(display);
        flush(display);
        try
        {
            std::thread([display, reset, flush, periodMs]
                        {
                            while (!lost.load())
                            {
                                std::this_thread::sleep_for(std::chrono::milliseconds(periodMs));
                                reset(display);
                                flush(display);
                            }
                            std::fprintf(stderr, "[window] X11 screensaver reset stopped: the X connection is gone\n");
                        })
                .detach();
        }
        catch (...)
        {
            error = "no thread for it (reset once only)";
            return false;
        }
        return true;
    }
    }
}

void ps2xHostKeepDisplayAwake() noexcept
{
    static std::atomic<bool> done{false};
    if (done.exchange(true))
        return;
    try
    {
        std::thread([]
                    {
                      try // nothing may leave this thread: an exception here would std::terminate the game
                      {
                        using namespace ps2x::keepawake;
                        // The X11 reset first: it is local and quick, where the session bus may hang without end (a
                        // bus that takes the connection and never answers: libdbus's connect has no timeout, only the
                        // calls after it have kReplyTimeoutMs). Then this thread may wait there; nothing else does.
                        std::string x11Error;
                        if (startX11Reset(kX11ResetPeriodMs, x11Error))
                            std::fprintf(stderr, "[window] X11 screensaver timer reset every %d s\n",
                                         kX11ResetPeriodMs / 1000);
                        else
                            std::fprintf(stderr, "[window] no X11 screensaver reset: %s\n", x11Error.c_str());
                        std::string error;
                        if (Bus *bus = openSessionBus(error))
                        {
                            const Result held = inhibitIdle(*bus, kApp, kReason);
                            if (held.by != Inhibitor::None)
                                std::fprintf(stderr, "[window] keeping the display awake: %s (cookie %u)\n",
                                             name(held.by), held.cookie);
                            else
                                std::fprintf(stderr, "[window] the desktop didn't take the screen-blank inhibit: %s\n",
                                             held.error.c_str());
                        }
                        else
                            std::fprintf(stderr, "[window] no screen-blank inhibit: %s\n", error.c_str());
                      }
                      catch (...)
                      {
                          std::fprintf(stderr, "[window] keeping the display awake stopped (out of memory)\n");
                      }
                    })
            .detach();
    }
    catch (...) // no thread to be had: the display may blank, the game goes on
    {
        std::fprintf(stderr, "[window] couldn't keep the display awake (no thread)\n");
    }
}
#endif
